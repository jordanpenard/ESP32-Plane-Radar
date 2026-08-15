#include "services/weather_time.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <sys/time.h>

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "services/adsb_client.h"
#include "services/display_settings.h"
#include "services/wifi_setup.h"

namespace services::weather {
namespace {

constexpr time_t kMinimumValidEpoch = 1609459200;  // 2021-01-01 UTC
// Even right after fully releasing BOTH of ADS-B's persistent TLS clients
// (see releasePersistentConnection()), real hardware logs repeatedly show
// `largest` permanently settle at ~32756 bytes for the rest of the boot
// session once ANY TLS connection has been used at all -- a ceiling that
// never reaches the previous 34000 threshold here, so weather always fell
// back to "WX N/A" every single cycle. This codebase's own history shows
// ~32KB was a working gate for ADS-B's own handshake at one point (see repo
// memory), so align to that same proven-reachable level rather than a
// theoretical one this hardware never actually hits post-TLS-use. A gate
// this close to the ceiling is still not a hard guarantee (mbedTLS can
// occasionally fail the actual allocation even when this passes), which is
// exactly why fetch() retries several times rather than relying on one shot.
constexpr uint32_t kMinHeapForSslBytes = 48000;
constexpr size_t kMinLargestBlockForSslBytes = 30000;

bool s_started = false;
bool s_valid = false;
bool s_stale = false;
float s_temperature_c = 0.0f;
int s_humidity_percent = 0;
int s_weather_code = -1;
int32_t s_utc_offset_seconds = 0;
unsigned long s_last_attempt_ms = 0;
unsigned long s_last_success_ms = 0;
int s_last_http_status = 0;
char s_last_error[32] = "none";
double s_last_latitude = 999.0;
double s_last_longitude = 999.0;
PollFn s_poll_fn = nullptr;
bool s_fetch_in_progress = false;

// Persistent across attempts/cycles for the same reason ADS-B's clients are
// (see adsb_client.cpp): a fresh WiFiClientSecure+HTTPClient pair every
// attempt forces a brand-new mbedTLS buffer alloc/free each time, which was
// the actual root cause of the ADS-B fragmentation ratchet ported here --
// weather retries up to 6x per cycle, so a stack-local client repeated that
// same churn every 3s within one cycle. Explicitly released at the end of
// fetch() (success or exhausted) so ADS-B gets its heap back between the
// infrequent (every ~15 min) cycles this runs.
WiFiClientSecure s_weather_client;
HTTPClient s_weather_http;

void setLastError(const char* message) {
  if (message == nullptr || message[0] == '\0') {
    message = "unknown";
  }
  snprintf(s_last_error, sizeof(s_last_error), "%s", message);
}

bool clockValid() { return time(nullptr) >= kMinimumValidEpoch; }

void pollNetwork() {
  esp_task_wdt_reset();
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

bool prepareSecureClient(WiFiClientSecure* client, const char* tag) {
  if (client == nullptr) {
    return false;
  }
  const uint32_t free_heap = ESP.getFreeHeap();
  const size_t largest_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  Serial.printf(
      "%s: TLS gate heap=%lu(need %lu) largest=%u(need %u)\n", tag,
      static_cast<unsigned long>(free_heap),
      static_cast<unsigned long>(kMinHeapForSslBytes),
      static_cast<unsigned>(largest_block),
      static_cast<unsigned>(kMinLargestBlockForSslBytes));
  if (free_heap < kMinHeapForSslBytes ||
      largest_block < kMinLargestBlockForSslBytes) {
    setLastError("low heap");
    Serial.printf("%s: skip TLS, low/fragmented heap=%lu, largest=%u\n", tag,
                  static_cast<unsigned long>(free_heap),
                  static_cast<unsigned>(largest_block));
    return false;
  }
  client->setInsecure();
  return true;
}

const char* conditionLabel(int code) {
  if (code == 0) return "CLEAR";
  if (code == 1) return "MOSTLY CLEAR";
  if (code == 2) return "PARTLY CLOUDY";
  if (code == 3) return "OVERCAST";
  if (code == 45 || code == 48) return "FOG";
  if (code >= 51 && code <= 57) return "DRIZZLE";
  if (code >= 61 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOW";
  if (code >= 80 && code <= 82) return "SHOWERS";
  if (code == 85 || code == 86) return "SNOW";
  if (code >= 95 && code <= 99) return "STORM";
  return "WEATHER";
}

const char* conditionToken(int code) {
  if (code == 0) return "SUNNY";
  if (code == 1) return "MOSTLY SUNNY";
  if (code == 2) return "PARTLY";
  if (code == 3) return "CLOUDY";
  if (code == 45 || code == 48) return "FOG";
  if (code >= 51 && code <= 57) return "DRIZZLE";
  if (code >= 61 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOW";
  if (code >= 80 && code <= 82) return "SHOWERS";
  if (code == 85 || code == 86) return "SNOW";
  if (code >= 95 && code <= 99) return "STORM";
  return "WX";
}

const char* conditionTrail(int code) {
  if (code == 0 || code == 1) return "CLEAR";
  if (code == 2 || code == 3) return "CLOUDY";
  if (code == 45 || code == 48) return "FOG";
  if (code >= 51 && code <= 57) return "DRIZZLE";
  if (code >= 61 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOW";
  if (code >= 80 && code <= 82) return "SHOWERS";
  if (code == 85 || code == 86) return "SNOW";
  if (code >= 95 && code <= 99) return "STORM";
  return "WX";
}

const char* conditionCompact(int code) {
  if (code == 0) return "SUN";
  if (code == 1) return "SUN";
  if (code == 2) return "PCLD";
  if (code == 3) return "CLDY";
  if (code == 45 || code == 48) return "FOG";
  if (code >= 51 && code <= 57) return "DRZL";
  if (code >= 61 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOW";
  if (code >= 80 && code <= 82) return "SHWR";
  if (code == 85 || code == 86) return "SNOW";
  if (code >= 95 && code <= 99) return "STRM";
  return "WX";
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<int64_t>(era) * 146097 +
         static_cast<int64_t>(day_of_era) - 719468;
}

void seedClockFromApiTime(const char* local_iso_time, int32_t utc_offset) {
  if (clockValid() || local_iso_time == nullptr) {
    return;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  if (sscanf(local_iso_time, "%d-%d-%dT%d:%d", &year, &month, &day, &hour,
             &minute) != 5) {
    return;
  }

  const time_t local_epoch = static_cast<time_t>(
      daysFromCivil(year, static_cast<unsigned>(month),
                    static_cast<unsigned>(day)) *
          86400 +
      hour * 3600 + minute * 60);
  if (local_epoch < kMinimumValidEpoch) {
    return;
  }

  timeval value = {};
  value.tv_sec = local_epoch - utc_offset;
  settimeofday(&value, nullptr);
}

bool fetchOnce(const String& url, int attempt, int max_attempts) {
  WiFiClientSecure& client = s_weather_client;
  // ADS-B keeps its own HTTPS connection alive between polls (to avoid
  // repeated TLS handshakes), which otherwise permanently reserves the one
  // large contiguous heap block this infrequent (every ~15 min) fetch also
  // needs. Release it first so the two never starve each other; ADS-B just
  // reconnects on its next normal poll cycle.
  services::adsb::releasePersistentConnection();
  // WiFiManager's always-on LAN config web portal (HTTP server + mDNS) is
  // the dominant permanent consumer of the largest contiguous heap block --
  // far bigger than anything ADS-B holds. wifiLoop() restarts it on its very
  // next call, so pausing it here just for this attempt is nearly free.
  wifiPauseLanPortal();
  if (!prepareSecureClient(&client, "weather")) {
    s_last_http_status = 0;
    return false;
  }

  HTTPClient& http = s_weather_http;
  if (!http.begin(client, url)) {
    s_last_http_status = 0;
    setLastError("http.begin failed");
    Serial.println("weather: http.begin failed");
    return false;
  }
  http.setReuse(true);
  http.setConnectTimeout(config::kWeatherRequestTimeoutMs);
  http.setTimeout(config::kWeatherRequestTimeoutMs);

  pollNetwork();
  const int code = http.GET();
  pollNetwork();
  if (code != HTTP_CODE_OK) {
    s_last_http_status = code;
    setLastError("http error");
    Serial.printf("weather: HTTP %d (attempt %d/%d)\n", code, attempt,
                  max_attempts);
    http.end();
    client.stop();
    return false;
  }
  s_last_http_status = code;

  const String payload = http.getString();
  http.end();
  pollNetwork();

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    setLastError("json parse error");
    Serial.printf("weather: JSON parse error: %s\n", error.c_str());
    return false;
  }

  JsonObject current = doc["current"].as<JsonObject>();
  if (current.isNull() || !current["temperature_2m"].is<float>() ||
      !current["relative_humidity_2m"].is<int>() ||
      !current["weather_code"].is<int>()) {
    setLastError("incomplete response");
    Serial.println("weather: incomplete response");
    return false;
  }

  s_temperature_c = current["temperature_2m"].as<float>();
  s_humidity_percent = current["relative_humidity_2m"].as<int>();
  s_weather_code = current["weather_code"].as<int>();
  s_utc_offset_seconds = doc["utc_offset_seconds"] | 0;
  seedClockFromApiTime(current["time"] | nullptr, s_utc_offset_seconds);
  s_valid = true;
  s_stale = false;
  s_last_success_ms = millis();
  setLastError("none");
  Serial.printf("weather: %.1f C, %d%%, code %d, UTC%+ld\n", s_temperature_c,
                s_humidity_percent, s_weather_code,
                static_cast<long>(s_utc_offset_seconds));
  return true;
}

bool fetch(double latitude, double longitude) {
  // Skips the poll callback's radar redraw for this call's duration (see
  // main.cpp's onNetworkPoll() and adsb_client.cpp's matching guard) -- a
  // competing redraw during weather's own TLS handshake/retries can slow it
  // enough to tip an already-marginal handshake into failure.
  struct FetchInProgressGuard {
    FetchInProgressGuard() { s_fetch_in_progress = true; }
    ~FetchInProgressGuard() { s_fetch_in_progress = false; }
  } fetch_guard;

  String url = config::kWeatherApiBase;
  url += "?latitude=";
  url += String(latitude, 6);
  url += "&longitude=";
  url += String(longitude, 6);
  url +=
      "&current=temperature_2m,relative_humidity_2m,weather_code,is_day"
      "&temperature_unit=celsius&timezone=auto&forecast_days=1";

  // A single attempt can lose a marginal TLS handshake race: mbedTLS's
  // transient allocation needs during a handshake can fail even when the
  // heap gate (a snapshot of the largest free block) passed a moment
  // earlier -- confirmed on real hardware, where ADS-B's own low-largest
  // probe succeeds only intermittently at the exact same reading that just
  // failed here. This isn't something a fixed threshold can fully predict,
  // so retry several times, spread over a longer window (mirroring how
  // adsb_client.cpp's own probe/retry loop needs ~15-30s to land a good
  // moment) rather than a tight, fast burst -- weather only runs once every
  // kWeatherFetchIntervalMs, so the extra latency here is free.
  constexpr int kMaxAttempts = 6;
  constexpr unsigned long kRetryDelayMs = 3000UL;
  constexpr unsigned long kRetryPollSliceMs = 100UL;
  bool succeeded = false;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    Serial.printf("weather: fetch attempt %d/%d\n", attempt, kMaxAttempts);
    if (fetchOnce(url, attempt, kMaxAttempts)) {
      succeeded = true;
      break;
    }
    if (attempt < kMaxAttempts) {
      for (unsigned long waited = 0; waited < kRetryDelayMs;
           waited += kRetryPollSliceMs) {
        pollNetwork();
        delay(kRetryPollSliceMs);
      }
    }
  }
  // Give the heap back to ADS-B for the ~15 min until the next cycle instead
  // of holding this connection open indefinitely (see s_weather_client).
  s_weather_client.stop();
  return succeeded;
}

}  // namespace

void begin() {
  if (!s_started) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    s_started = true;
  }
}

void setPollFn(PollFn fn) { s_poll_fn = fn; }

bool fetchInProgress() { return s_fetch_in_progress; }

bool refreshIfDue(double latitude, double longitude, bool force) {
  begin();
  const unsigned long now = millis();
  const bool location_changed =
      fabs(latitude - s_last_latitude) > 0.0001 ||
      fabs(longitude - s_last_longitude) > 0.0001;
  if (!force && !location_changed && s_last_attempt_ms != 0 &&
      now - s_last_attempt_ms < config::kWeatherFetchIntervalMs) {
    return false;
  }
  s_last_attempt_ms = now;
  s_last_latitude = latitude;
  s_last_longitude = longitude;
  if (settings::weatherEnabled() && fetch(latitude, longitude)) {
    return true;
  }

  // Keep last successful weather visible when a refresh fails.
  s_stale = s_valid;
  return false;
}

bool valid() { return s_valid; }

bool stale() { return s_stale; }

int lastHttpStatus() { return s_last_http_status; }

const char* lastError() { return s_last_error; }

unsigned long lastSuccessAgeSec() {
  if (s_last_success_ms == 0) {
    return 0;
  }
  return (millis() - s_last_success_ms) / 1000UL;
}

void formatWeatherLine(char* out, size_t out_len, int max_width) {
  if (out_len == 0) {
    return;
  }
  if (!s_valid) {
    snprintf(out, out_len, "WX N/A");
    return;
  }

  float temperature = s_temperature_c;
  const char unit =
      settings::temperatureFahrenheit() ? 'F' : 'C';
  if (settings::temperatureFahrenheit()) {
    temperature = temperature * 9.0f / 5.0f + 32.0f;
  }

  const char* token = conditionToken(s_weather_code);
  const char* compact = conditionCompact(s_weather_code);
  const long rounded_temperature = static_cast<long>(lroundf(temperature));

  if (max_width <= 148) {
    snprintf(out, out_len, "%s %ld%c %d%%%s", compact, rounded_temperature,
             unit, s_humidity_percent, s_stale ? " STALE" : "");
    return;
  }

  if (max_width <= 176) {
    snprintf(out, out_len, "%s %ld%c %d%%%s", token, rounded_temperature,
             unit, s_humidity_percent, s_stale ? " STALE" : "");
    return;
  }

  snprintf(out, out_len, "%s %ld%c %d%% %s%s", token, rounded_temperature,
           unit, s_humidity_percent, conditionTrail(s_weather_code),
           s_stale ? " STALE" : "");
}

void formatLocalDateTime(time_t utc_time, bool include_seconds, char* out,
                         size_t out_len) {
  const time_t local_now = utc_time + s_utc_offset_seconds;
  tm local = {};
  gmtime_r(&local_now, &local);
  const int year = local.tm_year + 1900;
  const int month = local.tm_mon + 1;
  const int day = local.tm_mday;
  if (include_seconds) {
    snprintf(out, out_len, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day,
             local.tm_hour, local.tm_min, local.tm_sec);
    return;
  }
  snprintf(out, out_len, "%04d-%02d-%02d %02d:%02d", year, month, day,
           local.tm_hour, local.tm_min);
}

void formatDateTimeLine(char* out, size_t out_len, bool include_seconds,
                        unsigned long display_delay_ms) {
  if (out_len == 0) {
    return;
  }

  const time_t utc_now = time(nullptr);
  if (utc_now < kMinimumValidEpoch) {
    snprintf(out, out_len, include_seconds ? "---- -- --:--:--"
                                           : "---- -- --:--");
    return;
  }

  const time_t delayed_utc_now = utc_now -
                                 static_cast<time_t>(display_delay_ms / 1000UL);
  formatLocalDateTime(delayed_utc_now, include_seconds, out, out_len);
}

bool hasLastFix() { return s_last_success_ms != 0; }

void formatDateTimeAtMillis(unsigned long event_ms, char* out, size_t out_len,
                           bool include_seconds) {
  if (out_len == 0) {
    return;
  }

  const time_t utc_now = time(nullptr);
  if (utc_now < kMinimumValidEpoch || event_ms == 0) {
    snprintf(out, out_len, include_seconds ? "---- -- --:--:--"
                                           : "---- -- --:--");
    return;
  }

  const unsigned long age_sec = (millis() - event_ms) / 1000UL;
  const time_t event_utc = utc_now - static_cast<time_t>(age_sec);
  formatLocalDateTime(event_utc, include_seconds, out, out_len);
}

void formatLastFixDateTimeLine(char* out, size_t out_len,
                               bool include_seconds) {
  formatDateTimeAtMillis(s_last_success_ms, out, out_len, include_seconds);
}

int currentLocalHour() {
  const time_t utc_now = time(nullptr);
  if (utc_now < kMinimumValidEpoch) {
    return -1;
  }
  const time_t local_now = utc_now + s_utc_offset_seconds;
  tm local = {};
  gmtime_r(&local_now, &local);
  return local.tm_hour;
}

}  // namespace services::weather
