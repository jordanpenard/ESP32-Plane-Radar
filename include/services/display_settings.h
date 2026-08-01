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

/** Load persistent display and OTA settings from NVS. */
void init();

bool footerEnabled();
bool weatherEnabled();
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
int textScalePercent();
const char* otaPassword();

/**
 * Store web-portal values. An empty OTA password keeps the current password so
 * the portal never needs to echo the stored secret into its HTML.
 */
void saveFromPortal(const char* footer_checkbox, const char* weather_checkbox,
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
                    const char* text_scale_percent_value,
                    const char* ota_password_value);

/** Restore defaults during a full BOOT-button reset. */
void clear();

}  // namespace services::settings
