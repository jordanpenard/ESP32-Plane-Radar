#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-C6 Super Mini, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;
/** Hold BOOT this long (but under kBootResetHoldMs) to toggle the LAN
 * config web portal on/off. It's off by default: the ESP32-C3 (320KB RAM,
 * no PSRAM) can't spare the large contiguous heap block it permanently
 * reserves once started, which otherwise starves ADS-B/weather TLS
 * handshakes of the memory they need. */
constexpr unsigned long kBootPortalToggleHoldMs = 1200UL;

/** LAN config web portal auto-enables for this long right after boot, so
 * the web UI is reachable without holding BOOT; if no portal activity
 * (a page view or form save) happens within the window, it auto-reverts
 * to normal (radar/ADS-B) mode to free the heap it reserves. Kept long
 * enough (30s) for a human to notice the on-screen IP, unlock their
 * phone/laptop, open a browser and type it in — the portal has no mDNS
 * during this window (see startLanWebPortal()'s enable_mdns param), so
 * only the raw numeric address works here, which takes longer to type
 * than a bookmarked/remembered hostname. */
constexpr unsigned long kBootPortalAutoWindowMs = 30000UL;

// --- Display: GC9B72 2.1" round 360×360 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_7;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_20;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_6;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_22;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_23;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9B72 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = false;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 48.7295;
constexpr double kDefaultRadarLon = 2.3682;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 3000;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;
/**
 * Hard ceiling on the radius actually sent to adsb.fi, regardless of the
 * screen-scaled radius the user's range preset would otherwise request
 * (25km preset can inflate to ~37km / ~20nm to the screen edge). adsb.fi's
 * API has no result-count/limit parameter -- distance is the only lever
 * to bound how many aircraft (and how many bytes) a single fetch can
 * return. In busy airspace a wide radius can return enough aircraft that
 * the response no longer reliably downloads within the read timeout at
 * degraded throughput, and the resulting forced reconnect fragments the
 * heap enough to break other TLS clients (weather) for extended periods.
 */
constexpr float kAdsbMaxFetchRadiusKm = 25.0f;

// --- Flight enrichment (origin/destination and detailed aircraft type) ---
constexpr char kFlightDataApiBase[] = "https://api.adsbdb.com/v0/";
/** One lookup at a time; successful results remain cached for six hours. */
constexpr unsigned long kFlightLookupMinIntervalMs = 750UL;
constexpr unsigned long kFlightLookupTimeoutMs = 5000UL;
constexpr unsigned long kFlightLookupFailureBackoffMs = 30000UL;
constexpr unsigned long kFlightCacheSuccessMs = 6UL * 60UL * 60UL * 1000UL;
constexpr unsigned long kFlightCacheMissMs = 10UL * 60UL * 1000UL;

// --- Weather and local time ---
constexpr char kWeatherApiBase[] = "https://api.open-meteo.com/v1/forecast";
constexpr unsigned long kWeatherFetchIntervalMs = 15UL * 60UL * 1000UL;
constexpr unsigned long kWeatherRequestTimeoutMs = 6000UL;

// --- User-facing defaults ---
constexpr char kOtaUsername[] = "admin";
/** Change this in the web settings before exposing the device to other users. */
constexpr char kDefaultOtaPassword[] = "plane-radar";

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

// --- Reliability ---
/** Task watchdog for the main loop task; a hang longer than this reboots
 * the device instead of leaving it silently unresponsive. Long-but-normal
 * blocking waits (WiFi connect attempts, the WiFi setup captive portal)
 * explicitly feed the watchdog so they aren't mistaken for a hang. */
constexpr uint32_t kWatchdogTimeoutSec = 20UL;
/** Preventive full reboot on this cadence (while idle: not mid-OTA, LAN
 * portal closed) to clear any slow heap fragmentation that accumulates
 * over very long uptimes from sources other than the LAN portal. */
constexpr unsigned long kMaintenanceRebootIntervalMs =
    24UL * 60UL * 60UL * 1000UL;
/** Last-resort self-heal: if the largest free heap block stays below this
 * for kCriticalLargestBlockStreakLimit consecutive ~30s ADS-B diag polls
 * (see maybeLogAdsbDiagnostics()), reboot rather than keep failing every
 * TLS handshake indefinitely. */
constexpr size_t kCriticalLargestFreeBlockBytes = 20000;
constexpr uint16_t kCriticalLargestBlockStreakLimit = 10;
/** Last-resort self-heal, second trigger: the largest-free-block check
 * above assumes failures correlate with a very low largest block, but
 * real hardware has shown mbedTLS handshakes can fail with -32512 for
 * hours straight while stuck at ~32-33KB (well above 20000) -- a value
 * that never recovers on its own once reached. Reboot instead once ADS-B
 * sees this many consecutive real connect failures (~1/60s once the
 * backoff ladder maxes out, so 10 is roughly 10 minutes), regardless of
 * what the heap gate reports. */
constexpr uint8_t kCriticalAdsbConnectFailStreakLimit = 10;

// --- Display auto-dim (night hours use a fixed, dimmer brightness) ---
constexpr int kAutoDimNightStartHour = 21;  // inclusive, local time
constexpr int kAutoDimNightEndHour = 7;     // exclusive, local time
constexpr int kAutoDimNightBrightnessPercent = 30;

// --- Optional interpolation diagnostics (Serial) ---
// Set true temporarily to capture structured interpolation data in monitor.
constexpr bool kInterpolationDebugLogEnabled = false;
// Log cadence when enabled.
constexpr unsigned long kInterpolationDebugLogIntervalMs = 1000UL;
// If non-empty, only log this aircraft key (hex or callsign). Empty = auto.
constexpr char kInterpolationDebugFocusKey[] = "";

}  // namespace config
