/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/display_settings.h"
#include "services/ota_update.h"
#include "services/radar_location.h"
#include "services/weather_time.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_radar_render_ms = 0;
long g_last_shown_portal_countdown = -2;  // sentinel: never a valid value
unsigned long g_last_portal_countdown_draw_ms = 0;

constexpr unsigned long kRadarRenderIntervalMs = 40UL;  // ~25 FPS target
constexpr unsigned long kPortalCountdownPollMs = 500UL;

void maybeRunMaintenanceReboot() {
  if (services::ota::inProgress() || wifiLanPortalActive()) {
    return;  // never reboot mid-OTA or while the user has the portal open
  }
  if (millis() < config::kMaintenanceRebootIntervalMs) {
    return;
  }
  Serial.println("Maintenance: scheduled restart to clear heap fragmentation");
  statusScreenMaintenanceRestart();
  delay(800);
  esp_restart();
}

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  services::weather::begin();
  ui::radarDisplayDraw();
  g_last_radar_render_ms = millis();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void showLanPortalScreen() {
  const String ip = WiFi.localIP().toString();
  const long countdown = wifiBootAutoPortalSecondsLeft();
  statusScreenLanPortal(ip.c_str(), wifiLanPortalMdnsActive(),
                       static_cast<int>(countdown));
  g_last_shown_portal_countdown = countdown;
  g_last_portal_countdown_draw_ms = millis();
}

/** Call every loop() iteration while the portal is active: redraws the
 * screen only when the countdown's displayed value actually changes (once
 * a second), so it counts down live and disappears the moment activity is
 * detected (wifiBootAutoPortalSecondsLeft() then returns -1). */
void updatePortalCountdownIfDue() {
  const unsigned long now = millis();
  if (now - g_last_portal_countdown_draw_ms < kPortalCountdownPollMs) {
    return;
  }
  if (wifiBootAutoPortalSecondsLeft() == g_last_shown_portal_countdown) {
    return;
  }
  showLanPortalScreen();
}

void onLanPortalToggled() {
  if (wifiLanPortalActive()) {
    // Free the ADS-B/lookup TLS clients' memory immediately rather than
    // waiting for their next natural teardown — the portal needs the
    // large contiguous heap block, and aircraft monitoring is paused
    // anyway while it's on.
    services::adsb::releasePersistentConnection();
    g_radar_visible = false;
    showLanPortalScreen();
  } else if (WiFi.status() == WL_CONNECTED) {
    g_last_adsb_fetch_ms = 0;  // fetch fresh data right away on resume
    showRadarIfConnected();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumePortalToggle()) {
    wifiToggleLanPortal();
    onLanPortalToggled();
    return;
  }
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  services::adsb::fetchUpdate(services::location::lat(),
                              services::location::lon(), fetch_km);
  handleBootButton();
}

void renderRadarIfDue() {
  const unsigned long now = millis();
  if (now - g_last_radar_render_ms < kRadarRenderIntervalMs) {
    return;
  }
  g_last_radar_render_ms = now;
  ui::radarDisplayRefreshAircraft();
}

void onNetworkPoll() {
  wifiLoop();
  if (!g_radar_visible || WiFi.status() != WL_CONNECTED ||
      services::ota::inProgress() || services::adsb::fetchInProgress() ||
      services::weather::fetchInProgress()) {
    // Skip the (relatively expensive) radar redraw while an ADS-B or weather
    // fetch's own blocking socket read is in progress: this callback is
    // invoked on every iteration of that read loop, and a ~25fps redraw
    // competing for CPU time with the read was slow enough to cause the
    // read to time out mid-body on larger responses. Nothing new to show
    // yet anyway — the fetch is about to deliver fresher data very shortly.
    return;
  }
  renderRadarIfDue();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");
  Serial.printf("boot: free heap=%lu largest=%u\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned>(
                    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  esp_task_wdt_init(config::kWatchdogTimeoutSec, /*panic=*/true);
  esp_task_wdt_add(NULL);

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::settings::init();
  services::adsb::setPollFn(onNetworkPoll);
  services::weather::setPollFn(onNetworkPoll);

  if (wifiSetupConnect()) {
    Serial.printf("boot: wifi up, free heap=%lu largest=%u\n",
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned>(
                      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    wifiStartBootAutoPortal();
    onLanPortalToggled();
  }
}

void loop() {
  esp_task_wdt_reset();
  handleBootButton();
  wifiLoop();
  if (wifiConsumeAutoPortalTimeout()) {
    onLanPortalToggled();
  }
  if (wifiConsumeWebExitRequest() && wifiLanPortalActive()) {
    wifiToggleLanPortal();
    onLanPortalToggled();
  }

  if (services::ota::inProgress()) {
    delay(10);
    return;
  }

  if (wifiLanPortalActive()) {
    // Aircraft/weather monitoring is paused while the LAN portal is on
    // (see onLanPortalToggled()); nothing else to do here but let the
    // portal's own HTTP handling (driven via wifiLoop() above) run and
    // keep the boot auto-portal countdown (if any) up to date.
    updatePortalCountdownIfDue();
    delay(10);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else {
      if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
        g_last_adsb_fetch_ms = millis();
        fetchAndDrawAircraft();
      }
      services::weather::refreshIfDue(services::location::lat(),
                                      services::location::lon());
      services::adsb::enrichOnePending();
      renderRadarIfDue();
      maybeRunMaintenanceReboot();
    }
  }

  delay(10);
}
