#include "services/wifi_setup.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include <WiFiClientSecure.h>

#include "config.h"
#include "services/display_settings.h"
#include "services/ota_update.h"
#include "services/radar_location.h"
#include "services/unit_policy.h"
#include "services/weather_time.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_portal_toggle_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootPortalToggleHoldMs &&
        held < config::kBootResetHoldMs) {
      s_boot_portal_toggle_pending = true;
    } else if (held >= config::kBootTapMinMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;
// Off by default: the LAN portal's HTTP server + mDNS permanently reserve
// the one large contiguous heap block that ADS-B/weather/lookup TLS
// handshakes need on this 320KB-RAM, no-PSRAM board. Hold BOOT for
// kBootPortalToggleHoldMs to turn it on when you actually need LAN access.
bool s_lan_portal_wanted = false;

// Boot auto-portal: enabled once right after connecting (see
// wifiStartBootAutoPortal()), auto-disabled if no portal activity is seen
// within config::kBootPortalAutoWindowMs (see wifiLoop()).
bool s_boot_auto_portal_pending = false;
unsigned long s_boot_auto_portal_started_ms = 0;
unsigned long s_boot_auto_portal_deadline_ms = 0;
// Tracks whether the currently-running LAN portal actually has mDNS up,
// since that can't be inferred from s_boot_auto_portal_pending alone: once
// activity cancels the boot auto-window, the countdown stops but mDNS was
// never turned on for that session (see startLanWebPortal()'s enable_mdns).
bool s_lan_portal_mdns_active = false;
bool s_auto_portal_timeout_pending = false;
unsigned long s_last_portal_activity_ms = 0;
// Set by handleExitPortal() (our own claimed /exit route) when the user
// clicks "Exit" in the portal; consumed once by wifiConsumeWebExitRequest()
// so main.cpp can turn the portal off and resume radar mode the same way
// the BOOT-button toggle does.
bool s_web_exit_requested = false;

void notePortalActivity() { s_last_portal_activity_ms = millis(); }

// Pinged by a tiny inline <script> on both the /param page and the
// portal's root menu page, so simply viewing either counts as activity
// (not just submitting a form) for the boot auto-portal window above.
constexpr char kPortalActivityBeacon[] =
    "<script>fetch('/activity').catch(function(){});</script>";

void ensureWifiManager();
// `enable_mdns=false` skips MDNS.begin()/addService() (used for the boot
// auto-portal window, see wifiStartBootAutoPortal()): mDNS is a suspected
// contributor to the LAN portal's permanent largest-free-block
// fragmentation, and the raw IP is already shown on the portal screen, so
// skipping it there costs nothing but the ".local" hostname convenience.
void startLanWebPortal(bool enable_mdns = true);
void stopLanWebPortal();
bool wifiLinkUp();
void attachSettingsRoutes();

String diagnosticsHtml() {
  String html;
  html.reserve(1200);
  html += "<!doctype html><html lang='en'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Plane Radar Diagnostics</title>";
  html += "<style>body{font-family:verdana;padding:1rem}table{border-collapse:collapse;width:100%;max-width:42rem}th,td{border:1px solid #ddd;padding:.5rem;text-align:left}th{background:#f6f6f6}</style>";
  html += "</head><body><h2>Diagnostics</h2><table>";

  html += "<tr><th>Item</th><th>Value</th></tr>";
  html += "<tr><td>Uptime (s)</td><td>" + String(millis() / 1000UL) + "</td></tr>";
  html += "<tr><td>Free heap</td><td>" + String(ESP.getFreeHeap()) + "</td></tr>";
  html += "<tr><td>WiFi status</td><td>" +
    String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") +
    "</td></tr>";
  html += "<tr><td>WiFi IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><td>Weather valid</td><td>" +
    String(services::weather::valid() ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Weather stale</td><td>" +
    String(services::weather::stale() ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Weather last HTTP</td><td>" +
    String(services::weather::lastHttpStatus()) + "</td></tr>";
  html += "<tr><td>Weather last error</td><td>" +
    String(services::weather::lastError()) + "</td></tr>";
  html += "<tr><td>Weather data age (s)</td><td>" +
    String(services::weather::lastSuccessAgeSec()) + "</td></tr>";
  html += "</table><p><a href='/param'>Back to setup</a></p></body></html>";
  return html;
}

constexpr int kCoordParamLen = 20;
constexpr char kLatitudeInputAttrs[] =
    "type=\"number\" step=\"0.000001\" min=\"-90\" max=\"90\"";
constexpr char kLongitudeInputAttrs[] =
    "type=\"number\" step=\"0.000001\" min=\"-180\" max=\"180\"";
constexpr int kAltitudeOffsetParamLen = 16;
constexpr char kAltitudeOffsetInputAttrs[] =
  "type=\"number\" step=\"0.1\"";
constexpr int kInterpolationDelayParamLen = 6;
constexpr char kInterpolationDelayInputAttrs[] =
  "type=\"number\" min=\"0\" max=\"5000\" step=\"1\"";
constexpr int kInterpolationPresetCount = 4;
constexpr int kOtaPasswordParamLen =
    static_cast<int>(services::settings::kOtaPasswordMaxLen);
constexpr int kTextScaleParamLen = 4;

char s_interpolation_preset_html[1800] = {};

WiFiManagerParameter s_param_style(
    "<style>"
    ".pr-h{margin:1.3rem 0 .4rem;padding-top:.7rem;border-top:1px solid #e2e2e2;"
    "font-size:1.05rem;font-weight:700;color:#1fa3ec}"
    ".pr-h:first-of-type{margin-top:.1rem;padding-top:0;border-top:0}"
    ".pr-hint{display:block;margin:-4px 0 8px;color:#767676;font-size:.82em;line-height:1.3}"
    // Dims a field + its label while a related "enabled" checkbox is off.
    ".pr-dim{opacity:.4}"
    "</style>");
WiFiManagerParameter s_script_activity_beacon(kPortalActivityBeacon);

WiFiManagerParameter s_param_back_link_top(
    "<div style=\"margin:0 0 1.1rem\">"
    "<a href=\"/\" style=\"display:inline-block;padding:.35rem .85rem;"
    "background:#eef6fc;color:#1fa3ec;border:1px solid #bfe3f7;"
    "border-radius:.3rem;text-decoration:none;font-size:.92rem\">"
    "&larr; Back to menu</a></div>");

WiFiManagerParameter s_section_location("<h3 class=\"pr-h\">Location</h3>");
WiFiManagerParameter s_section_display("<h3 class=\"pr-h\">Display</h3>");
WiFiManagerParameter s_section_altitude("<h3 class=\"pr-h\">Altitude</h3>");
WiFiManagerParameter s_section_clock("<h3 class=\"pr-h\">Clock</h3>");
WiFiManagerParameter s_section_adsb(
    "<h3 class=\"pr-h\">ADS-B &amp; interpolation</h3>");
WiFiManagerParameter s_section_advanced("<h3 class=\"pr-h\">Advanced</h3>");

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kLatitudeInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kLongitudeInputAttrs);
char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

// Hidden field actually submitted/saved; kept in sync with the visible
// preset <select> below via JS (same pattern as the interpolation delay
// presets further down).
char s_radar_range_idx_default[4] = "1";
WiFiManagerParameter s_param_radar_range_idx(
    "radar_range_idx", "", s_radar_range_idx_default, 2,
    "type=\"hidden\"", WFM_NO_LABEL);
char s_range_preset_html[700] = {};
WiFiManagerParameter s_param_range_preset(s_range_preset_html);

char s_footer_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_footer("show_footer", "Show weather and clock", "T",
                                    2, s_footer_checkbox_attrs,
                                    WFM_LABEL_AFTER);

char s_weather_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_weather(
    "show_weather", "Show current weather", "T", 2,
    s_weather_checkbox_attrs, WFM_LABEL_AFTER);

char s_fahrenheit_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_fahrenheit(
    "temp_f", "Temperature in Fahrenheit", "T", 2,
    s_fahrenheit_checkbox_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_break_fahrenheit("<br/>");

char s_weather_fix_time_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_weather_fix_time(
    "wx_fix_time", "Flash last weather update time", "T", 2,
    s_weather_fix_time_checkbox_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_hint_weather_fix_time(
    "<small class=\"pr-hint\">Shows the last successful weather update's "
    "date/time in blue, in place of the weather line, for 1 out of every "
    "10 seconds.</small>");

char s_adsb_fix_time_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_adsb_fix_time(
    "adsb_fix_time", "Show last ADS-B update time", "T", 2,
    s_adsb_fix_time_checkbox_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_hint_adsb_fix_time(
    "<small class=\"pr-hint\">Replaces the live clock with the last "
    "successful ADS-B update's date/time, in green.</small>");

WiFiManagerParameter s_param_altitude_offset(
  "alt_offset", "Altitude offset (same unit as Display distances)", "0", kAltitudeOffsetParamLen,
    kAltitudeOffsetInputAttrs);
WiFiManagerParameter s_hint_altitude_offset(
    "<small class=\"pr-hint\">Positive raises detected altitude, negative "
    "lowers it (e.g. correct for a low-lying antenna site).</small>");

char s_alt_filter_enabled_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_alt_filter_enabled(
  "alt_filter_enabled", "Altitude filter enabled", "T", 2,
  s_alt_filter_enabled_checkbox_attrs, WFM_LABEL_AFTER);

char s_alt_filter_under_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_alt_filter_under(
  "alt_filter_under",
  "Hide aircraft below the threshold (uncheck to hide above instead)", "T", 2,
  s_alt_filter_under_checkbox_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_break_alt_filter_under("<br/>");

WiFiManagerParameter s_param_alt_filter_value(
  "alt_filter_value",
  "Threshold altitude (same unit as Display distances)", "0",
  kAltitudeOffsetParamLen, kAltitudeOffsetInputAttrs);
WiFiManagerParameter s_hint_alt_filter(
    "<small class=\"pr-hint\">Hides aircraft above or below this altitude, "
    "depending on the checkbox above.</small>");
// Grays out the threshold checkbox/field while the altitude filter itself
// is disabled, since they have no effect in that state.
WiFiManagerParameter s_script_alt_filter_toggle(
    "<script>(function(){"
    "var en=document.querySelector('[name=alt_filter_enabled]');"
    "var under=document.querySelector('[name=alt_filter_under]');"
    "var val=document.querySelector('[name=alt_filter_value]');"
    "var underLabel=document.querySelector('label[for=alt_filter_under]');"
    "var valLabel=document.querySelector('label[for=alt_filter_value]');"
    "if(!en||!under||!val)return;"
    "function sync(){"
    "var on=en.checked;"
    "under.disabled=!on;val.disabled=!on;"
    "[under,val,underLabel,valLabel].forEach(function(el){"
    "if(el)el.classList.toggle('pr-dim',!on);"
    "});"
    "}"
    "sync();en.addEventListener('change',sync);"
    "})();</script>");

WiFiManagerParameter s_param_interpolation_delay(
    "interp_delay_ms", "Interpolation delay (ms)", "0",
    kInterpolationDelayParamLen, kInterpolationDelayInputAttrs);

char s_adsb_interpolation_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_adsb_interpolation(
  "adsb_interp", "ADS-B interpolation enabled", "T", 2,
  s_adsb_interpolation_checkbox_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_break_adsb_interp("<br/>");

WiFiManagerParameter s_param_interpolation_delay_presets(
  s_interpolation_preset_html);
WiFiManagerParameter s_hint_interp(
    "<small class=\"pr-hint\">Smooths aircraft motion between ADS-B fetches; "
    "higher = smoother but more display latency.</small>");

// Fetches elevation directly from the browser (open-meteo.com) instead of
// routing it through the ESP32's own HTTPS stack: this button only ever
// exists on the /param page, which is only served while the LAN portal is
// active — and the portal itself permanently reserves the largest
// contiguous heap block, so on-device TLS handshakes here are unreliable
// no matter how the retry/threshold logic is tuned (confirmed on hardware:
// SSL "Memory allocation failed" even above a 30KB largest-block gate).
// Doing the fetch client-side sidesteps the constrained heap entirely.
WiFiManagerParameter s_param_altitude_offset_button(
    "<div style=\"margin-top:.5rem\"><button type=\"button\" id=\"autoElevBtn\" "
    "onclick=\"var lat=document.querySelector('[name=radar_lat]');"
    "var lon=document.querySelector('[name=radar_lon]');"
    "var miles=document.querySelector('[name=use_miles]');"
    "var off=document.querySelector('[name=alt_offset]');"
    "var btn=document.getElementById('autoElevBtn');"
    "if(!lat||!lon||!off){return;}"
    "btn.disabled=true;btn.textContent='Fetching...';"
    "var url='https://api.open-meteo.com/v1/forecast?latitude='+encodeURIComponent(lat.value)"
    "+'&longitude='+encodeURIComponent(lon.value)+'&current=temperature_2m&forecast_days=1&timezone=auto';"
    "fetch(url).then(function(r){return r.json();}).then(function(d){"
    "var elevM=d.elevation;"
    "if(typeof elevM!=='number'||isNaN(elevM)){throw new Error('no elevation');}"
    "var offsetFeet=-elevM/0.3048;"
    "var useMiles=miles&&miles.checked;"
    "off.value=(useMiles?offsetFeet:offsetFeet*0.3048).toFixed(1);"
    "btn.textContent='Use location elevation';btn.disabled=false;"
    "}).catch(function(e){"
    "alert('Could not fetch location elevation');"
    "btn.textContent='Use location elevation';btn.disabled=false;"
    "});\">Use location elevation</button></div>");

char s_clock24_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_clock24("clock_24", "Use 24-hour clock", "T", 2,
                                     s_clock24_checkbox_attrs,
                                     WFM_LABEL_AFTER);

char s_time_seconds_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_time_seconds(
  "time_seconds", "Show seconds in clock", "T", 2,
  s_time_seconds_checkbox_attrs, WFM_LABEL_AFTER);

char s_clock_follow_interp_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_clock_follow_interp(
    "clock_follow_interp", "Clock follows interpolation delay", "T", 2,
    s_clock_follow_interp_checkbox_attrs, WFM_LABEL_AFTER);

constexpr char kTextScaleAttrs[] =
    "type=\"range\" min=\"80\" max=\"130\" step=\"5\" "
    "oninput=\"document.getElementById('text_scale_value').value="
    "this.value+'%'\"";
WiFiManagerParameter s_param_text_scale(
    "text_scale", "Radar text size", "110", kTextScaleParamLen,
    kTextScaleAttrs);
WiFiManagerParameter s_param_text_scale_output(
    "<div style=\"text-align:center;margin-top:-5px\">"
    "<output id=\"text_scale_value\" for=\"text_scale\"></output></div>"
    "<script>(function(){var s=document.getElementById('text_scale'),"
    "o=document.getElementById('text_scale_value');"
    "if(s&&o)o.value=s.value+'%';})();</script>");

char s_auto_dim_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_auto_dim(
    "auto_dim", "Auto-dim at night", "T", 2, s_auto_dim_checkbox_attrs,
    WFM_LABEL_AFTER);
WiFiManagerParameter s_hint_auto_dim(
    "<small class=\"pr-hint\">Dims the display automatically between "
    "9 PM and 7 AM local time.</small>");
constexpr int kBrightnessParamLen = 4;
constexpr char kBrightnessAttrs[] =
    "type=\"range\" min=\"20\" max=\"100\" step=\"5\" "
    "oninput=\"document.getElementById('brightness_pct_value').value="
    "this.value+'%'\"";
WiFiManagerParameter s_param_brightness(
    "brightness_pct", "Brightness", "100", kBrightnessParamLen,
    kBrightnessAttrs);
WiFiManagerParameter s_param_brightness_output(
    "<div style=\"text-align:center;margin-top:-5px\">"
    "<output id=\"brightness_pct_value\" for=\"brightness_pct\"></output></div>"
    "<script>(function(){var s=document.getElementById('brightness_pct'),"
    "o=document.getElementById('brightness_pct_value');"
    "if(s&&o)o.value=s.value+'%';})();</script>");

constexpr char kOtaPasswordAttrs[] =
    "type=\"password\" autocomplete=\"new-password\" "
    "placeholder=\"leave blank to keep current\"";
WiFiManagerParameter s_param_ota_password(
    "ota_password", "OTA password (user: admin)", "", kOtaPasswordParamLen,
    kOtaPasswordAttrs);

WiFiManagerParameter s_param_diag_link(
  "<div style=\"margin-top:.75rem\"><a href=\"/diag\">Diagnostics</a></div>");

WiFiManagerParameter s_param_back_link_bottom(
    "<div style=\"margin-top:.75rem\"><a href=\"/\">&larr; Back to menu</a>"
    "</div>");

void refreshCheckboxAttrs(char* attrs, size_t attrs_len, bool checked) {
  snprintf(attrs, attrs_len, "type=\"checkbox\"%s",
           checked ? " checked" : "");
}

int clampInterpolationDelayMs(int value) {
  if (value < services::settings::kInterpolationDelayMinMs) {
    return services::settings::kInterpolationDelayMinMs;
  }
  if (value > services::settings::kInterpolationDelayMaxMs) {
    return services::settings::kInterpolationDelayMaxMs;
  }
  return value;
}

void refreshInterpolationDelayPresetHtml() {
  const int fetch_ms = static_cast<int>(config::kAdsbFetchIntervalMs);
  const int p0 = 0;
  const int p1 = clampInterpolationDelayMs((fetch_ms * 25) / 100);
  const int p2 = clampInterpolationDelayMs((fetch_ms * 45) / 100);
  const int p3 = clampInterpolationDelayMs((fetch_ms * 65) / 100);

  snprintf(
      s_interpolation_preset_html, sizeof(s_interpolation_preset_html),
      "<div style=\"margin-top:.35rem\">"
      "<label for=\"interp_delay_preset\" style=\"font-size:.92em\">"
      "Interpolation preset (from fetch interval %d ms)</label><br>"
      "<select id=\"interp_delay_preset\" style=\"width:100%%;max-width:18rem\" "
      "onchange=\"(function(v){var i=document.querySelector('[name=interp_delay_ms]');"
      "if(i){i.value=v;}})(this.value)\">"
      "<option value=\"%d\">Off (0%%, %d ms)</option>"
      "<option value=\"%d\">Low (25%%, %d ms)</option>"
      "<option value=\"%d\">Medium (45%%, %d ms)</option>"
      "<option value=\"%d\">High (65%%, %d ms)</option>"
      "</select></div>"
      "<script>(function(){"
      "var i=document.querySelector('[name=interp_delay_ms]');"
      "var s=document.getElementById('interp_delay_preset');"
      "if(!i||!s){return;}"
      "var p=[%d,%d,%d,%d];"
      "function nearest(v){"
      "var best=p[0],d=Math.abs(v-p[0]);"
      "for(var n=1;n<p.length;n++){var nd=Math.abs(v-p[n]);if(nd<d){d=nd;best=p[n];}}"
      "return String(best);"
      "}"
      "var sync=function(){var v=parseInt(i.value||'0',10);if(!isNaN(v)){s.value=nearest(v);}};"
      "sync();"
      "i.addEventListener('input',sync);"
      "})();</script>",
      fetch_ms, p0, p0, p1, p1, p2, p2, p3, p3, p0, p1, p2, p3);
}

void refreshRangePresetHtml() {
  snprintf(s_radar_range_idx_default, sizeof(s_radar_range_idx_default), "%u",
           static_cast<unsigned>(ui::radar::rangeIndex()));
  s_param_radar_range_idx.setValue(s_radar_range_idx_default,
                                   sizeof(s_radar_range_idx_default) - 1);

  char options[512] = {};
  size_t pos = 0;
  for (size_t i = 0; i < ui::radar::kRangePresetCount; ++i) {
    char label[16];
    ui::radar::formatRing3Label(label, sizeof(label),
                                ui::radar::kRangePresets[i].ring3_km,
                                ui::radar::useMiles());
    pos += static_cast<size_t>(snprintf(
        options + pos, sizeof(options) - pos, "<option value=\"%u\"%s>%s</option>",
        static_cast<unsigned>(i),
        (i == ui::radar::rangeIndex()) ? " selected" : "", label));
  }

  snprintf(
      s_range_preset_html, sizeof(s_range_preset_html),
      "<div style=\"margin-top:.35rem\">"
      "<label for=\"radar_range_preset\" style=\"font-size:.92em\">"
      "Radar range</label><br>"
      "<select id=\"radar_range_preset\" style=\"width:100%%;max-width:18rem\" "
      "onchange=\"(function(v){var i=document.querySelector('[name=radar_range_idx]');"
      "if(i){i.value=v;}})(this.value)\">%s</select></div>",
      options);
}

void refreshPortalParamDefaults() {
  refreshInterpolationDelayPresetHtml();
  refreshRangePresetHtml();
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  refreshCheckboxAttrs(s_miles_checkbox_attrs,
                       sizeof(s_miles_checkbox_attrs),
                       ui::radar::useMiles());
  s_param_miles.setValue("T", 2);
  refreshCheckboxAttrs(s_runways_checkbox_attrs,
                       sizeof(s_runways_checkbox_attrs),
                       ui::radar::showRunways());
  s_param_runways.setValue("T", 2);
  refreshCheckboxAttrs(s_footer_checkbox_attrs,
                       sizeof(s_footer_checkbox_attrs),
                       services::settings::footerEnabled());
  s_param_footer.setValue("T", 2);
  refreshCheckboxAttrs(s_weather_checkbox_attrs,
                       sizeof(s_weather_checkbox_attrs),
                       services::settings::weatherEnabled());
  s_param_weather.setValue("T", 2);
  refreshCheckboxAttrs(s_fahrenheit_checkbox_attrs,
                       sizeof(s_fahrenheit_checkbox_attrs),
                       services::settings::temperatureFahrenheit());
  s_param_fahrenheit.setValue("T", 2);
  refreshCheckboxAttrs(s_weather_fix_time_checkbox_attrs,
                       sizeof(s_weather_fix_time_checkbox_attrs),
                       services::settings::showLastWeatherFixTime());
  s_param_weather_fix_time.setValue("T", 2);
  refreshCheckboxAttrs(s_adsb_fix_time_checkbox_attrs,
                       sizeof(s_adsb_fix_time_checkbox_attrs),
                       services::settings::showLastAdsbFetchTime());
  s_param_adsb_fix_time.setValue("T", 2);
  char altitude_offset_buf[kAltitudeOffsetParamLen + 1];
  const float altitude_offset = services::units::useImperialDistance()
                                    ? services::settings::altitudeOffsetFeet()
                                    : services::settings::altitudeOffsetFeet() * 0.3048f;
  snprintf(altitude_offset_buf, sizeof(altitude_offset_buf), "%.1f",
           altitude_offset);
  s_param_altitude_offset.setValue(altitude_offset_buf, kAltitudeOffsetParamLen);
  refreshCheckboxAttrs(s_alt_filter_enabled_checkbox_attrs,
                       sizeof(s_alt_filter_enabled_checkbox_attrs),
                       services::settings::altitudeFilterEnabled());
  s_param_alt_filter_enabled.setValue("T", 2);
  refreshCheckboxAttrs(s_alt_filter_under_checkbox_attrs,
                       sizeof(s_alt_filter_under_checkbox_attrs),
                       services::settings::altitudeFilterHideUnder());
  s_param_alt_filter_under.setValue("T", 2);
  char altitude_filter_buf[kAltitudeOffsetParamLen + 1];
  const float altitude_filter_value =
      services::units::useImperialDistance()
          ? services::settings::altitudeFilterThresholdFeet()
          : services::settings::altitudeFilterThresholdFeet() * 0.3048f;
  snprintf(altitude_filter_buf, sizeof(altitude_filter_buf), "%.1f",
           altitude_filter_value);
  s_param_alt_filter_value.setValue(altitude_filter_buf,
                                    kAltitudeOffsetParamLen);
  char interpolation_delay_buf[kInterpolationDelayParamLen + 1];
  snprintf(interpolation_delay_buf, sizeof(interpolation_delay_buf), "%d",
           services::settings::interpolationDelayMs());
  s_param_interpolation_delay.setValue(interpolation_delay_buf,
                                       kInterpolationDelayParamLen);
  refreshCheckboxAttrs(s_adsb_interpolation_checkbox_attrs,
                       sizeof(s_adsb_interpolation_checkbox_attrs),
                       services::settings::adsbInterpolationEnabled());
  s_param_adsb_interpolation.setValue("T", 2);
  refreshCheckboxAttrs(s_clock24_checkbox_attrs,
                       sizeof(s_clock24_checkbox_attrs),
                       services::settings::use24HourClock());
  s_param_clock24.setValue("T", 2);
  refreshCheckboxAttrs(s_time_seconds_checkbox_attrs,
                       sizeof(s_time_seconds_checkbox_attrs),
                       services::settings::showTimeSeconds());
  s_param_time_seconds.setValue("T", 2);
  refreshCheckboxAttrs(s_clock_follow_interp_checkbox_attrs,
                       sizeof(s_clock_follow_interp_checkbox_attrs),
                       services::settings::clockFollowsInterpolationDelay());
  s_param_clock_follow_interp.setValue("T", 2);
  char text_scale_buf[kTextScaleParamLen + 1];
  snprintf(text_scale_buf, sizeof(text_scale_buf), "%d",
           services::settings::textScalePercent());
  s_param_text_scale.setValue(text_scale_buf, kTextScaleParamLen);
  refreshCheckboxAttrs(s_auto_dim_checkbox_attrs,
                       sizeof(s_auto_dim_checkbox_attrs),
                       services::settings::autoDimEnabled());
  s_param_auto_dim.setValue("T", 2);
  char brightness_buf[kBrightnessParamLen + 1];
  snprintf(brightness_buf, sizeof(brightness_buf), "%d",
           services::settings::brightnessPercent());
  s_param_brightness.setValue(brightness_buf, kBrightnessParamLen);
  s_param_ota_password.setValue("", kOtaPasswordParamLen);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
  ui::radar::saveRangeIndexFromPortal(s_param_radar_range_idx.getValue());
  services::settings::saveFromPortal(
      s_param_footer.getValue(), s_param_weather.getValue(),
      s_param_fahrenheit.getValue(), services::units::useImperialDistance(),
      s_param_altitude_offset.getValue(),
      s_param_alt_filter_enabled.getValue(),
      s_param_alt_filter_under.getValue(),
      s_param_alt_filter_value.getValue(),
      s_param_adsb_interpolation.getValue(),
      s_param_interpolation_delay.getValue(),
      s_param_clock24.getValue(),
      s_param_time_seconds.getValue(),
      s_param_clock_follow_interp.getValue(),
      s_param_weather_fix_time.getValue(),
      s_param_adsb_fix_time.getValue(),
      s_param_text_scale.getValue(),
      s_param_auto_dim.getValue(),
      s_param_brightness.getValue(),
      s_param_ota_password.getValue());
}

void savePortalParamsFromRequest(WebServer& web) {
  const String latitude = web.arg("radar_lat");
  const String longitude = web.arg("radar_lon");
  const String miles = web.arg("use_miles");
  const String runways = web.arg("show_runways");
  const String range_idx = web.arg("radar_range_idx");
  const String footer = web.arg("show_footer");
  const String weather = web.arg("show_weather");
  const String fahrenheit = web.arg("temp_f");
  const String altitude_offset = web.arg("alt_offset");
  const String altitude_filter_enabled = web.arg("alt_filter_enabled");
  const String altitude_filter_under = web.arg("alt_filter_under");
  const String altitude_filter_value = web.arg("alt_filter_value");
  const String adsb_interpolation = web.arg("adsb_interp");
  const String interpolation_delay_ms = web.arg("interp_delay_ms");
  const String clock24 = web.arg("clock_24");
  const String time_seconds = web.arg("time_seconds");
  const String clock_follow_interp = web.arg("clock_follow_interp");
  const String weather_fix_time = web.arg("wx_fix_time");
  const String adsb_fix_time = web.arg("adsb_fix_time");
  const String text_scale = web.arg("text_scale");
  const String auto_dim = web.arg("auto_dim");
  const String brightness_pct = web.arg("brightness_pct");
  const String ota_password = web.arg("ota_password");

  if (!services::location::saveFromStrings(latitude.c_str(),
                                           longitude.c_str())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(miles.c_str());
  ui::radar::saveRunwaysFromPortal(runways.c_str());
  ui::radar::saveRangeIndexFromPortal(range_idx.c_str());
  services::settings::saveFromPortal(
      footer.c_str(), weather.c_str(), fahrenheit.c_str(),
      services::units::useImperialDistance(), altitude_offset.c_str(),
      altitude_filter_enabled.c_str(), altitude_filter_under.c_str(),
      altitude_filter_value.c_str(),
      adsb_interpolation.c_str(),
      interpolation_delay_ms.c_str(), clock24.c_str(),
      time_seconds.c_str(),
      clock_follow_interp.c_str(),
      weather_fix_time.c_str(),
      adsb_fix_time.c_str(),
      text_scale.c_str(), auto_dim.c_str(), brightness_pct.c_str(),
      ota_password.c_str());
  refreshPortalParamDefaults();
}

void handleDiagnosticsPage() {
  if (!s_wm.server) {
    return;
  }
  notePortalActivity();
  s_wm.server->send(200, "text/html", diagnosticsHtml());
}

void handleActivityPing() {
  notePortalActivity();
  if (s_wm.server) {
    s_wm.server->send(204, "text/plain", "");
  }
}

void handleExitPortal() {
  if (!s_wm.server) {
    return;
  }
  notePortalActivity();
  s_web_exit_requested = true;
  s_wm.server->sendHeader("Cache-Control",
                          "no-cache, no-store, must-revalidate");
  s_wm.server->send(
      200, "text/html",
      "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>LAN config closed</title>"
      "<style>body{font-family:verdana;text-align:center;margin:0;padding:3rem}"
      ".msg{display:inline-block;min-width:16rem;text-align:left;padding:1.5rem;"
      "border:1px solid #eee;border-left:5px solid #5cb85c;"
      "border-radius:.3rem}</style></head><body>"
      "<div class='msg'><strong>LAN config closed</strong><br>"
      "<small>Returning to radar mode. You can close this tab.</small>"
      "</div></body></html>");
}

void handleSettingsSaved() {
  if (!s_wm.server) {
    return;
  }
  notePortalActivity();

  WebServer& web = *s_wm.server;
  savePortalParamsFromRequest(web);
  web.send(
      200, "text/html",
      "<!doctype html><html lang='en'><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='3;url=/param'>"
      "<title>Setup saved</title>"
      "<style>body{font-family:verdana;text-align:center;margin:0;padding:3rem}"
      ".msg{display:inline-block;min-width:16rem;text-align:left;padding:1.5rem;"
      "border:1px solid #eee;border-left:5px solid #5cb85c;"
      "border-radius:.3rem}a{color:#1fa3ec}</style></head><body>"
      "<div class='msg'><strong>Saved</strong><br>"
      "<small>Returning to Setup in 3 seconds...</small><br><br>"
      "<a href='/param'>Return now</a></div></body></html>");
}

void attachSettingsRoutes() {
  if (!s_wm.server) {
    return;
  }
  // Register before WiFiManager's built-in /paramsave handler so the custom
  // confirmation can redirect back to Setup.
  s_wm.server->on("/paramsave", HTTP_POST, handleSettingsSaved);
  s_wm.server->on("/diag", HTTP_GET, handleDiagnosticsPage);
  s_wm.server->on("/activity", HTTP_GET, handleActivityPing);
  // Claim WiFiManager's built-in /exit before it registers its own: that
  // default handler just tears down its own webserver/mDNS via an internal
  // "abort" flag, leaving our s_lan_portal_wanted state unaware, so
  // wifiLoop() would immediately restart it. Ours instead flags the exit
  // for wifiConsumeWebExitRequest() to act on through the normal toggle path.
  s_wm.server->on("/exit", HTTP_GET, handleExitPortal);
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_style);
  wm.addParameter(&s_script_activity_beacon);
  wm.addParameter(&s_param_back_link_top);

  wm.addParameter(&s_section_location);
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);

  wm.addParameter(&s_section_altitude);
  wm.addParameter(&s_param_altitude_offset);
  wm.addParameter(&s_hint_altitude_offset);
  wm.addParameter(&s_param_altitude_offset_button);
  wm.addParameter(&s_param_alt_filter_enabled);
  wm.addParameter(&s_param_alt_filter_under);
  wm.addParameter(&s_break_alt_filter_under);
  wm.addParameter(&s_param_alt_filter_value);
  wm.addParameter(&s_hint_alt_filter);
  wm.addParameter(&s_script_alt_filter_toggle);

  wm.addParameter(&s_section_display);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_radar_range_idx);
  wm.addParameter(&s_param_range_preset);
  wm.addParameter(&s_param_footer);
  wm.addParameter(&s_param_weather);
  wm.addParameter(&s_param_fahrenheit);
  wm.addParameter(&s_break_fahrenheit);
  wm.addParameter(&s_param_weather_fix_time);
  wm.addParameter(&s_hint_weather_fix_time);
  wm.addParameter(&s_param_adsb_fix_time);
  wm.addParameter(&s_hint_adsb_fix_time);
  wm.addParameter(&s_param_text_scale);
  wm.addParameter(&s_param_text_scale_output);
  wm.addParameter(&s_param_auto_dim);
  wm.addParameter(&s_hint_auto_dim);
  wm.addParameter(&s_param_brightness);
  wm.addParameter(&s_param_brightness_output);

  wm.addParameter(&s_section_clock);
  wm.addParameter(&s_param_clock24);
  wm.addParameter(&s_param_time_seconds);
  wm.addParameter(&s_param_clock_follow_interp);

  wm.addParameter(&s_section_adsb);
  wm.addParameter(&s_param_adsb_interpolation);
  wm.addParameter(&s_break_adsb_interp);
  wm.addParameter(&s_param_interpolation_delay);
  wm.addParameter(&s_param_interpolation_delay_presets);
  wm.addParameter(&s_hint_interp);

  wm.addParameter(&s_section_advanced);
  wm.addParameter(&s_param_ota_password);
  wm.addParameter(&s_param_diag_link);
  wm.addParameter(&s_param_back_link_bottom);

  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  services::settings::clear();
  Serial.println("WiFi credentials, location, units, and display settings cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setTitle("Plane Radar");
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  services::ota::configure(s_wm, attachSettingsRoutes, kPortalActivityBeacon);
  s_wm_configured = true;
}

void startLanWebPortal(bool enable_mdns) {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
  s_lan_portal_mdns_active = false;
#ifdef WM_MDNS
  if (enable_mdns) {
    MDNS.end();
    s_lan_portal_mdns_active = MDNS.begin(config::kPortalHostname);
    if (s_lan_portal_mdns_active) {
      MDNS.addService("http", "tcp", 80);
    }
  }
#endif
  s_wm.startWebPortal();
  if (enable_mdns) {
    Serial.printf("LAN config: http://%s.local or http://%s\n",
                  config::kPortalHostname, WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("LAN config: http://%s\n",
                  WiFi.localIP().toString().c_str());
  }
}

void stopLanWebPortal() {
  s_lan_portal_mdns_active = false;
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    esp_task_wdt_reset();
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  if (!storedWifiCredentials()) {
    return false;
  }

  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0) {
    return false;
  }
  const String pass = s_wm.getWiFiPass();
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    esp_task_wdt_reset();
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

bool bootButtonConsumePortalToggle() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool toggle = s_boot_portal_toggle_pending;
  if (toggle) {
    s_boot_portal_toggle_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return toggle;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (s_lan_portal_wanted && !s_wm.getWebPortalActive() &&
        !s_wm.getConfigPortalActive()) {
      // Keep mDNS off if this (re)start happens while the boot auto-window
      // is still pending, so an unexpected restart during that window
      // can't reintroduce the mDNS heap-fragmentation cost it was
      // specifically started without (see startLanWebPortal()).
      startLanWebPortal(/*enable_mdns=*/!s_boot_auto_portal_pending);
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
    if (s_boot_auto_portal_pending) {
      if (s_last_portal_activity_ms > s_boot_auto_portal_started_ms) {
        // Someone opened the portal: hand off to normal manual control.
        s_boot_auto_portal_pending = false;
      } else if (millis() >= s_boot_auto_portal_deadline_ms) {
        Serial.println(
            "LAN portal: boot auto-window expired unused, disabling");
        s_boot_auto_portal_pending = false;
        s_lan_portal_wanted = false;
        stopLanWebPortal();
        s_auto_portal_timeout_pending = true;
      }
    }
  } else {
    stopLanWebPortal();
  }
}

void wifiToggleLanPortal() {
  s_boot_auto_portal_pending = false;
  s_lan_portal_wanted = !s_lan_portal_wanted;
  if (s_lan_portal_wanted) {
    Serial.println("LAN portal: enabled (hold BOOT again to disable)");
    startLanWebPortal();
  } else {
    // WiFiManager's web server permanently fragments the largest
    // contiguous heap block for the rest of the boot session once it has
    // actually been started — stopWebPortal()/MDNS.end() never reclaim it
    // (confirmed on hardware repeatedly: ADS-B's TLS handshake keeps
    // failing with -32512 long after the portal is closed). This happens
    // even when mDNS was never used/attempted at all (a boot-auto-window
    // session, opened without mDNS, that's then manually toggled off) —
    // so key the reboot decision on "was the web server actually running"
    // rather than trying to guess which sub-case is safe. Only a reboot
    // restores it, so force one here rather than leaving ADS-B crippled
    // until the user notices. (The boot auto-window's own unattended
    // timeout-close path calls stopLanWebPortal() directly, bypassing
    // this function entirely, so it never reboots on its own.)
    const bool need_reboot_to_reclaim_heap = s_wm.getWebPortalActive();
    Serial.println("LAN portal: disabled, releasing heap");
    stopLanWebPortal();
    if (need_reboot_to_reclaim_heap) {
      Serial.println("LAN portal: restarting to reclaim fragmented heap");
      statusScreenLanPortalClosing();
      delay(800);
      esp_restart();
    }
  }
}

bool wifiLanPortalActive() { return s_lan_portal_wanted; }

bool wifiLanPortalMdnsActive() { return s_lan_portal_mdns_active; }

bool wifiPauseLanPortal() {
  if (!s_wm.getWebPortalActive()) {
    return false;
  }
  stopLanWebPortal();
  return true;
}

void wifiStartBootAutoPortal() {
  const unsigned long now = millis();
  s_last_portal_activity_ms = now;
  s_boot_auto_portal_started_ms = now;
  s_boot_auto_portal_deadline_ms = now + config::kBootPortalAutoWindowMs;
  s_boot_auto_portal_pending = true;
  s_lan_portal_wanted = true;
  startLanWebPortal(/*enable_mdns=*/false);
  Serial.printf("LAN portal: auto-enabled for %lus after boot\n",
                config::kBootPortalAutoWindowMs / 1000UL);
}

bool wifiConsumeAutoPortalTimeout() {
  if (!s_auto_portal_timeout_pending) {
    return false;
  }
  s_auto_portal_timeout_pending = false;
  return true;
}

bool wifiConsumeWebExitRequest() {
  if (!s_web_exit_requested) {
    return false;
  }
  s_web_exit_requested = false;
  return true;
}

long wifiBootAutoPortalSecondsLeft() {
  if (!s_boot_auto_portal_pending) {
    return -1;
  }
  const unsigned long now = millis();
  if (now >= s_boot_auto_portal_deadline_ms) {
    return 0;
  }
  return static_cast<long>((s_boot_auto_portal_deadline_ms - now + 999UL) /
                           1000UL);
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
