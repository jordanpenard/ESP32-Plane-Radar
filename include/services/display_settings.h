#pragma once

#include <cstddef>

namespace services::settings {

constexpr size_t kOtaPasswordMaxLen = 32;
constexpr int kTextScaleMinPercent = 80;
constexpr int kTextScaleMaxPercent = 130;
constexpr int kTextScaleDefaultPercent = 110;
constexpr int kInterpolationDelayMinMs = 0;
constexpr int kInterpolationDelayMaxMs = 5000;
constexpr int kInterpolationDelayDefaultMs = 0;
constexpr int kBrightnessMinPercent = 20;
constexpr int kBrightnessMaxPercent = 100;
constexpr int kBrightnessDefaultPercent = 100;

/** Load persistent display and OTA settings from NVS. */
void init();

const char* posix_tz();
bool footerTimeEnabled();
bool footerWeatherEnabled();
bool footerHeapEnabled();
bool footerWifiEnabled();
bool temperatureFahrenheit();
bool altitudeMeters();
float altitudeOffsetFeet();
void setAltitudeOffsetFeet(float feet);
bool altitudeFilterEnabled();
bool altitudeFilterHideUnder();
float altitudeFilterThresholdFeet();
bool adsbInterpolationEnabled();
int interpolationDelayMs();
bool clockFollowsInterpolationDelay();
bool use24HourClock();
bool showTimeSeconds();
/** When on, the weather line periodically shows the last successful weather
 * fetch's date/time (in blue) instead of the current conditions. */
bool showLastWeatherFixTime();
/** When on, the clock line shows the last successful ADS-B fetch's date/time
 * (in green) instead of the live clock. */
bool showLastAdsbFetchTime();
int textScalePercent();
/** When on, the display uses config::kAutoDimNightBrightnessPercent during
 * night hours (config::kAutoDimNightStartHour..kAutoDimNightEndHour local
 * time) instead of brightnessPercent(). */
bool autoDimEnabled();
/** Manual brightness (kBrightnessMinPercent..kBrightnessMaxPercent); also
 * the brightness used whenever autoDimEnabled() is off or it's daytime. */
int brightnessPercent();
const char* otaPassword();

/**
 * Store web-portal values. An empty OTA password keeps the current password so
 * the portal never needs to echo the stored secret into its HTML.
 */
void saveFromPortal(const char* footer_time_checkbox, const char* footer_weather_checkbox,
                    const char* footer_heap_checkbox, const char* footer_wifi_checkbox,
                    const char* posix_tz_config,
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
                    const char* ota_password_value);

/** Restore defaults during a full BOOT-button reset. */
void clear();

}  // namespace services::settings
