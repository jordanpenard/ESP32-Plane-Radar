#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_system.h>
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
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
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

void ensureWifiManager();
void startLanWebPortal();
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

WiFiManagerParameter s_param_after_fahrenheit_break("<br/>");

WiFiManagerParameter s_param_altitude_offset(
  "alt_offset", "Altitude offset (same unit as Display distances)", "0", kAltitudeOffsetParamLen,
    kAltitudeOffsetInputAttrs);

WiFiManagerParameter s_param_interpolation_delay(
    "interp_delay_ms", "Interpolation delay (ms)", "0",
    kInterpolationDelayParamLen, kInterpolationDelayInputAttrs);

WiFiManagerParameter s_param_interpolation_delay_presets(
  s_interpolation_preset_html);

WiFiManagerParameter s_param_altitude_offset_button(
    "<div style=\"margin-top:.5rem\"><button type=\"button\" onclick=\"var lat=document.querySelector('[name=radar_lat]');var lon=document.querySelector('[name=radar_lon]');var url='/altitudeoffsetauto';if(lat&&lon){url+='?lat='+encodeURIComponent(lat.value)+'&lon='+encodeURIComponent(lon.value);}window.location=url;\">Use location elevation</button></div>");

char s_clock24_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_clock24("clock_24", "Use 24-hour clock", "T", 2,
                                     s_clock24_checkbox_attrs,
                                     WFM_LABEL_AFTER);

WiFiManagerParameter s_param_after_clock_break("<br/>");

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

constexpr char kOtaPasswordAttrs[] =
    "type=\"password\" autocomplete=\"new-password\" "
    "placeholder=\"leave blank to keep current\"";
WiFiManagerParameter s_param_ota_password(
    "ota_password", "OTA password (user: admin)", "", kOtaPasswordParamLen,
    kOtaPasswordAttrs);

WiFiManagerParameter s_param_diag_link(
  "<div style=\"margin-top:.75rem\"><a href=\"/diag\">Diagnostics</a></div>");

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

void refreshPortalParamDefaults() {
  refreshInterpolationDelayPresetHtml();
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
  s_param_after_fahrenheit_break.setValue("<br/>", 5);
  char altitude_offset_buf[kAltitudeOffsetParamLen + 1];
  const float altitude_offset = services::units::useImperialDistance()
                                    ? services::settings::altitudeOffsetFeet()
                                    : services::settings::altitudeOffsetFeet() * 0.3048f;
  snprintf(altitude_offset_buf, sizeof(altitude_offset_buf), "%.1f",
           altitude_offset);
  s_param_altitude_offset.setValue(altitude_offset_buf, kAltitudeOffsetParamLen);
  char interpolation_delay_buf[kInterpolationDelayParamLen + 1];
  snprintf(interpolation_delay_buf, sizeof(interpolation_delay_buf), "%d",
           services::settings::interpolationDelayMs());
  s_param_interpolation_delay.setValue(interpolation_delay_buf,
                                       kInterpolationDelayParamLen);
  refreshCheckboxAttrs(s_clock24_checkbox_attrs,
                       sizeof(s_clock24_checkbox_attrs),
                       services::settings::use24HourClock());
  s_param_clock24.setValue("T", 2);
  char text_scale_buf[kTextScaleParamLen + 1];
  snprintf(text_scale_buf, sizeof(text_scale_buf), "%d",
           services::settings::textScalePercent());
  s_param_text_scale.setValue(text_scale_buf, kTextScaleParamLen);
  s_param_ota_password.setValue("", kOtaPasswordParamLen);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
  services::settings::saveFromPortal(
      s_param_footer.getValue(), s_param_weather.getValue(),
      s_param_fahrenheit.getValue(), services::units::useImperialDistance(),
      s_param_altitude_offset.getValue(),
      s_param_interpolation_delay.getValue(),
      s_param_clock24.getValue(),
      s_param_text_scale.getValue(),
      s_param_ota_password.getValue());
}

bool parseQueryCoord(const String& value, double* out) {
  if (out == nullptr || value.length() == 0) {
    return false;
  }
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (end == value.c_str() || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = parsed;
  return true;
}

bool validLatLon(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

bool fetchElevationMeters(double latitude, double longitude, float* elevation_m) {
  if (elevation_m == nullptr) {
    return false;
  }

  String url = config::kWeatherApiBase;
  url += "?latitude=";
  url += String(latitude, 6);
  url += "&longitude=";
  url += String(longitude, 6);
  url += "&current=temperature_2m&forecast_days=1&timezone=auto";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("altitude offset: http.begin failed");
    return false;
  }
  http.setConnectTimeout(config::kWeatherRequestTimeoutMs);
  http.setTimeout(config::kWeatherRequestTimeoutMs);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("altitude offset: HTTP %d\n", code);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("altitude offset: JSON parse error: %s\n", error.c_str());
    return false;
  }

  const float elevation = doc["elevation"] | NAN;
  if (!std::isfinite(elevation)) {
    Serial.println("altitude offset: missing elevation");
    return false;
  }

  *elevation_m = elevation;
  return true;
}

void handleAltitudeOffsetAuto() {
  if (!s_wm.server) {
    return;
  }

  WebServer& web = *s_wm.server;
  double latitude = services::location::lat();
  double longitude = services::location::lon();

  if (web.hasArg("lat") && !parseQueryCoord(web.arg("lat"), &latitude)) {
    web.send(400, "text/plain", "Invalid latitude");
    return;
  }
  if (web.hasArg("lon") && !parseQueryCoord(web.arg("lon"), &longitude)) {
    web.send(400, "text/plain", "Invalid longitude");
    return;
  }
  if (!validLatLon(latitude, longitude)) {
    web.send(400, "text/plain", "Invalid latitude/longitude range");
    return;
  }

  float elevation_m = 0.0f;
  if (!fetchElevationMeters(latitude, longitude, &elevation_m)) {
    web.send(502, "text/plain", "Could not fetch location elevation");
    return;
  }

  services::settings::setAltitudeOffsetFeet(-elevation_m / 0.3048f);
  refreshPortalParamDefaults();

  web.sendHeader("Location", "/param", true);
  web.send(302, "text/plain", "");
}

void savePortalParamsFromRequest(WebServer& web) {
  const String latitude = web.arg("radar_lat");
  const String longitude = web.arg("radar_lon");
  const String miles = web.arg("use_miles");
  const String runways = web.arg("show_runways");
  const String footer = web.arg("show_footer");
  const String weather = web.arg("show_weather");
  const String fahrenheit = web.arg("temp_f");
  const String altitude_offset = web.arg("alt_offset");
  const String interpolation_delay_ms = web.arg("interp_delay_ms");
  const String clock24 = web.arg("clock_24");
  const String text_scale = web.arg("text_scale");
  const String ota_password = web.arg("ota_password");

  if (!services::location::saveFromStrings(latitude.c_str(),
                                           longitude.c_str())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(miles.c_str());
  ui::radar::saveRunwaysFromPortal(runways.c_str());
  services::settings::saveFromPortal(
      footer.c_str(), weather.c_str(), fahrenheit.c_str(),
      services::units::useImperialDistance(), altitude_offset.c_str(),
      interpolation_delay_ms.c_str(), clock24.c_str(),
      text_scale.c_str(), ota_password.c_str());
  refreshPortalParamDefaults();
}

void handleDiagnosticsPage() {
  if (!s_wm.server) {
    return;
  }
  s_wm.server->send(200, "text/html", diagnosticsHtml());
}

void handleSettingsSaved() {
  if (!s_wm.server) {
    return;
  }

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
  s_wm.server->on("/altitudeoffsetauto", HTTP_GET, handleAltitudeOffsetAuto);
  s_wm.server->on("/diag", HTTP_GET, handleDiagnosticsPage);
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_footer);
  wm.addParameter(&s_param_weather);
  wm.addParameter(&s_param_fahrenheit);
  wm.addParameter(&s_param_after_fahrenheit_break);
  wm.addParameter(&s_param_altitude_offset);
  wm.addParameter(&s_param_interpolation_delay);
  wm.addParameter(&s_param_interpolation_delay_presets);
  wm.addParameter(&s_param_altitude_offset_button);
  wm.addParameter(&s_param_clock24);
  wm.addParameter(&s_param_after_clock_break);
  wm.addParameter(&s_param_text_scale);
  wm.addParameter(&s_param_text_scale_output);
  wm.addParameter(&s_param_ota_password);
  wm.addParameter(&s_param_diag_link);
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
  services::ota::configure(s_wm, attachSettingsRoutes);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
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
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
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
