#pragma once

#include <cstddef>

namespace services::weather {

using PollFn = void (*)();

/** Start UTC NTP synchronization. Safe to call after every reconnect. */
void begin();
void setPollFn(PollFn fn);

/** True while refreshIfDue() is blocked inside its own network fetch —
 * mirrors services::adsb::fetchInProgress() so callers can also skip a
 * competing radar redraw during weather's TLS handshake/retries. */
bool fetchInProgress();

/**
 * Refresh current conditions and the location's UTC offset when due.
 * Returns true only when displayable data changed.
 */
bool refreshIfDue(double latitude, double longitude, bool force = false);

bool valid();
bool stale();
int lastHttpStatus();
const char* lastError();
unsigned long lastSuccessAgeSec();
void formatWeatherLine(char* out, size_t out_len, int max_width);
void formatDateTimeLine(char* out, size_t out_len,
						bool include_seconds = false,
						unsigned long display_delay_ms = 0);
/** True once a weather fetch has succeeded at least once this boot. */
bool hasLastFix();
/** Date/time (same format as formatDateTimeLine) of the last successful
 * weather fetch, derived from its age rather than a stored calendar value. */
void formatLastFixDateTimeLine(char* out, size_t out_len,
                               bool include_seconds = false);
/** Date/time (same format as formatDateTimeLine) for an arbitrary millis()
 * timestamp of some other event (e.g. services::adsb::lastFetchUpdateMs()),
 * using this clock's own UTC-offset/sync state. Placeholder dashes if the
 * clock hasn't synced or event_ms is 0 (event never happened). */
void formatDateTimeAtMillis(unsigned long event_ms, char* out, size_t out_len,
                           bool include_seconds = false);
/** Current local hour (0-23), or -1 if the clock hasn't synced yet. */
int currentLocalHour();

}  // namespace services::weather
