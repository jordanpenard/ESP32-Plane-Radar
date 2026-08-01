#pragma once

class WiFiManager;

namespace services::ota {

using AdditionalRoutesFn = void (*)();

/**
 * Add an authenticated firmware page to the WiFiManager portal. The optional
 * callback runs while the portal server is being created, before WiFiManager
 * installs its default routes. `extra_menu_html`, if given, is appended
 * after the firmware update button in the portal's custom menu HTML (shown
 * on the portal's root page).
 */
void configure(WiFiManager& manager,
               AdditionalRoutesFn additional_routes = nullptr,
               const char* extra_menu_html = nullptr);

/** True while an OTA upload is actively writing flash. */
bool inProgress();

}  // namespace services::ota
