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
  bool has_prev_sample;
  float prev_lat;
  float prev_lon;
  float prev_altitude_ft;
  bool prev_has_altitude;
  bool prev_on_ground;
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

/** True for the duration of fetchUpdate(), including its blocking network
 * I/O — callers driven via PollFn (e.g. a screen redraw) should skip
 * expensive work while this is true so it doesn't steal CPU time from the
 * in-progress socket read. */
bool fetchInProgress();

/** millis() when the last successful fetchUpdate completed. */
unsigned long lastFetchUpdateMs();

/**
 * Closes any kept-alive HTTPS connections (ADS-B fetch and flight-data
 * lookup) that are currently reused between polls, freeing back the large
 * contiguous heap block(s) they hold onto. Other subsystems that need an
 * occasional big TLS handshake of their own (e.g. the weather fetch, which
 * only runs every ~15 minutes) should call this first to get a fair shot at
 * that memory; ADS-B will transparently reconnect on its next poll cycle.
 */
void releasePersistentConnection();

/**
 * Look up one uncached aircraft through ADSBDB. Results are rate-limited and
 * cached. Returns true when a visible route or type changed.
 */
bool enrichOnePending();

}  // namespace services::adsb
