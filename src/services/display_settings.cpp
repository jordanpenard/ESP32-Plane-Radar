#include "services/display_settings.h"

#include <Preferences.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "config.h"

namespace services::settings {
namespace {

constexpr char kPrefsNamespace[] = "display";
constexpr char kKeyFooter[] = "footer";
constexpr char kKeyWeather[] = "weather";
constexpr char kKeyFahrenheit[] = "tempF";
constexpr char kKeyAltitudeMeters[] = "altM";
constexpr char kKeyAltitudeOffsetFeet[] = "altOffFt";
constexpr char kKeyAltitudeFilterEnabled[] = "altFilOn";
constexpr char kKeyAltitudeFilterUnder[] = "altFilUnd";
constexpr char kKeyAltitudeFilterThresholdFeet[] = "altFilFt";
constexpr char kKeyInterpolationDelayMs[] = "interpDly";
constexpr char kKeyClockFollowInterp[] = "clkIntrp";
constexpr char kKeyClock24[] = "time24";
constexpr char kKeyTimeSeconds[] = "timeSec";
constexpr char kKeyTextScale[] = "fontPct";
constexpr char kKeyOtaPassword[] = "otaPass";

char s_ota_password[kOtaPasswordMaxLen + 1] = {};
bool s_footer_enabled = true;
bool s_weather_enabled = true;
bool s_temperature_fahrenheit = false;
bool s_altitude_meters = false;
float s_altitude_offset_feet = 0.0f;
bool s_altitude_filter_enabled = false;
bool s_altitude_filter_hide_under = false;
float s_altitude_filter_threshold_feet = 0.0f;
bool s_use_24_hour_clock = true;
bool s_show_time_seconds = false;
int s_text_scale_percent = kTextScaleDefaultPercent;
int s_interpolation_delay_ms = kInterpolationDelayDefaultMs;
bool s_clock_follows_interpolation_delay = true;

bool checkboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  return strcmp(value, "on") == 0 || strcmp(value, "T") == 0 ||
         strcmp(value, "t") == 0 || strcmp(value, "F") == 0 ||
         strcmp(value, "f") == 0;
}

void copyCleanText(const char* value, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (value == nullptr) {
    return;
  }

  size_t written = 0;
  bool previous_space = true;
  for (size_t i = 0; value[i] != '\0' && written + 1 < out_len; ++i) {
    const unsigned char ch = static_cast<unsigned char>(value[i]);
    if (std::isspace(ch)) {
      if (!previous_space) {
        out[written++] = ' ';
        previous_space = true;
      }
      continue;
    }
    if (ch >= 32 && ch <= 126) {
      out[written++] = static_cast<char>(ch);
      previous_space = false;
    }
  }
  while (written > 0 && out[written - 1] == ' ') {
    --written;
  }
  out[written] = '\0';
}

int clampTextScalePercent(int value) {
  if (value < kTextScaleMinPercent) {
    return kTextScaleMinPercent;
  }
  if (value > kTextScaleMaxPercent) {
    return kTextScaleMaxPercent;
  }
  return value;
}

int clampInterpolationDelayMs(int value) {
  if (value < kInterpolationDelayMinMs) {
    return kInterpolationDelayMinMs;
  }
  if (value > kInterpolationDelayMaxMs) {
    return kInterpolationDelayMaxMs;
  }
  return value;
}

bool parseTextScalePercent(const char* value, int* result) {
  if (value == nullptr || value[0] == '\0' || result == nullptr) {
    return false;
  }

  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value) {
    return false;
  }
  while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }

  *result = clampTextScalePercent(static_cast<int>(parsed));
  return true;
}

bool parseInterpolationDelayMs(const char* value, int* result) {
  if (value == nullptr || value[0] == '\0' || result == nullptr) {
    return false;
  }

  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value) {
    return false;
  }
  while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }

  *result = clampInterpolationDelayMs(static_cast<int>(parsed));
  return true;
}

bool parseAltitudeOffset(const char* value, float* result) {
  if (value == nullptr || value[0] == '\0' || result == nullptr) {
    return false;
  }

  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  if (end == value) {
    return false;
  }
  while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }

  *result = parsed;
  return true;
}

void loadDefaults() {
  copyCleanText(config::kDefaultOtaPassword, s_ota_password,
                sizeof(s_ota_password));
  s_footer_enabled = true;
  s_weather_enabled = true;
  s_temperature_fahrenheit = false;
  s_altitude_meters = false;
  s_altitude_offset_feet = 0.0f;
  s_altitude_filter_enabled = false;
  s_altitude_filter_hide_under = false;
  s_altitude_filter_threshold_feet = 0.0f;
  s_use_24_hour_clock = true;
  s_show_time_seconds = false;
  s_text_scale_percent = kTextScaleDefaultPercent;
  s_interpolation_delay_ms = kInterpolationDelayDefaultMs;
  s_clock_follows_interpolation_delay = true;
}

void persist() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kKeyFooter, s_footer_enabled);
  prefs.putBool(kKeyWeather, s_weather_enabled);
  prefs.putBool(kKeyFahrenheit, s_temperature_fahrenheit);
  prefs.putBool(kKeyAltitudeMeters, s_altitude_meters);
  prefs.putFloat(kKeyAltitudeOffsetFeet, s_altitude_offset_feet);
  prefs.putBool(kKeyAltitudeFilterEnabled, s_altitude_filter_enabled);
  prefs.putBool(kKeyAltitudeFilterUnder, s_altitude_filter_hide_under);
  prefs.putFloat(kKeyAltitudeFilterThresholdFeet,
                 s_altitude_filter_threshold_feet);
  prefs.putBool(kKeyClock24, s_use_24_hour_clock);
  prefs.putBool(kKeyTimeSeconds, s_show_time_seconds);
  prefs.putInt(kKeyTextScale, s_text_scale_percent);
  prefs.putInt(kKeyInterpolationDelayMs, s_interpolation_delay_ms);
  prefs.putBool(kKeyClockFollowInterp, s_clock_follows_interpolation_delay);
  prefs.putString(kKeyOtaPassword, s_ota_password);
  prefs.end();
}

}  // namespace

void init() {
  loadDefaults();

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }

  s_footer_enabled = prefs.getBool(kKeyFooter, true);
  s_weather_enabled = prefs.getBool(kKeyWeather, true);
  s_temperature_fahrenheit = prefs.getBool(kKeyFahrenheit, false);
  s_altitude_meters = prefs.getBool(kKeyAltitudeMeters, false);
  s_altitude_offset_feet = prefs.getFloat(kKeyAltitudeOffsetFeet, 0.0f);
  s_altitude_filter_enabled = prefs.getBool(kKeyAltitudeFilterEnabled, false);
  s_altitude_filter_hide_under = prefs.getBool(kKeyAltitudeFilterUnder, false);
  s_altitude_filter_threshold_feet =
      prefs.getFloat(kKeyAltitudeFilterThresholdFeet, 0.0f);
  s_use_24_hour_clock = prefs.getBool(kKeyClock24, true);
  s_show_time_seconds = prefs.getBool(kKeyTimeSeconds, false);
  s_text_scale_percent = clampTextScalePercent(
      prefs.getInt(kKeyTextScale, kTextScaleDefaultPercent));
  s_interpolation_delay_ms = clampInterpolationDelayMs(
      prefs.getInt(kKeyInterpolationDelayMs, kInterpolationDelayDefaultMs));
  s_clock_follows_interpolation_delay =
      prefs.getBool(kKeyClockFollowInterp, true);

  String value = prefs.getString(kKeyOtaPassword, config::kDefaultOtaPassword);
  copyCleanText(value.c_str(), s_ota_password, sizeof(s_ota_password));
  if (s_ota_password[0] == '\0') {
    copyCleanText(config::kDefaultOtaPassword, s_ota_password,
                  sizeof(s_ota_password));
  }
  prefs.end();
}

bool footerEnabled() { return s_footer_enabled; }

bool weatherEnabled() { return s_weather_enabled; }

bool temperatureFahrenheit() { return s_temperature_fahrenheit; }

bool altitudeMeters() { return s_altitude_meters; }

float altitudeOffsetFeet() { return s_altitude_offset_feet; }

void setAltitudeOffsetFeet(float feet) {
  s_altitude_offset_feet = feet;
  persist();
  Serial.printf("Altitude offset set: %.1f ft\n", s_altitude_offset_feet);
}

bool altitudeFilterEnabled() { return s_altitude_filter_enabled; }

bool altitudeFilterHideUnder() { return s_altitude_filter_hide_under; }

float altitudeFilterThresholdFeet() { return s_altitude_filter_threshold_feet; }

int interpolationDelayMs() { return s_interpolation_delay_ms; }

bool clockFollowsInterpolationDelay() {
  return s_clock_follows_interpolation_delay;
}

bool use24HourClock() { return s_use_24_hour_clock; }

bool showTimeSeconds() { return s_show_time_seconds; }

int textScalePercent() { return s_text_scale_percent; }

const char* otaPassword() { return s_ota_password; }

void saveFromPortal(const char* footer_checkbox, const char* weather_checkbox,
                    const char* fahrenheit_checkbox,
                    bool use_miles,
                    const char* altitude_offset_value,
                    const char* altitude_filter_enabled_checkbox,
                    const char* altitude_filter_under_checkbox,
                    const char* altitude_filter_value,
                    const char* interpolation_delay_ms_value,
                    const char* clock24_checkbox,
                    const char* time_seconds_checkbox,
                    const char* clock_follow_interp_checkbox,
                    const char* text_scale_percent_value,
                    const char* ota_password_value) {
  s_footer_enabled = checkboxChecked(footer_checkbox);
  s_weather_enabled = checkboxChecked(weather_checkbox);
  s_temperature_fahrenheit = checkboxChecked(fahrenheit_checkbox);
  float altitude_offset = s_altitude_offset_feet;
  if (parseAltitudeOffset(altitude_offset_value, &altitude_offset)) {
    s_altitude_offset_feet = use_miles ? altitude_offset : altitude_offset / 0.3048f;
  }
  s_altitude_filter_enabled = checkboxChecked(altitude_filter_enabled_checkbox);
  s_altitude_filter_hide_under = checkboxChecked(altitude_filter_under_checkbox);
  float altitude_filter_value_feet = s_altitude_filter_threshold_feet;
  if (parseAltitudeOffset(altitude_filter_value, &altitude_filter_value_feet)) {
    s_altitude_filter_threshold_feet =
        use_miles ? altitude_filter_value_feet
                  : altitude_filter_value_feet / 0.3048f;
  }
  s_use_24_hour_clock = checkboxChecked(clock24_checkbox);
  s_show_time_seconds = checkboxChecked(time_seconds_checkbox);
  s_clock_follows_interpolation_delay =
      checkboxChecked(clock_follow_interp_checkbox);
  int text_scale_percent = s_text_scale_percent;
  if (parseTextScalePercent(text_scale_percent_value, &text_scale_percent)) {
    s_text_scale_percent = text_scale_percent;
  }
  int interpolation_delay_ms = s_interpolation_delay_ms;
  if (parseInterpolationDelayMs(interpolation_delay_ms_value,
                                &interpolation_delay_ms)) {
    s_interpolation_delay_ms = interpolation_delay_ms;
  }

  char password[kOtaPasswordMaxLen + 1] = {};
  copyCleanText(ota_password_value, password, sizeof(password));
  if (password[0] != '\0') {
    strncpy(s_ota_password, password, sizeof(s_ota_password) - 1);
    s_ota_password[sizeof(s_ota_password) - 1] = '\0';
  }

  persist();
  Serial.printf("Display footer: %s, weather: %s, altitude: %s, text: %d%%\n",
                s_footer_enabled ? "on" : "off",
                s_weather_enabled ? "on" : "off",
                s_altitude_meters ? "m" : "ft", s_text_scale_percent);
  Serial.printf("Altitude offset: %.1f ft\n", s_altitude_offset_feet);
  Serial.printf("Altitude filter: %s, mode: %s, threshold: %.1f ft\n",
                s_altitude_filter_enabled ? "on" : "off",
                s_altitude_filter_hide_under ? "hide below" : "hide above",
                s_altitude_filter_threshold_feet);
  Serial.printf("Interpolation delay: %d ms\n", s_interpolation_delay_ms);
}

void clear() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  loadDefaults();
}

}  // namespace services::settings
