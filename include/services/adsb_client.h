#pragma once

#include <cstddef>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  float altitude_ft;
  float vertical_rate_fpm;
  bool has_altitude;
  bool on_ground;
  char hex[7];
  char callsign[9];
  /** IATA/ICAO origin-destination pair, for example "BOS-IND". */
  char route[10];
  /** Compact detailed model, for example "B737-800". */
  char type[18];
  char alt[12];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/** millis() when the last successful fetchUpdate completed. */
unsigned long lastFetchUpdateMs();

/**
 * Look up one uncached aircraft through ADSBDB. Results are rate-limited and
 * cached. Returns true when a visible route or type changed.
 */
bool enrichOnePending();

}  // namespace services::adsb
