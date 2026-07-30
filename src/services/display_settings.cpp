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
constexpr char kKeyClock24[] = "time24";
constexpr char kKeyTextScale[] = "fontPct";
constexpr char kKeyOtaPassword[] = "otaPass";

char s_ota_password[kOtaPasswordMaxLen + 1] = {};
bool s_footer_enabled = true;
bool s_weather_enabled = true;
bool s_temperature_fahrenheit = false;
bool s_altitude_meters = false;
float s_altitude_offset_feet = 0.0f;
bool s_use_24_hour_clock = true;
int s_text_scale_percent = kTextScaleDefaultPercent;

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
  s_use_24_hour_clock = true;
  s_text_scale_percent = kTextScaleDefaultPercent;
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
  prefs.putBool(kKeyClock24, s_use_24_hour_clock);
  prefs.putInt(kKeyTextScale, s_text_scale_percent);
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
  s_use_24_hour_clock = prefs.getBool(kKeyClock24, true);
  s_text_scale_percent = clampTextScalePercent(
      prefs.getInt(kKeyTextScale, kTextScaleDefaultPercent));

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

bool use24HourClock() { return s_use_24_hour_clock; }

int textScalePercent() { return s_text_scale_percent; }

const char* otaPassword() { return s_ota_password; }

void saveFromPortal(const char* footer_checkbox, const char* weather_checkbox,
                    const char* fahrenheit_checkbox,
                    bool use_miles,
                    const char* altitude_offset_value,
                    const char* clock24_checkbox,
                    const char* text_scale_percent_value,
                    const char* ota_password_value) {
  s_footer_enabled = checkboxChecked(footer_checkbox);
  s_weather_enabled = checkboxChecked(weather_checkbox);
  s_temperature_fahrenheit = checkboxChecked(fahrenheit_checkbox);
  float altitude_offset = s_altitude_offset_feet;
  if (parseAltitudeOffset(altitude_offset_value, &altitude_offset)) {
    s_altitude_offset_feet = use_miles ? altitude_offset : altitude_offset / 0.3048f;
  }
  s_use_24_hour_clock = checkboxChecked(clock24_checkbox);
  int text_scale_percent = s_text_scale_percent;
  if (parseTextScalePercent(text_scale_percent_value, &text_scale_percent)) {
    s_text_scale_percent = text_scale_percent;
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
