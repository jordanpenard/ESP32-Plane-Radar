#pragma once

void statusScreenPortal();
void statusScreenConnectFailed();
void statusScreenWifiReset();
void statusScreenFirmwareUpdate();
/** Shown right before an automatic restart triggered by closing a
 * manually-opened (mDNS) LAN portal — freeing its permanently fragmented
 * heap block requires a reboot, not just stopping the web server. */
void statusScreenLanPortalClosing();
/** Shown right before the periodic preventive-maintenance reboot (see
 * config::kMaintenanceRebootIntervalMs). */
void statusScreenMaintenanceRestart();
/** Shown while the user has turned on the LAN config web portal (aircraft
 * monitoring is paused during this). `ip` is the device's current local IP
 * as a string, e.g. "192.168.8.181". `mdns_active` selects whether the
 * ".local" hostname line is shown at all (it's only valid once mDNS is
 * actually up — see wifiLanPortalMdnsActive()). `countdown_seconds`, if
 * >= 0, shows a "Config off in Ns" line for the boot auto-portal window
 * (pass -1, the default, once it's been opened manually / activity has
 * been detected).
 */
void statusScreenLanPortal(const char* ip, bool mdns_active,
                          int countdown_seconds = -1);

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();
