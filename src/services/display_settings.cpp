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
constexpr char kKeyFahrenheit[] = "tempF";
constexpr char kKeyAltitudeMeters[] = "altM";
constexpr char kKeyAltitudeOffsetFeet[] = "altOffFt";
constexpr char kKeyAltitudeFilterEnabled[] = "altFilOn";
constexpr char kKeyAltitudeFilterUnder[] = "altFilUnd";
constexpr char kKeyAltitudeFilterThresholdFeet[] = "altFilFt";
constexpr char kKeyAdsbInterpolation[] = "adsbIntrp";
constexpr char kKeyInterpolationDelayMs[] = "interpDly";
constexpr char kKeyClockFollowInterp[] = "clkIntrp";
constexpr char kKeyClock24[] = "time24";
constexpr char kKeyTimeSeconds[] = "timeSec";
constexpr char kKeyLastWeatherFixTime[] = "wxFixTime";
constexpr char kKeyLastAdsbFetchTime[] = "adsbFixTime";
constexpr char kKeyTextScale[] = "fontPct";
constexpr char kKeyOtaPassword[] = "otaPass";
constexpr char kKeyAutoDim[] = "autoDim";
constexpr char kKeyBrightness[] = "brightPct";

char s_ota_password[kOtaPasswordMaxLen + 1] = {};
char s_footer_config[13] = {};
bool s_temperature_fahrenheit = false;
bool s_altitude_meters = false;
float s_altitude_offset_feet = 0.0f;
bool s_altitude_filter_enabled = false;
bool s_altitude_filter_hide_under = false;
float s_altitude_filter_threshold_feet = 0.0f;
bool s_adsb_interpolation_enabled = true;
bool s_use_24_hour_clock = true;
bool s_show_time_seconds = false;
bool s_show_last_weather_fix_time = false;
bool s_show_last_adsb_fetch_time = false;
int s_text_scale_percent = kTextScaleDefaultPercent;
int s_interpolation_delay_ms = kInterpolationDelayDefaultMs;
bool s_clock_follows_interpolation_delay = true;
bool s_auto_dim_enabled = false;
int s_brightness_percent = kBrightnessDefaultPercent;

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

int clampBrightnessPercent(int value) {
  if (value < kBrightnessMinPercent) {
    return kBrightnessMinPercent;
  }
  if (value > kBrightnessMaxPercent) {
    return kBrightnessMaxPercent;
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

bool parseBrightnessPercent(const char* value, int* result) {
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

  *result = clampBrightnessPercent(static_cast<int>(parsed));
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
  copyCleanText("none", s_footer_config,
                sizeof(s_footer_config));
  s_temperature_fahrenheit = false;
  s_altitude_meters = false;
  s_altitude_offset_feet = 0.0f;
  s_altitude_filter_enabled = false;
  s_altitude_filter_hide_under = false;
  s_altitude_filter_threshold_feet = 0.0f;
  s_adsb_interpolation_enabled = true;
  s_use_24_hour_clock = true;
  s_show_time_seconds = false;
  s_show_last_weather_fix_time = false;
  s_show_last_adsb_fetch_time = false;
  s_text_scale_percent = kTextScaleDefaultPercent;
  s_interpolation_delay_ms = kInterpolationDelayDefaultMs;
  s_clock_follows_interpolation_delay = true;
  s_auto_dim_enabled = false;
  s_brightness_percent = kBrightnessDefaultPercent;
}

void persist() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putString(kKeyFooter, s_footer_config);
  prefs.putBool(kKeyFahrenheit, s_temperature_fahrenheit);
  prefs.putBool(kKeyAltitudeMeters, s_altitude_meters);
  prefs.putFloat(kKeyAltitudeOffsetFeet, s_altitude_offset_feet);
  prefs.putBool(kKeyAltitudeFilterEnabled, s_altitude_filter_enabled);
  prefs.putBool(kKeyAltitudeFilterUnder, s_altitude_filter_hide_under);
  prefs.putFloat(kKeyAltitudeFilterThresholdFeet,
                 s_altitude_filter_threshold_feet);
  prefs.putBool(kKeyAdsbInterpolation, s_adsb_interpolation_enabled);
  prefs.putBool(kKeyClock24, s_use_24_hour_clock);
  prefs.putBool(kKeyTimeSeconds, s_show_time_seconds);
  prefs.putBool(kKeyLastWeatherFixTime, s_show_last_weather_fix_time);
  prefs.putBool(kKeyLastAdsbFetchTime, s_show_last_adsb_fetch_time);
  prefs.putInt(kKeyTextScale, s_text_scale_percent);
  prefs.putInt(kKeyInterpolationDelayMs, s_interpolation_delay_ms);
  prefs.putBool(kKeyClockFollowInterp, s_clock_follows_interpolation_delay);
  prefs.putBool(kKeyAutoDim, s_auto_dim_enabled);
  prefs.putInt(kKeyBrightness, s_brightness_percent);
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

  String footer_value = prefs.getString(kKeyFooter, "none");
  copyCleanText(footer_value.c_str(), s_footer_config, sizeof(s_footer_config));
  if (s_footer_config[0] == '\0') {
    copyCleanText("none", s_footer_config,
                  sizeof(s_footer_config));
  }
  s_temperature_fahrenheit = prefs.getBool(kKeyFahrenheit, false);
  s_altitude_meters = prefs.getBool(kKeyAltitudeMeters, false);
  s_altitude_offset_feet = prefs.getFloat(kKeyAltitudeOffsetFeet, 0.0f);
  s_altitude_filter_enabled = prefs.getBool(kKeyAltitudeFilterEnabled, false);
  s_altitude_filter_hide_under = prefs.getBool(kKeyAltitudeFilterUnder, false);
  s_altitude_filter_threshold_feet =
      prefs.getFloat(kKeyAltitudeFilterThresholdFeet, 0.0f);
  s_adsb_interpolation_enabled = prefs.getBool(kKeyAdsbInterpolation, true);
  s_use_24_hour_clock = prefs.getBool(kKeyClock24, true);
  s_show_time_seconds = prefs.getBool(kKeyTimeSeconds, false);
  s_show_last_weather_fix_time =
      prefs.getBool(kKeyLastWeatherFixTime, false);
  s_show_last_adsb_fetch_time =
      prefs.getBool(kKeyLastAdsbFetchTime, false);
  s_text_scale_percent = clampTextScalePercent(
      prefs.getInt(kKeyTextScale, kTextScaleDefaultPercent));
  s_interpolation_delay_ms = clampInterpolationDelayMs(
      prefs.getInt(kKeyInterpolationDelayMs, kInterpolationDelayDefaultMs));
  s_clock_follows_interpolation_delay =
      prefs.getBool(kKeyClockFollowInterp, true);
  s_auto_dim_enabled = prefs.getBool(kKeyAutoDim, false);
  s_brightness_percent = clampBrightnessPercent(
      prefs.getInt(kKeyBrightness, kBrightnessDefaultPercent));

  String value = prefs.getString(kKeyOtaPassword, config::kDefaultOtaPassword);
  copyCleanText(value.c_str(), s_ota_password, sizeof(s_ota_password));
  if (s_ota_password[0] == '\0') {
    copyCleanText(config::kDefaultOtaPassword, s_ota_password,
                  sizeof(s_ota_password));
  }
  prefs.end();
}

const char* footerConfig() { return s_footer_config; }

bool footerEnabled() { return strcmp(s_footer_config, "none") != 0; }

bool weatherEnabled() { return strcmp(s_footer_config, "time_weather") == 0; }

bool heapEnabled() { return strcmp(s_footer_config, "time_heap") == 0; }

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

bool adsbInterpolationEnabled() { return s_adsb_interpolation_enabled; }

int interpolationDelayMs() { return s_interpolation_delay_ms; }

bool clockFollowsInterpolationDelay() {
  return s_clock_follows_interpolation_delay;
}

bool use24HourClock() { return s_use_24_hour_clock; }

bool showTimeSeconds() { return s_show_time_seconds; }

bool showLastWeatherFixTime() { return s_show_last_weather_fix_time; }

bool showLastAdsbFetchTime() { return s_show_last_adsb_fetch_time; }

int textScalePercent() { return s_text_scale_percent; }

bool autoDimEnabled() { return s_auto_dim_enabled; }

int brightnessPercent() { return s_brightness_percent; }

const char* otaPassword() { return s_ota_password; }

void saveFromPortal(const char* footer_config,
                    const char* fahrenheit_checkbox,
                    bool use_miles,
                    const char* altitude_offset_value,
                    const char* altitude_filter_enabled_checkbox,
                    const char* altitude_filter_under_checkbox,
                    const char* altitude_filter_value,
                    const char* adsb_interpolation_checkbox,
                    const char* interpolation_delay_ms_value,
                    const char* clock24_checkbox,
                    const char* time_seconds_checkbox,
                    const char* clock_follow_interp_checkbox,
                    const char* last_weather_fix_time_checkbox,
                    const char* last_adsb_fetch_time_checkbox,
                    const char* text_scale_percent_value,
                    const char* auto_dim_checkbox,
                    const char* brightness_percent_value,
                    const char* ota_password_value) {

  char footer[13] = {};
  copyCleanText(footer_config, footer, sizeof(footer));
  if (footer[0] != '\0') {
    strncpy(s_footer_config, footer, sizeof(s_footer_config) - 1);
    s_footer_config[sizeof(s_footer_config) - 1] = '\0';
  }
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
  s_adsb_interpolation_enabled = checkboxChecked(adsb_interpolation_checkbox);
  s_use_24_hour_clock = checkboxChecked(clock24_checkbox);
  s_show_time_seconds = checkboxChecked(time_seconds_checkbox);
  s_clock_follows_interpolation_delay =
      checkboxChecked(clock_follow_interp_checkbox);
  s_show_last_weather_fix_time =
      checkboxChecked(last_weather_fix_time_checkbox);
  s_show_last_adsb_fetch_time =
      checkboxChecked(last_adsb_fetch_time_checkbox);
  int text_scale_percent = s_text_scale_percent;
  if (parseTextScalePercent(text_scale_percent_value, &text_scale_percent)) {
    s_text_scale_percent = text_scale_percent;
  }
  int interpolation_delay_ms = s_interpolation_delay_ms;
  if (parseInterpolationDelayMs(interpolation_delay_ms_value,
                                &interpolation_delay_ms)) {
    s_interpolation_delay_ms = interpolation_delay_ms;
  }
  s_auto_dim_enabled = checkboxChecked(auto_dim_checkbox);
  int brightness_percent = s_brightness_percent;
  if (parseBrightnessPercent(brightness_percent_value, &brightness_percent)) {
    s_brightness_percent = brightness_percent;
  }

  char password[kOtaPasswordMaxLen + 1] = {};
  copyCleanText(ota_password_value, password, sizeof(password));
  if (password[0] != '\0') {
    strncpy(s_ota_password, password, sizeof(s_ota_password) - 1);
    s_ota_password[sizeof(s_ota_password) - 1] = '\0';
  }

  persist();
  Serial.printf("Footer: %s, altitude: %s, text: %d%%\n",
                s_footer_config,
                s_altitude_meters ? "m" : "ft", s_text_scale_percent);
  Serial.printf("Altitude offset: %.1f ft\n", s_altitude_offset_feet);
  Serial.printf("Altitude filter: %s, mode: %s, threshold: %.1f ft\n",
                s_altitude_filter_enabled ? "on" : "off",
                s_altitude_filter_hide_under ? "hide below" : "hide above",
                s_altitude_filter_threshold_feet);
  Serial.printf("ADS-B interpolation: %s\n",
                s_adsb_interpolation_enabled ? "on" : "off");
  Serial.printf("Interpolation delay: %d ms\n", s_interpolation_delay_ms);
  Serial.printf("Auto-dim: %s, brightness: %d%%\n",
                s_auto_dim_enabled ? "on" : "off", s_brightness_percent);
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
