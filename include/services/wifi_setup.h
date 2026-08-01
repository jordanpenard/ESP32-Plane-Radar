#pragma once

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
/**
 * Keeps the LAN config web portal (WiFiManager's HTTP server + mDNS) off
 * by default: once started it permanently reserves the large contiguous
 * heap block that ADS-B/weather TLS handshakes need on this 320KB-RAM,
 * no-PSRAM board. Hold BOOT for kBootPortalToggleHoldMs to turn it on when
 * you actually need LAN access; hold again to turn it back off.
 */
void wifiToggleLanPortal();
/** True when the user has turned the LAN config web portal on (via
 * wifiToggleLanPortal()). Callers should pause aircraft/weather polling
 * while this is true, since the portal is holding the large contiguous
 * heap block those TLS handshakes need. */
bool wifiLanPortalActive();
/** True once the currently-running LAN portal actually has mDNS up (not
 * just requested) — false during the boot auto-portal window and until
 * MDNS.begin() succeeds on a manual BOOT-hold toggle. */
bool wifiLanPortalMdnsActive();
/**
 * Temporarily stops the LAN config web portal if it's currently active
 * (e.g. because the user turned it on via wifiToggleLanPortal()), freeing
 * its footprint for a consumer that needs a big contiguous block of its
 * own (e.g. an ADS-B or weather TLS handshake). wifiLoop() restarts it on
 * its next call as long as it's still wanted. Returns true if it was
 * actually running (and thus stopped).
 */
bool wifiPauseLanPortal();
/**
 * Call once right after a successful WiFi connect: auto-enables the LAN
 * config web portal for config::kBootPortalAutoWindowMs so the web UI is
 * reachable without holding BOOT. If no portal activity (a page view or
 * form save) happens within that window, it auto-reverts to normal mode
 * on its own (see wifiConsumeAutoPortalTimeout()).
 */
void wifiStartBootAutoPortal();
/**
 * True exactly once, the loop iteration the boot auto-portal window
 * expired with no activity and the portal was auto-disabled. Callers
 * should treat this like wifiToggleLanPortal() just turned the portal
 * off (e.g. resume the radar view).
 */
bool wifiConsumeAutoPortalTimeout();
/**
 * Whole seconds remaining in the boot auto-portal window, or -1 if that
 * window isn't currently pending (portal activity already detected, the
 * window already timed out, or the portal was opened manually instead).
 */
long wifiBootAutoPortalSecondsLeft();
/**
 * True exactly once, the loop iteration after the user clicked "Exit" in
 * the LAN config web portal. Callers should turn the portal off (e.g. via
 * wifiToggleLanPortal()) and resume the radar view, the same as a manual
 * BOOT-button toggle.
 */
bool wifiConsumeWebExitRequest();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). */
bool bootButtonConsumeTap();
/** Latched medium hold, distinct from a tap or the long-hold WiFi reset;
 * toggles the LAN config web portal via wifiToggleLanPortal(). */
bool bootButtonConsumePortalToggle();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();
