#pragma once

#include <cstddef>

namespace services::weather {

using PollFn = void (*)();

/** Start UTC NTP synchronization. Safe to call after every reconnect. */
void begin();
void setPollFn(PollFn fn);

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

}  // namespace services::weather
