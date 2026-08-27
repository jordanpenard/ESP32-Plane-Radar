#include "services/adsb_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cctype>
#include <cfloat>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "services/display_settings.h"
#include "services/ota_update.h"
#include "services/unit_policy.h"
#include "ui/radar_range.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr float kMetersPerFoot = 0.3048f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kAdsbRequestTimeoutMs = 10000;
// Real hardware logs (strong RSSI, ruling out a weak WiFi link) showed the
// response body genuinely trickling in at only ~650 B/s under degraded
// conditions -- an ~11-12KB body needs ~18s at that rate. A separate, more
// generous budget for just the body-read phase lets a merely-slow transfer
// finish instead of being truncated and forcing an expensive reconnect.
constexpr unsigned long kAdsbBodyReadTimeoutMs = 20000;
constexpr size_t kEnrichmentCacheSize = 48;
constexpr uint32_t kMinHeapForSslBytes = 54500;
constexpr uint32_t kMinHeapForLookupSslBytes = 62000;
constexpr size_t kMinLargestBlockForAdsbSslBytes = 34500;
constexpr size_t kMinLargestBlockForLookupSslBytes = 40000;
constexpr unsigned long kAdsbLowLargestProbeIntervalMs = 15000UL;
constexpr unsigned long kAdsbLowHeapProbeIntervalMs = 12000UL;
constexpr uint32_t kAdsbLowHeapProbeMarginBytes = 1800;
constexpr uint32_t kMinHeapForLookupWorkBytes = 62000;
constexpr size_t kMinLargestBlockForLookupWorkBytes = 40000;
constexpr unsigned long kTlsSkipLogIntervalMs = 10000UL;
constexpr unsigned long kAdsbTlsLowHeapBackoffMs = 5000UL;
constexpr unsigned long kAdsbTlsFailureBackoffBaseMs = 15000UL;
constexpr unsigned long kAdsbTlsFailureBackoffMaxMs = 60000UL;
// adsb.fi's public endpoints are rate-limited to 1 request/second; retrying
// instantly after a stalled/incomplete read (which already burned most of
// the request timeout) would otherwise reconnect far faster than that.
constexpr unsigned long kAdsbIncompleteResponseBackoffMs = 5000UL;
constexpr unsigned long kAdsbDiagIntervalMs = 30000UL;
// Sanity cap on HTTP response bodies. A legitimate ADS-B/lookup JSON
// response never gets close to this size; a Content-Length far beyond it
// almost always means the HTTP framing got desynchronized (e.g. stale bytes
// left over from a reused-but-not-fully-drained connection), not a real
// payload. Treat anything past this as corrupt and refuse to trust it.
constexpr size_t kMaxResponseBodyBytes = 98304;

unsigned long s_last_tls_skip_log_ms = 0;
unsigned long s_last_adsb_diag_log_ms = 0;
uint32_t s_adsb_tls_skip_count = 0;
uint32_t s_lookup_tls_skip_count = 0;
uint32_t s_adsb_skip_low_heap_only_count = 0;
uint32_t s_adsb_skip_low_largest_only_count = 0;
uint32_t s_adsb_skip_low_both_count = 0;
uint32_t s_lookup_skip_low_heap_only_count = 0;
uint32_t s_lookup_skip_low_largest_only_count = 0;
uint32_t s_lookup_skip_low_both_count = 0;
uint32_t s_lookup_connect_fail_count = 0;
uint32_t s_adsb_connect_fail_count = 0;
uint8_t s_adsb_connect_fail_streak = 0;
uint16_t s_critical_largest_block_streak = 0;
unsigned long s_adsb_tls_cooldown_until_ms = 0;
unsigned long s_last_adsb_low_largest_probe_ms = 0;
unsigned long s_last_adsb_low_heap_probe_ms = 0;
uint32_t s_adsb_low_largest_probe_count = 0;
uint32_t s_adsb_low_largest_probe_success_count = 0;
uint32_t s_adsb_low_heap_probe_count = 0;
uint32_t s_adsb_low_heap_probe_success_count = 0;

// Tracks whether the current "adsb" TLS attempt was gated through one of
// the opportunistic probes below, so success can be attributed correctly
// (previously this was inferred from an unrelated post-fetch memory
// snapshot, which did not actually reflect whether the probed attempt
// succeeded).
bool s_adsb_probe_active_largest = false;
bool s_adsb_probe_active_heap = false;

// Visibility into whether the persistent-connection keep-alive is actually
// avoiding new TLS handshakes (each of which needs a large contiguous heap
// block and is the main source of the observed fragmentation ratchet).
uint32_t s_adsb_tls_reuse_count = 0;
uint32_t s_adsb_tls_handshake_count = 0;

bool s_diag_window_has_sample = false;
uint32_t s_diag_window_min_heap = 0;
uint32_t s_diag_window_max_heap = 0;
size_t s_diag_window_min_largest = 0;
size_t s_diag_window_max_largest = 0;
uint32_t s_diag_window_min_internal_heap = 0;
uint32_t s_diag_window_max_internal_heap = 0;
size_t s_diag_window_min_internal_largest = 0;
size_t s_diag_window_max_internal_largest = 0;

void sampleDiagWindow(uint32_t free_heap, size_t largest_block) {
  const uint32_t internal_heap =
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_largest =
    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!s_diag_window_has_sample) {
  s_diag_window_has_sample = true;
  s_diag_window_min_heap = free_heap;
  s_diag_window_max_heap = free_heap;
  s_diag_window_min_largest = largest_block;
  s_diag_window_max_largest = largest_block;
  s_diag_window_min_internal_heap = internal_heap;
  s_diag_window_max_internal_heap = internal_heap;
  s_diag_window_min_internal_largest = internal_largest;
  s_diag_window_max_internal_largest = internal_largest;
  return;
  }

  s_diag_window_min_heap = std::min(s_diag_window_min_heap, free_heap);
  s_diag_window_max_heap = std::max(s_diag_window_max_heap, free_heap);
  s_diag_window_min_largest =
    std::min(s_diag_window_min_largest, largest_block);
  s_diag_window_max_largest =
    std::max(s_diag_window_max_largest, largest_block);
  s_diag_window_min_internal_heap =
    std::min(s_diag_window_min_internal_heap, internal_heap);
  s_diag_window_max_internal_heap =
    std::max(s_diag_window_max_internal_heap, internal_heap);
  s_diag_window_min_internal_largest =
    std::min(s_diag_window_min_internal_largest, internal_largest);
  s_diag_window_max_internal_largest =
    std::max(s_diag_window_max_internal_largest, internal_largest);
}

void resetDiagWindow() { s_diag_window_has_sample = false; }

bool prepareSecureClient(WiFiClientSecure* client, const char* tag,
                         uint32_t min_heap_bytes,
                         size_t min_largest_block) {
  if (client == nullptr) {
    return false;
  }
  if (strcmp(tag, "adsb") == 0) {
    s_adsb_probe_active_largest = false;
    s_adsb_probe_active_heap = false;
  }
  if (client->connected()) {
    // Reusing an already-established (kept-alive) TLS session: no new
    // handshake means no new large contiguous allocation, so the
    // fragmentation-aware admission check below does not apply.
    if (strcmp(tag, "adsb") == 0) {
      ++s_adsb_tls_reuse_count;
    }
    return true;
  }
  const unsigned long now = millis();
  const uint32_t free_heap = ESP.getFreeHeap();
  const size_t largest_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  sampleDiagWindow(free_heap, largest_block);

  const bool low_heap = free_heap < min_heap_bytes;
  const bool low_largest = largest_block < min_largest_block;

  bool allow_adsb_probe = false;
  if (strcmp(tag, "adsb") == 0 && !low_heap && low_largest) {
    const unsigned long elapsed = now - s_last_adsb_low_largest_probe_ms;
    if (s_last_adsb_low_largest_probe_ms == 0 ||
        elapsed >= kAdsbLowLargestProbeIntervalMs) {
      allow_adsb_probe = true;
      s_adsb_probe_active_largest = true;
      s_last_adsb_low_largest_probe_ms = now;
      ++s_adsb_low_largest_probe_count;
      Serial.printf("adsb: low-largest probe heap=%lu largest=%u\n",
                    static_cast<unsigned long>(free_heap),
                    static_cast<unsigned>(largest_block));
    }
  }
  if (strcmp(tag, "adsb") == 0 && low_heap && !low_largest &&
      free_heap + kAdsbLowHeapProbeMarginBytes >= min_heap_bytes) {
    const unsigned long elapsed = now - s_last_adsb_low_heap_probe_ms;
    if (s_last_adsb_low_heap_probe_ms == 0 ||
        elapsed >= kAdsbLowHeapProbeIntervalMs) {
      allow_adsb_probe = true;
      s_adsb_probe_active_heap = true;
      s_last_adsb_low_heap_probe_ms = now;
      ++s_adsb_low_heap_probe_count;
      Serial.printf("adsb: low-heap probe heap=%lu largest=%u\n",
                    static_cast<unsigned long>(free_heap),
                    static_cast<unsigned>(largest_block));
    }
  }

  if (low_heap || low_largest) {
    if (!allow_adsb_probe) {
      if (strcmp(tag, "adsb") == 0) {
        ++s_adsb_tls_skip_count;
        if (low_heap && low_largest) {
          ++s_adsb_skip_low_both_count;
        } else if (low_heap) {
          ++s_adsb_skip_low_heap_only_count;
        } else {
          ++s_adsb_skip_low_largest_only_count;
        }
      } else if (strcmp(tag, "flight data") == 0) {
        ++s_lookup_tls_skip_count;
        if (low_heap && low_largest) {
          ++s_lookup_skip_low_both_count;
        } else if (low_heap) {
          ++s_lookup_skip_low_heap_only_count;
        } else {
          ++s_lookup_skip_low_largest_only_count;
        }
      }

      if (s_last_tls_skip_log_ms == 0 ||
          now - s_last_tls_skip_log_ms >= kTlsSkipLogIntervalMs) {
        Serial.printf("%s: skip TLS, low/fragmented heap=%lu, largest=%u\n", tag,
                      static_cast<unsigned long>(free_heap),
                      static_cast<unsigned>(largest_block));
        s_last_tls_skip_log_ms = now;
      }
      return false;
    }
  }
  if (strcmp(tag, "adsb") == 0) {
    ++s_adsb_tls_handshake_count;
  }
  client->setInsecure();
  return true;
}

// Defined further below alongside the other persistent TLS clients; needed
// here to tell "low largest because ADS-B is actively using it" (fine) apart
// from "low largest and nothing is even connected" (genuinely fragmented).
extern WiFiClientSecure s_adsb_client;

void maybeLogAdsbDiagnostics() {
  const unsigned long now = millis();
  if (s_last_adsb_diag_log_ms != 0 &&
      now - s_last_adsb_diag_log_ms < kAdsbDiagIntervalMs) {
    return;
  }
  s_last_adsb_diag_log_ms = now;

  const uint32_t free_heap = ESP.getFreeHeap();
  const size_t largest_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const uint32_t internal_heap =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  sampleDiagWindow(free_heap, largest_block);

  unsigned long cooldown_remaining_ms = 0;
  if (s_adsb_tls_cooldown_until_ms > now) {
    cooldown_remaining_ms = s_adsb_tls_cooldown_until_ms - now;
  }

  Serial.printf(
      "adsb diag: heap=%lu largest=%u iheap=%lu ilargest=%u "
      "win{heap=%lu..%lu largest=%u..%u iheap=%lu..%lu ilargest=%u..%u} "
      "thr{adsb_heap=%lu adsb_largest=%u lookup_heap=%lu lookup_largest=%u} "
      "probe{largest=%lu/%lu heap=%lu/%lu} "
      "tls_skip{adsb=%lu[h=%lu l=%lu b=%lu],lookup=%lu[h=%lu l=%lu b=%lu]} "
      "conn_fail{adsb=%lu,lookup=%lu} cool_ms=%lu streak=%u "
      "tls_conn{reuse=%lu,new=%lu} rssi=%d\n",
      static_cast<unsigned long>(free_heap),
      static_cast<unsigned>(largest_block),
      static_cast<unsigned long>(internal_heap),
      static_cast<unsigned>(internal_largest),
      static_cast<unsigned long>(s_diag_window_min_heap),
      static_cast<unsigned long>(s_diag_window_max_heap),
      static_cast<unsigned>(s_diag_window_min_largest),
      static_cast<unsigned>(s_diag_window_max_largest),
      static_cast<unsigned long>(s_diag_window_min_internal_heap),
      static_cast<unsigned long>(s_diag_window_max_internal_heap),
      static_cast<unsigned>(s_diag_window_min_internal_largest),
      static_cast<unsigned>(s_diag_window_max_internal_largest),
      static_cast<unsigned long>(kMinHeapForSslBytes),
      static_cast<unsigned>(kMinLargestBlockForAdsbSslBytes),
      static_cast<unsigned long>(kMinHeapForLookupSslBytes),
      static_cast<unsigned>(kMinLargestBlockForLookupSslBytes),
      static_cast<unsigned long>(s_adsb_low_largest_probe_count),
      static_cast<unsigned long>(s_adsb_low_largest_probe_success_count),
      static_cast<unsigned long>(s_adsb_low_heap_probe_count),
      static_cast<unsigned long>(s_adsb_low_heap_probe_success_count),
      static_cast<unsigned long>(s_adsb_tls_skip_count),
      static_cast<unsigned long>(s_adsb_skip_low_heap_only_count),
      static_cast<unsigned long>(s_adsb_skip_low_largest_only_count),
      static_cast<unsigned long>(s_adsb_skip_low_both_count),
      static_cast<unsigned long>(s_lookup_tls_skip_count),
      static_cast<unsigned long>(s_lookup_skip_low_heap_only_count),
      static_cast<unsigned long>(s_lookup_skip_low_largest_only_count),
      static_cast<unsigned long>(s_lookup_skip_low_both_count),
      static_cast<unsigned long>(s_adsb_connect_fail_count),
      static_cast<unsigned long>(s_lookup_connect_fail_count),
      cooldown_remaining_ms, static_cast<unsigned>(s_adsb_connect_fail_streak),
      static_cast<unsigned long>(s_adsb_tls_reuse_count),
      static_cast<unsigned long>(s_adsb_tls_handshake_count), WiFi.RSSI());

  // Last-resort self-heal: the largest free block has been below the
  // critical threshold for many consecutive polls — every TLS handshake
  // is failing anyway, so reboot rather than keep failing forever. Only
  // count this while ADS-B itself has no active connection: a low value
  // while it's connected and successfully fetching just reflects that
  // connection's own buffers legitimately in use, not wasted fragmentation.
  if (largest_block < config::kCriticalLargestFreeBlockBytes &&
      !s_adsb_client.connected()) {
    ++s_critical_largest_block_streak;
  } else {
    s_critical_largest_block_streak = 0;
  }
  // Second self-heal trigger: real hardware has shown -32512 handshake
  // failures persisting indefinitely while largest_block sits well above
  // kCriticalLargestFreeBlockBytes (stuck at ~32-33KB, never dipping below
  // 20000) -- the heap-size gate alone can miss this. Key this one off the
  // actual observed failure streak instead of a memory-size proxy.
  const bool connect_fail_critical =
      s_adsb_connect_fail_streak >= config::kCriticalAdsbConnectFailStreakLimit;
  if (s_critical_largest_block_streak >=
          config::kCriticalLargestBlockStreakLimit ||
      connect_fail_critical) {
    if (services::ota::inProgress()) {
      Serial.println(
          "adsb diag: critical heap fragmentation but OTA in progress — "
          "deferring restart");
    } else {
      Serial.printf(
          "adsb diag: critical heap fragmentation (largest_streak=%u "
          "connect_fail_streak=%u largest=%u) — restarting\n",
          static_cast<unsigned>(s_critical_largest_block_streak),
          static_cast<unsigned>(s_adsb_connect_fail_streak),
          static_cast<unsigned>(largest_block));
      delay(200);
      esp_restart();
    }
  }

  resetDiagWindow();
}

Aircraft s_aircraft[kMaxAircraft];
Aircraft s_previous_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;
bool s_fetch_in_progress = false;
unsigned long s_last_fetch_update_ms = 0;
unsigned long s_last_adsb_tls_skip_ms = 0;
unsigned long s_last_enrichment_lookup_ms = 0;
unsigned long s_last_enrichment_failure_ms = 0;
double s_last_center_lat = 0.0;
double s_last_center_lon = 0.0;

struct EnrichmentCacheEntry {
  char hex[7] = {};
  char callsign[9] = {};
  char route[10] = {};
  char type[18] = {};
  unsigned long refreshed_ms = 0;
  bool has_data = false;
};

EnrichmentCacheEntry s_enrichment_cache[kEnrichmentCacheSize];

// Persistent TLS clients, reused across fetch cycles instead of being
// constructed/destroyed on every call. WiFiClientSecure's destructor tears
// down the mbedTLS context (and its ~16-34KB RX/TX buffers), so recreating
// it every 3s was the actual root cause of the heap fragmentation ratchet:
// each cycle freed and re-allocated a large contiguous block, and the
// allocator could not always reclaim the exact same region. Keeping the
// client (and the HTTPClient that owns it) alive lets HTTP keep-alive avoid
// a brand-new TLS handshake most of the time, so the big buffers stay put.
WiFiClientSecure s_adsb_client;
HTTPClient s_adsb_http;
WiFiClientSecure s_lookup_client;
HTTPClient s_lookup_http;

void pollNetwork() {
  esp_task_wdt_reset();
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http, unsigned long timeout_ms,
                       int max_attempts) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long started_ms = millis();
  int attempts = 0;
  while (millis() - started_ms < timeout_ms) {
    ++attempts;
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    // Any GET() failure (timeout, connection lost, refused, etc.) leaves the
    // underlying TCP/TLS session in a state that must not be trusted for
    // reuse on the next cycle -- force a hard close now. Since the client is
    // now a persistent, kept-alive object (not recreated every cycle),
    // leaving a half-open/broken socket here would otherwise make the next
    // fetch silently try to "reuse" a dead connection, or leave it stuck
    // fragmenting the heap indefinitely.
    WiFiClient* stream = http.getStreamPtr();
    if (stream != nullptr) {
      stream->stop();
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    if (attempts >= max_attempts) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

bool readResponseBodyWithPoll(HTTPClient& http, String& payload,
                              unsigned long timeout_ms, bool* out_complete,
                              const char* tag) {
  *out_complete = false;
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  const int content_length = http.getSize();
  if (content_length > static_cast<int>(kMaxResponseBodyBytes)) {
    Serial.printf("http: implausible content-length %d, dropping\n",
                  content_length);
    return false;
  }
  if (content_length > 0) {
    payload.reserve(static_cast<unsigned>(content_length + 1));
  }

  uint8_t buffer[512];
  const unsigned long started_ms = millis();
  bool timed_out = true;
  while (millis() - started_ms < timeout_ms) {
    pollNetwork();
    const int available = stream->available();
    if (available > 0) {
      const int to_read =
          available > static_cast<int>(sizeof(buffer)) ? static_cast<int>(sizeof(buffer))
                                                       : available;
      const int read_bytes = stream->readBytes(buffer, to_read);
      if (read_bytes > 0) {
        payload.concat(reinterpret_cast<const char*>(buffer),
                       static_cast<unsigned>(read_bytes));
        if (payload.length() > kMaxResponseBodyBytes) {
          Serial.println("http: response body exceeded sanity cap");
          return false;
        }
      }
    }
    if (content_length > 0 &&
        static_cast<int>(payload.length()) >= content_length) {
      *out_complete = true;
      timed_out = false;
      break;
    }
    if (!http.connected() && stream->available() <= 0) {
      timed_out = false;
      break;
    }
    delay(1);
  }

  if (content_length <= 0) {
    // Length wasn't known up front (missing header/chunked transfer): only
    // ever consider it "complete" once the peer has actually closed, since
    // there is no other way to know every byte arrived. On a kept-alive
    // connection this correctly means "don't trust it, close and retry
    // fresh" rather than risking a truncated/garbled payload.
    *out_complete = payload.length() > 0 && !http.connected();
  }

  if (!*out_complete && payload.length() > 0) {
    // Temporary diagnostics (TWENTY-FIRST/TWENTY-SECOND issue investigation):
    // pin down whether "incomplete response" is a real timeout, a
    // missing/mismatched Content-Length, or the peer closing mid-body, and
    // whether it correlates with a weak WiFi signal (adsb.fi is a remote
    // WAN endpoint, not LAN, so link quality to the router matters here).
    Serial.printf(
        "%s: incomplete detail len=%u content_length=%d connected=%d "
        "timed_out=%d elapsed_ms=%lu rssi=%d\n",
        tag, static_cast<unsigned>(payload.length()), content_length,
        http.connected() ? 1 : 0, timed_out ? 1 : 0,
        static_cast<unsigned long>(millis() - started_ms), WiFi.RSSI());
  }

  return payload.length() > 0;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

float pickVerticalRateFpm(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "baro_rate", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "geom_rate", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

bool sameAircraftIdentity(const Aircraft& a, const Aircraft& b) {
  if (a.hex[0] != '\0' && b.hex[0] != '\0' && strcmp(a.hex, b.hex) == 0) {
    return true;
  }
  return a.callsign[0] != '\0' && b.callsign[0] != '\0' &&
         strcmp(a.callsign, b.callsign) == 0;
}

const Aircraft* findPreviousAircraftSample(const Aircraft* previous,
                                           size_t previous_count,
                                           const Aircraft& current) {
  for (size_t i = 0; i < previous_count; ++i) {
    if (sameAircraftIdentity(previous[i], current)) {
      return &previous[i];
    }
  }
  return nullptr;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    alt += services::settings::altitudeOffsetFeet();
    if (services::units::useImperialDistance()) {
      snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
      return;
    }
    snprintf(out, out_len, "%d m",
             static_cast<int>(lroundf(alt * kMetersPerFoot)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  ac->has_prev_sample = false;
  ac->prev_lat = ac->lat;
  ac->prev_lon = ac->lon;
  ac->prev_altitude_ft = ac->altitude_ft;
  ac->prev_has_altitude = false;
  ac->prev_on_ground = ac->on_ground;

  ac->on_ground = isOnGround(plane);
  ac->has_altitude = false;
  ac->altitude_ft = 0.0f;
  ac->vertical_rate_fpm = pickVerticalRateFpm(plane);

  if (!ac->on_ground) {
    float alt = 0.0f;
    if (readJsonFloat(plane, "alt_baro", &alt) ||
        readJsonFloat(plane, "alt_geom", &alt)) {
      ac->altitude_ft = alt;
      ac->has_altitude = true;
    }
  }

  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  ac->route[0] = '\0';
  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

bool sameKey(const EnrichmentCacheEntry& entry, const Aircraft& plane) {
  return entry.refreshed_ms != 0 && strcmp(entry.hex, plane.hex) == 0 &&
         strcmp(entry.callsign, plane.callsign) == 0;
}

bool cacheEntryFresh(const EnrichmentCacheEntry& entry,
                     unsigned long now) {
  if (entry.refreshed_ms == 0) {
    return false;
  }
  const unsigned long ttl = entry.has_data ? config::kFlightCacheSuccessMs
                                           : config::kFlightCacheMissMs;
  return now - entry.refreshed_ms < ttl;
}

EnrichmentCacheEntry* findCacheEntry(const Aircraft& plane,
                                     unsigned long now) {
  for (auto& entry : s_enrichment_cache) {
    if (sameKey(entry, plane) && cacheEntryFresh(entry, now)) {
      return &entry;
    }
  }
  return nullptr;
}

bool copyIfDifferent(char* destination, size_t destination_len,
                     const char* source) {
  if (source == nullptr || source[0] == '\0' ||
      strcmp(destination, source) == 0) {
    return false;
  }
  strncpy(destination, source, destination_len - 1);
  destination[destination_len - 1] = '\0';
  return true;
}

bool applyCacheEntry(Aircraft* plane, const EnrichmentCacheEntry& entry) {
  bool changed = false;
  changed |= copyIfDifferent(plane->route, sizeof(plane->route), entry.route);
  changed |= copyIfDifferent(plane->type, sizeof(plane->type), entry.type);
  return changed;
}

EnrichmentCacheEntry* cacheSlotFor(const Aircraft& plane,
                                   unsigned long now) {
  EnrichmentCacheEntry* oldest = &s_enrichment_cache[0];
  unsigned long oldest_age = 0;
  for (auto& entry : s_enrichment_cache) {
    if (sameKey(entry, plane) || entry.refreshed_ms == 0) {
      return &entry;
    }
    const unsigned long age = now - entry.refreshed_ms;
    if (age >= oldest_age) {
      oldest = &entry;
      oldest_age = age;
    }
  }
  return oldest;
}

void copySafeIdentifier(const char* source, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  size_t written = 0;
  if (source != nullptr) {
    for (size_t i = 0; source[i] != '\0' && written + 1 < out_len; ++i) {
      const unsigned char ch = static_cast<unsigned char>(source[i]);
      if (std::isalnum(ch)) {
        out[written++] =
            static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      }
    }
  }
  out[written] = '\0';
}

void copyAirportCode(const JsonObject& airport, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  copyJsonStringTrimmed(airport, "iata_code", out, out_len);
  if (out[0] == '\0') {
    copyJsonStringTrimmed(airport, "icao_code", out, out_len);
  }
}

void parseRoute(const JsonObject& response, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  JsonObject route = response["flightroute"].as<JsonObject>();
  if (route.isNull()) {
    return;
  }

  char origin[5] = {};
  char destination[5] = {};
  copyAirportCode(route["origin"].as<JsonObject>(), origin, sizeof(origin));
  copyAirportCode(route["destination"].as<JsonObject>(), destination,
                  sizeof(destination));
  if (origin[0] != '\0' && destination[0] != '\0') {
    snprintf(out, out_len, "%s-%s", origin, destination);
  }
}

bool startsWith(const char* value, const char* prefix) {
  return value != nullptr && prefix != nullptr &&
         strncmp(value, prefix, strlen(prefix)) == 0;
}

void compactAircraftType(const char* detailed, const char* icao, char* out,
                         size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';

  const char* value = detailed;
  char prefixed[40] = {};
  if (startsWith(value, "Boeing ")) {
    snprintf(prefixed, sizeof(prefixed), "B%s", value + 7);
    value = prefixed;
  } else if (startsWith(value, "Airbus ")) {
    value += 7;
  } else if (startsWith(value, "Bombardier ")) {
    value += 11;
  } else if (startsWith(value, "Embraer ")) {
    value += 8;
  } else if (startsWith(value, "De Havilland Canada ")) {
    value += 20;
  }

  if (value == nullptr || value[0] == '\0') {
    value = icao;
  }
  if (value == nullptr) {
    return;
  }

  size_t written = 0;
  bool previous_space = false;
  for (size_t i = 0; value[i] != '\0' && written + 1 < out_len; ++i) {
    const char ch = value[i];
    if (ch == ' ') {
      if (!previous_space) {
        out[written++] = ' ';
        previous_space = true;
      }
      continue;
    }
    out[written++] = ch;
    previous_space = false;
  }
  while (written > 0 && out[written - 1] == ' ') {
    --written;
  }
  out[written] = '\0';
}

void parseDetailedType(const JsonObject& response, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  JsonObject aircraft = response["aircraft"].as<JsonObject>();
  if (aircraft.isNull()) {
    return;
  }
  compactAircraftType(aircraft["type"] | nullptr,
                      aircraft["icao_type"] | nullptr, out, out_len);
}

String enrichmentUrl(const Aircraft& plane) {
  char hex[sizeof(plane.hex)] = {};
  char callsign[sizeof(plane.callsign)] = {};
  copySafeIdentifier(plane.hex, hex, sizeof(hex));
  copySafeIdentifier(plane.callsign, callsign, sizeof(callsign));

  String url = config::kFlightDataApiBase;
  if (hex[0] != '\0') {
    url += "aircraft/";
    url += hex;
    if (callsign[0] != '\0' && strcmp(hex, callsign) != 0) {
      url += "?callsign=";
      url += callsign;
    }
  } else {
    url += "callsign/";
    url += callsign;
  }
  return url;
}

String callsignRouteUrl(const Aircraft& plane) {
  char callsign[sizeof(plane.callsign)] = {};
  copySafeIdentifier(plane.callsign, callsign, sizeof(callsign));
  if (callsign[0] == '\0') {
    return {};
  }

  String url = config::kFlightDataApiBase;
  url += "callsign/";
  url += callsign;
  return url;
}

bool fetchFlightDataJson(const String& url, const char* callsign,
                         JsonDocument* doc, bool* not_found) {
  *not_found = false;
  if (!prepareSecureClient(&s_lookup_client, "flight data",
                           kMinHeapForLookupSslBytes,
                           kMinLargestBlockForLookupSslBytes)) {
    return false;
  }

  HTTPClient& http = s_lookup_http;
  if (!http.begin(s_lookup_client, url)) {
    Serial.println("flight data: http.begin failed");
    return false;
  }
  http.setReuse(true);
  http.setTimeout(config::kFlightLookupTimeoutMs);
  const int code =
      performGetWithPoll(http, config::kFlightLookupTimeoutMs, 1);
  if (code < 0) {
    ++s_lookup_connect_fail_count;
  }

  String payload;
  bool body_complete = false;
  if (code == HTTP_CODE_OK) {
    readResponseBodyWithPoll(http, payload, config::kFlightLookupTimeoutMs,
                             &body_complete, "flight data");
    if (!body_complete) {
      // Don't trust a truncated/ambiguous body, and don't let a corrupted
      // mid-stream state bleed into the next reused request on this
      // connection -- force a clean reconnect next time instead.
      Serial.println("flight data: incomplete response, dropping connection");
      s_lookup_client.stop();
      payload = String();
    }
  }
  http.end();

  if (code == HTTP_CODE_NOT_FOUND) {
    *not_found = true;
    return true;
  }
  if (code != HTTP_CODE_OK || payload.length() == 0) {
    Serial.printf("flight data: HTTP %d for %s\n", code, callsign);
    return false;
  }

  const DeserializationError error = deserializeJson(*doc, payload);
  if (error) {
    Serial.printf("flight data: JSON parse error: %s\n", error.c_str());
    return false;
  }
  return true;
}

bool fetchEnrichment(const Aircraft& plane, EnrichmentCacheEntry* entry) {
  entry->route[0] = '\0';
  entry->type[0] = '\0';

  JsonDocument doc;
  bool not_found = false;
  if (!fetchFlightDataJson(enrichmentUrl(plane), plane.callsign, &doc,
                           &not_found)) {
    return false;
  }

  if (!not_found) {
    JsonObject response = doc["response"].as<JsonObject>();
    if (!response.isNull()) {
      parseRoute(response, entry->route, sizeof(entry->route));
      parseDetailedType(response, entry->type, sizeof(entry->type));
    }
  }

  // ADSBDB may return HTTP 200 "unknown aircraft" from the combined endpoint
  // even though its callsign database has a valid route. Retry only the route
  // through the callsign endpoint in that case.
  if (entry->route[0] == '\0') {
    const String route_url = callsignRouteUrl(plane);
    if (route_url.length() > 0 && route_url != enrichmentUrl(plane)) {
      JsonDocument route_doc;
      bool route_not_found = false;
      if (!fetchFlightDataJson(route_url, plane.callsign, &route_doc,
                               &route_not_found)) {
        return false;
      }
      if (!route_not_found) {
        JsonObject response = route_doc["response"].as<JsonObject>();
        if (!response.isNull()) {
          parseRoute(response, entry->route, sizeof(entry->route));
        }
      }
    }
  }

  if (entry->route[0] == '\0' && entry->type[0] == '\0') {
    Serial.printf("flight data: no match for %s\n", plane.callsign);
  }
  return true;
}

void applyCachedEnrichment(Aircraft* plane) {
  EnrichmentCacheEntry* entry = findCacheEntry(*plane, millis());
  if (entry != nullptr) {
    applyCacheEntry(plane, *entry);
  }
}

bool altitudeFilteredOut(const Aircraft& plane) {
  if (!services::settings::altitudeFilterEnabled()) {
    return false;
  }

  float altitude_ft = 0.0f;
  if (plane.on_ground) {
    altitude_ft = 0.0f;
  } else if (plane.has_altitude) {
    altitude_ft = plane.altitude_ft;
  } else {
    // Keep planes with unknown altitude visible.
    return false;
  }

  altitude_ft += services::settings::altitudeOffsetFeet();
  const float threshold_ft = services::settings::altitudeFilterThresholdFeet();
  if (services::settings::altitudeFilterHideUnder()) {
    return altitude_ft < threshold_ft;
  }
  return altitude_ft > threshold_ft;
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

bool fetchInProgress() { return s_fetch_in_progress; }

namespace {
// Keeps s_fetch_in_progress true for fetchUpdate()'s entire body, including
// every early-return path, without needing to set it at each return site.
struct FetchInProgressGuard {
  FetchInProgressGuard() { s_fetch_in_progress = true; }
  ~FetchInProgressGuard() { s_fetch_in_progress = false; }
};
}  // namespace

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastFetchUpdateMs() { return s_last_fetch_update_ms; }

void releasePersistentConnection() {
  // Both clients are kept alive/reused across polls to avoid repeated TLS
  // handshakes (see fetchUpdate()/fetchFlightDataJson()). That's normally a
  // win, but it also means each one permanently holds onto a large
  // contiguous heap block for as long as it stays connected. Occasional,
  // low-frequency consumers elsewhere (weather) can starve forever if ADS-B
  // never lets go, so give them an explicit way to reclaim that memory.
  const bool adsb_was_connected = s_adsb_client.connected();
  const bool lookup_was_connected = s_lookup_client.connected();
  const uint32_t heap_before = ESP.getFreeHeap();
  const size_t largest_before =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  s_adsb_client.stop();
  s_lookup_client.stop();
  Serial.printf(
      "adsb: release requested (adsb_conn=%d,lookup_conn=%d) heap %lu->%lu "
      "largest %u->%u\n",
      adsb_was_connected, lookup_was_connected,
      static_cast<unsigned long>(heap_before),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned>(largest_before),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  // Skips the poll callback's radar redraw for this call's duration (see
  // main.cpp's onNetworkPoll()) -- a ~25fps redraw competing for CPU with
  // the blocking socket read was slowing effective read throughput enough
  // to time out mid-body on larger responses (see repo memory, TWENTY-
  // SECOND issue).
  FetchInProgressGuard fetch_guard;
  maybeLogAdsbDiagnostics();

  const unsigned long now = millis();
  if (s_adsb_tls_cooldown_until_ms != 0 && now < s_adsb_tls_cooldown_until_ms) {
    return false;
  }
  if (s_last_adsb_tls_skip_ms != 0 &&
      now - s_last_adsb_tls_skip_ms < kAdsbTlsLowHeapBackoffMs) {
    return false;
  }

  const size_t previous_count = s_aircraft_count;
  memcpy(s_previous_aircraft, s_aircraft, sizeof(s_aircraft));

  s_last_center_lat = center_lat;
  s_last_center_lon = center_lon;
  // Cap the radius actually queried, independent of the (screen-scaled)
  // radius the caller asks for -- see config::kAdsbMaxFetchRadiusKm.
  const float capped_fetch_radius_km =
      fetch_radius_km > config::kAdsbMaxFetchRadiusKm
          ? config::kAdsbMaxFetchRadiusKm
          : fetch_radius_km;
  const float dist_nm = kmToNauticalMiles(capped_fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  WiFiClientSecure& client = s_adsb_client;
  if (!prepareSecureClient(&client, "adsb", kMinHeapForSslBytes,
                           kMinLargestBlockForAdsbSslBytes)) {
    s_last_adsb_tls_skip_ms = now;
    s_adsb_tls_cooldown_until_ms = now + kAdsbTlsLowHeapBackoffMs;
    return false;
  }
  s_last_adsb_tls_skip_ms = 0;

  HTTPClient& http = s_adsb_http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }
  http.setReuse(true);

  http.useHTTP10(true); // <-- FORCE SERVER TO DROP CHUNKED ENCODING

  http.setTimeout(kAdsbRequestTimeoutMs);
  const int code = performGetWithPoll(http, kAdsbRequestTimeoutMs, 1);
  if (code < 0) {
    ++s_adsb_connect_fail_count;
    const uint8_t capped_streak =
        s_adsb_connect_fail_streak < 3 ? s_adsb_connect_fail_streak : 3;
    const unsigned long backoff_ms = std::min(
        kAdsbTlsFailureBackoffMaxMs,
        kAdsbTlsFailureBackoffBaseMs << capped_streak);
    s_adsb_tls_cooldown_until_ms = now + backoff_ms;
    if (s_adsb_connect_fail_streak < 255) {
      ++s_adsb_connect_fail_streak;
    }
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }
  if (s_adsb_probe_active_largest) {
    ++s_adsb_low_largest_probe_success_count;
    s_adsb_probe_active_largest = false;
  }
  if (s_adsb_probe_active_heap) {
    ++s_adsb_low_heap_probe_success_count;
    s_adsb_probe_active_heap = false;
  }
  s_adsb_connect_fail_streak = 0;
  s_adsb_tls_cooldown_until_ms = 0;

  String payload;
  bool body_complete = false;
  const bool got_body =
      readResponseBodyWithPoll(http, payload, kAdsbBodyReadTimeoutMs,
                               &body_complete, "adsb");
  if (!got_body || !body_complete) {
    Serial.println(got_body ? "adsb: incomplete response, dropping connection"
                            : "adsb: empty response");
    // Force a hard close instead of trusting HTTPClient's keep-alive reuse:
    // a truncated/ambiguous body means the connection's byte stream state
    // can no longer be trusted for the next reused request.
    client.stop();
    http.end();
    // Use a fresh millis() here, not the stale `now` captured at function
    // entry -- the request/read above can itself take up to the full
    // ~10s timeout, so `now` may already be that old.
    s_adsb_tls_cooldown_until_ms = millis() + kAdsbIncompleteResponseBackoffMs;
    return false;
  }
  http.end();

  // The adsb.fi response includes many fields per aircraft (rr_lat/rr_lon,
  // nac_p/nac_v, sil, sda, mlat[], tisb[], messages, seen, rssi, dbFlags...)
  // that this firmware never reads. Parsing them anyway roughly doubled the
  // JsonDocument's memory footprint on wide-radius fetches (many aircraft),
  // which combined with the raw payload buffer could exhaust/fragment the
  // heap (`NoMemory`/`IncompleteInput` parse errors). A filter tells
  // ArduinoJson to skip storing anything outside this shape entirely.
  JsonDocument filter;
  JsonObject filter_ac = filter["ac"][0].to<JsonObject>();
  filter_ac["lat"] = true;
  filter_ac["lon"] = true;
  filter_ac["true_heading"] = true;
  filter_ac["mag_heading"] = true;
  filter_ac["track"] = true;
  filter_ac["gs"] = true;
  filter_ac["baro_rate"] = true;
  filter_ac["geom_rate"] = true;
  filter_ac["alt_baro"] = true;
  filter_ac["alt_geom"] = true;
  filter_ac["hex"] = true;
  filter_ac["flight"] = true;
  filter_ac["t"] = true;  // basic aircraft type code, tag's fallback model line

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    s_last_fetch_update_ms = millis();
    Serial.printf("adsb: 0 aircraft\n");
    return true;
  }

  size_t n = 0;
  size_t scanned = 0;
  for (JsonObject plane : ac) {
    ++scanned;
    if ((scanned & 0x03u) == 0u) {
      pollNetwork();
    }
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    if (altitudeFilteredOut(s_aircraft[n])) {
      continue;
    }

    const Aircraft* prev =
      findPreviousAircraftSample(s_previous_aircraft, previous_count,
                                   s_aircraft[n]);
    if (prev != nullptr) {
      s_aircraft[n].has_prev_sample = true;
      s_aircraft[n].prev_lat = prev->lat;
      s_aircraft[n].prev_lon = prev->lon;
      s_aircraft[n].prev_altitude_ft = prev->altitude_ft;
      s_aircraft[n].prev_has_altitude = prev->has_altitude;
      s_aircraft[n].prev_on_ground = prev->on_ground;
    }

    applyCachedEnrichment(&s_aircraft[n]);
    ++n;
  }

  s_aircraft_count = n;
  s_last_fetch_update_ms = millis();
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

bool enrichOnePending() {
  const unsigned long now = millis();
  if (s_aircraft_count == 0 ||
      (s_last_enrichment_failure_ms != 0 &&
       now - s_last_enrichment_failure_ms <
           config::kFlightLookupFailureBackoffMs) ||
      (s_last_enrichment_lookup_ms != 0 &&
       now - s_last_enrichment_lookup_ms <
           config::kFlightLookupMinIntervalMs)) {
    return false;
  }

  // Keep core ADS-B updates responsive: skip non-critical enrichment when
  // memory is below the same safety envelope used by lookup TLS.
  const uint32_t free_heap = ESP.getFreeHeap();
  const size_t largest_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (free_heap < kMinHeapForLookupWorkBytes ||
      largest_block < kMinLargestBlockForLookupWorkBytes) {
    return false;
  }

  size_t candidate_index = kMaxAircraft;
  float candidate_dist_sq = FLT_MAX;
  for (size_t index = 0; index < s_aircraft_count; ++index) {
    Aircraft& plane = s_aircraft[index];
    if (plane.hex[0] == '\0' && plane.callsign[0] == '\0') {
      continue;
    }

    EnrichmentCacheEntry* cached = findCacheEntry(plane, now);
    if (cached != nullptr) {
      applyCacheEntry(&plane, *cached);
      continue;
    }

    const float dlat = plane.lat - static_cast<float>(s_last_center_lat);
    const float dlon = plane.lon - static_cast<float>(s_last_center_lon);
    const float dist_sq = dlat * dlat + dlon * dlon;
    if (dist_sq < candidate_dist_sq) {
      candidate_index = index;
      candidate_dist_sq = dist_sq;
    }
  }

  if (candidate_index == kMaxAircraft) {
    return false;
  }

  Aircraft& plane = s_aircraft[candidate_index];
  s_last_enrichment_lookup_ms = now;
  EnrichmentCacheEntry* entry = cacheSlotFor(plane, now);
  *entry = {};
  strncpy(entry->hex, plane.hex, sizeof(entry->hex) - 1);
  strncpy(entry->callsign, plane.callsign, sizeof(entry->callsign) - 1);

  if (!fetchEnrichment(plane, entry)) {
    // Network failures are retried after a short global backoff.
    *entry = {};
    s_last_enrichment_failure_ms = millis();
    return false;
  }

  s_last_enrichment_failure_ms = 0;
  entry->refreshed_ms = millis();
  entry->has_data = entry->route[0] != '\0' || entry->type[0] != '\0';
  const bool changed = applyCacheEntry(&plane, *entry);
  Serial.printf("flight data: %s %s %s\n", plane.callsign,
                entry->route[0] != '\0' ? entry->route : "(route unknown)",
                entry->type[0] != '\0' ? entry->type : "(type unchanged)");
  return changed;
}

}  // namespace services::adsb
