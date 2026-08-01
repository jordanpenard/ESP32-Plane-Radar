#include "ui/radar_display.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/display_settings.h"
#include "services/radar_location.h"
#include "services/unit_policy.h"
#include "services/weather_time.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace lgfx_fonts = lgfx::v1::fonts;

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;
uint16_t kColorFooterBackground = 0x0084;
uint16_t kColorWeatherFixTimeStale = 0xF800;
uint16_t kColorAdsbFixTime = 0x2648;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &lgfx_fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &lgfx_fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &lgfx_fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;
bool s_footer_metrics_ready = false;
bool s_footer_use_vlw = false;
float s_footer_vlw_size = 0.36f;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;

char s_cached_scale_label[12] = {};
bool s_cached_scale_label_valid = false;
uint8_t s_cached_range_index = 255;
bool s_cached_scale_use_miles = false;

char s_cached_weather_line[32] = {};
char s_cached_date_time[20] = {};
uint16_t s_cached_weather_line_color = 0;
uint16_t s_cached_date_time_color = 0;
bool s_cached_footer_valid = false;
unsigned long s_cached_footer_ms = 0;
bool s_cached_footer_weather_enabled = false;
bool s_cached_footer_show_seconds = false;
unsigned long s_cached_footer_delay_ms = 0;
bool s_cached_footer_show_last_fix = false;
bool s_cached_footer_show_adsb_time = false;

int s_cached_tag_block_width[services::adsb::kMaxAircraft] = {};
uint32_t s_cached_tag_block_hash[services::adsb::kMaxAircraft] = {};
bool s_cached_tag_block_valid[services::adsb::kMaxAircraft] = {};
int s_cached_tag_text_scale_percent = -1;
bool s_cached_tag_style_vlw = false;

struct AircraftScreenTrack {
  bool valid = false;
  bool has_pos = false;
  char key[9] = {};
  int x = 0;
  int y = 0;
};

struct AircraftDisplayTrack {
  bool valid = false;
  char key[9] = {};
  float lat = 0.0f;
  float lon = 0.0f;
  bool has_altitude = false;
  float altitude_ft = 0.0f;
  unsigned long last_ms = 0;
};

AircraftScreenTrack s_aircraft_screen_tracks[services::adsb::kMaxAircraft] = {};
size_t s_aircraft_track_replace_cursor = 0;
uint8_t s_aircraft_track_range_index = 255;
bool s_aircraft_track_use_miles = false;
AircraftDisplayTrack s_aircraft_display_tracks[services::adsb::kMaxAircraft] = {};
size_t s_aircraft_display_track_replace_cursor = 0;

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

float configuredTextScale() {
  return static_cast<float>(services::settings::textScalePercent()) / 100.0f;
}

void applyBitmapTextScale(lgfx::LGFXBase& gfx) {
  gfx.setTextSize(configuredTextScale());
}

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();
void updateFooterCacheIfNeeded();
void syncAircraftScreenTrackDomain();
void makeAircraftTrackKey(const services::adsb::Aircraft& plane, char* out,
                         size_t out_len);
void smoothAircraftScreenPosition(const services::adsb::Aircraft& plane,
                                  int target_x, int target_y, int* out_x,
                                  int* out_y);
void applyDisplayContinuityFilter(const services::adsb::Aircraft& plane,
                                  float target_lat, float target_lon,
                                  bool has_target_altitude,
                                  float target_altitude_ft, float* out_lat,
                                  float* out_lon, bool* out_has_altitude,
                                  float* out_altitude_ft);
void formatAltitudeFromFeet(float altitude_ft, char* out, size_t out_len);

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {
        &lgfx_fonts::FreeSansBold12pt7b, &lgfx_fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {
        &lgfx_fonts::FreeSansBold9pt7b, &lgfx_fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  applyScaleStyle();
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangePresetCount; ++i) {
    for (bool miles : {false, true}) {
      radar::formatRing3Label(label, sizeof(label), radar::kRangePresets[i].ring3_km,
                              miles);
      const int w = tft.textWidth(label);
      if (w > s_scale_label_max_w) {
        s_scale_label_max_w = w;
      }
    }
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {
        &lgfx_fonts::FreeSansBold12pt7b, &lgfx_fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initFooterMetrics() {
  if (s_footer_metrics_ready) {
    return;
  }
  if (displayFontIsSmooth()) {
    s_footer_use_vlw = true;
    s_footer_vlw_size =
        findVlwSizeForHeight(radar::kFooterLabelHeightPx);
  }
  s_footer_metrics_ready = true;
}

void initPalette() {
  int brightness = services::settings::brightnessPercent();
  if (services::settings::autoDimEnabled()) {
    const int hour = services::weather::currentLocalHour();
    const bool is_night =
        hour >= 0 && (hour >= config::kAutoDimNightStartHour ||
                     hour < config::kAutoDimNightEndHour);
    if (is_night) {
      brightness = config::kAutoDimNightBrightnessPercent;
    }
  }
  auto scale = [brightness](uint8_t v) -> uint8_t {
    return static_cast<uint8_t>((static_cast<int>(v) * brightness) / 100);
  };

  radar::kColorBackground = tft.color565(scale(radar::kBgR), scale(radar::kBgG),
                                         scale(radar::kBgB));
  radar::kColorGrid = tft.color565(scale(radar::kGridR), scale(radar::kGridG),
                                   scale(radar::kGridB));
  radar::kColorLabel = tft.color565(scale(255), scale(255), scale(255));
  radar::kColorCenter = radar::kColorLabel;
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft = tft.color565(
        scale(radar::kAircraftB), scale(radar::kAircraftG), scale(radar::kAircraftR));
  } else {
    radar::kColorAircraft = tft.color565(
        scale(radar::kAircraftR), scale(radar::kAircraftG), scale(radar::kAircraftB));
  }
  radar::kColorTrackVector = tft.color565(
      scale(radar::kTrackR), scale(radar::kTrackG), scale(radar::kTrackB));
  radar::kColorTagType = tft.color565(
      scale(radar::kTagTypeR), scale(radar::kTagTypeG), scale(radar::kTagTypeB));
  radar::kColorTagAltitude = tft.color565(
      scale(radar::kTagAltR), scale(radar::kTagAltG), scale(radar::kTagAltB));
  radar::kColorRunway = tft.color565(
      scale(radar::kRunwayR), scale(radar::kRunwayG), scale(radar::kRunwayB));
  radar::kColorRunwayLabel = tft.color565(scale(radar::kRunwayLabelR),
                                          scale(radar::kRunwayLabelG),
                                          scale(radar::kRunwayLabelB));
  radar::kColorFooterBackground = tft.color565(
      scale(radar::kFooterBgR), scale(radar::kFooterBgG), scale(radar::kFooterBgB));
  radar::kColorWeatherFixTimeStale =
      tft.color565(scale(radar::kWeatherFixTimeStaleR),
                   scale(radar::kWeatherFixTimeStaleG),
                   scale(radar::kWeatherFixTimeStaleB));
  radar::kColorAdsbFixTime = tft.color565(scale(radar::kAdsbFixTimeR),
                                          scale(radar::kAdsbFixTimeG),
                                          scale(radar::kAdsbFixTimeB));
}

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 0.01745329252f;
constexpr unsigned long kInterpolationMaxMs = 4000UL;
constexpr float kExtrapolationGain = 0.45f;
constexpr float kDisplayPositionTauSec = 1.4f;
constexpr float kDisplayAltitudeTauSec = 1.8f;

struct MotionSample {
  bool valid = false;
  unsigned long sample_ms = 0;
  float lat = 0.0f;
  float lon = 0.0f;
  bool has_altitude = false;
  float altitude_ft = 0.0f;
};

struct AircraftMotionHistory {
  bool valid = false;
  char key[9] = {};
  MotionSample samples[3] = {};
  uint8_t next = 0;
  uint8_t count = 0;
};

AircraftMotionHistory s_aircraft_motion_histories[services::adsb::kMaxAircraft] =
    {};
size_t s_aircraft_motion_replace_cursor = 0;
unsigned long s_motion_history_last_fetch_ms = 0;

struct InterpolationDebugState {
  bool header_printed = false;
  bool has_prev = false;
  char key[9] = {};
  unsigned long last_log_ms = 0;
  float prev_lat = 0.0f;
  float prev_lon = 0.0f;
  float prev_alt_ft = 0.0f;
};

InterpolationDebugState s_interp_debug = {};

float lonKmPerDegAtLat(float lat_deg) {
  const float scale = cosf(lat_deg * kDegToRad);
  return kKmPerDeg * std::max(0.20f, fabsf(scale));
}

float median3(float a, float b, float c) {
  if (a > b) {
    const float t = a;
    a = b;
    b = t;
  }
  if (b > c) {
    const float t = b;
    b = c;
    c = t;
  }
  if (a > b) {
    const float t = a;
    a = b;
    b = t;
  }
  return b;
}

float combinedEstimate(const float* values, size_t count, float fallback) {
  if (count == 0 || values == nullptr) {
    return fallback;
  }
  if (count == 1) {
    return values[0];
  }
  if (count == 2) {
    return (values[0] + values[1]) * 0.5f;
  }
  return median3(values[0], values[1], values[2]);
}

AircraftMotionHistory* findMotionHistorySlot(const char* key, bool create) {
  if (key == nullptr || key[0] == '\0') {
    return nullptr;
  }

  for (auto& history : s_aircraft_motion_histories) {
    if (history.valid && strcmp(history.key, key) == 0) {
      return &history;
    }
  }
  if (!create) {
    return nullptr;
  }

  for (auto& history : s_aircraft_motion_histories) {
    if (!history.valid) {
      history = {};
      history.valid = true;
      strncpy(history.key, key, sizeof(history.key) - 1);
      history.key[sizeof(history.key) - 1] = '\0';
      return &history;
    }
  }

  AircraftMotionHistory* slot =
      &s_aircraft_motion_histories[s_aircraft_motion_replace_cursor %
                                   services::adsb::kMaxAircraft];
  s_aircraft_motion_replace_cursor =
      (s_aircraft_motion_replace_cursor + 1) % services::adsb::kMaxAircraft;
  *slot = {};
  slot->valid = true;
  strncpy(slot->key, key, sizeof(slot->key) - 1);
  slot->key[sizeof(slot->key) - 1] = '\0';
  return slot;
}

bool historySampleByAge(const AircraftMotionHistory& history, uint8_t age,
                        MotionSample* out) {
  if (out == nullptr || age >= history.count) {
    return false;
  }
  const int newest_index = (static_cast<int>(history.next) + 2) % 3;
  const int index = (newest_index - static_cast<int>(age) + 3) % 3;
  const MotionSample& sample = history.samples[index];
  if (!sample.valid) {
    return false;
  }
  *out = sample;
  return true;
}

void appendMotionSample(AircraftMotionHistory* history,
                        const services::adsb::Aircraft& plane,
                        unsigned long fetch_ms) {
  if (history == nullptr) {
    return;
  }

  if (history->count > 0) {
    MotionSample latest = {};
    if (historySampleByAge(*history, 0, &latest) &&
        latest.sample_ms == fetch_ms) {
      const int newest_index = (static_cast<int>(history->next) + 2) % 3;
      MotionSample& sample = history->samples[newest_index];
      sample.lat = plane.lat;
      sample.lon = plane.lon;
      sample.has_altitude = plane.has_altitude;
      sample.altitude_ft = plane.altitude_ft;
      return;
    }
  }

  MotionSample& slot = history->samples[history->next % 3];
  slot.valid = true;
  slot.sample_ms = fetch_ms;
  slot.lat = plane.lat;
  slot.lon = plane.lon;
  slot.has_altitude = plane.has_altitude;
  slot.altitude_ft = plane.altitude_ft;

  history->next = (history->next + 1) % 3;
  if (history->count < 3) {
    ++history->count;
  }
}

void updateMotionHistoriesIfNeeded(const services::adsb::Aircraft* planes,
                                   size_t count) {
  const unsigned long fetch_ms = services::adsb::lastFetchUpdateMs();
  if (planes == nullptr || fetch_ms == 0 || count == 0 ||
      fetch_ms == s_motion_history_last_fetch_ms) {
    return;
  }
  s_motion_history_last_fetch_ms = fetch_ms;

  for (size_t i = 0; i < count; ++i) {
    char key[9] = {};
    makeAircraftTrackKey(planes[i], key, sizeof(key));
    AircraftMotionHistory* history = findMotionHistorySlot(key, true);
    appendMotionSample(history, planes[i], fetch_ms);
  }
}

bool estimateMotionKmh(const services::adsb::Aircraft& plane, float* vx_kmh,
                       float* vy_kmh) {
  if (vx_kmh == nullptr || vy_kmh == nullptr) {
    return false;
  }

  float vx_candidates[3] = {};
  float vy_candidates[3] = {};
  size_t candidate_count = 0;

  const float adsb_speed_kmh = std::max(0.0f, plane.gs_knots) * 1.852f;
  const float adsb_track_rad = plane.track_deg * kDegToRad;
  const float adsb_vx_kmh = sinf(adsb_track_rad) * adsb_speed_kmh;
  const float adsb_vy_kmh = cosf(adsb_track_rad) * adsb_speed_kmh;
  vx_candidates[candidate_count] = adsb_vx_kmh;
  vy_candidates[candidate_count] = adsb_vy_kmh;
  ++candidate_count;

  char key[9] = {};
  makeAircraftTrackKey(plane, key, sizeof(key));
  AircraftMotionHistory* history = findMotionHistorySlot(key, false);
  if (history != nullptr && history->count >= 2) {
    MotionSample s0 = {};
    MotionSample s1 = {};
    if (historySampleByAge(*history, 0, &s0) &&
        historySampleByAge(*history, 1, &s1) &&
        s0.sample_ms > s1.sample_ms) {
      const float dt_h =
          std::max(0.0001f,
                   static_cast<float>(s0.sample_ms - s1.sample_ms) / 3600000.0f);
      const float avg_lat = (s0.lat + s1.lat) * 0.5f;
      vx_candidates[candidate_count] =
          ((s0.lon - s1.lon) * lonKmPerDegAtLat(avg_lat)) / dt_h;
      vy_candidates[candidate_count] = ((s0.lat - s1.lat) * kKmPerDeg) / dt_h;
      ++candidate_count;
    }
  }

  if (history != nullptr && history->count >= 3 && candidate_count < 3) {
    MotionSample s1 = {};
    MotionSample s2 = {};
    if (historySampleByAge(*history, 1, &s1) &&
        historySampleByAge(*history, 2, &s2) &&
        s1.sample_ms > s2.sample_ms) {
      const float dt_h =
          std::max(0.0001f,
                   static_cast<float>(s1.sample_ms - s2.sample_ms) / 3600000.0f);
      const float avg_lat = (s1.lat + s2.lat) * 0.5f;
      vx_candidates[candidate_count] =
          ((s1.lon - s2.lon) * lonKmPerDegAtLat(avg_lat)) / dt_h;
      vy_candidates[candidate_count] = ((s1.lat - s2.lat) * kKmPerDeg) / dt_h;
      ++candidate_count;
    }
  }

  if (candidate_count == 1 && adsb_speed_kmh <= 0.0f) {
    *vx_kmh = 0.0f;
    *vy_kmh = 0.0f;
    return false;
  }

  *vx_kmh = combinedEstimate(vx_candidates, candidate_count, adsb_vx_kmh);
  *vy_kmh = combinedEstimate(vy_candidates, candidate_count, adsb_vy_kmh);

  float max_observed_speed = adsb_speed_kmh;
  for (size_t i = 1; i < candidate_count; ++i) {
    const float speed =
        sqrtf(vx_candidates[i] * vx_candidates[i] + vy_candidates[i] * vy_candidates[i]);
    if (speed > max_observed_speed) {
      max_observed_speed = speed;
    }
  }
  const float max_speed = std::max(55.0f, max_observed_speed * 1.30f);
  const float estimate_speed = sqrtf((*vx_kmh) * (*vx_kmh) + (*vy_kmh) * (*vy_kmh));
  if (estimate_speed > max_speed && estimate_speed > 0.01f) {
    const float scale = max_speed / estimate_speed;
    *vx_kmh *= scale;
    *vy_kmh *= scale;
  }
  return true;
}

float effectiveVerticalRateFpm(const services::adsb::Aircraft& plane) {
  if (!plane.has_altitude || plane.on_ground) {
    return 0.0f;
  }

  float candidates[3] = {};
  size_t count = 0;
  candidates[count++] = plane.vertical_rate_fpm;

  char key[9] = {};
  makeAircraftTrackKey(plane, key, sizeof(key));
  AircraftMotionHistory* history = findMotionHistorySlot(key, false);
  if (history != nullptr && history->count >= 2) {
    MotionSample s0 = {};
    MotionSample s1 = {};
    if (historySampleByAge(*history, 0, &s0) &&
        historySampleByAge(*history, 1, &s1) && s0.has_altitude &&
        s1.has_altitude && s0.sample_ms > s1.sample_ms) {
      const float dt_min =
          std::max(0.02f,
                   static_cast<float>(s0.sample_ms - s1.sample_ms) / 60000.0f);
      candidates[count++] = (s0.altitude_ft - s1.altitude_ft) / dt_min;
    }
  }
  if (history != nullptr && history->count >= 3 && count < 3) {
    MotionSample s1 = {};
    MotionSample s2 = {};
    if (historySampleByAge(*history, 1, &s1) &&
        historySampleByAge(*history, 2, &s2) && s1.has_altitude &&
        s2.has_altitude && s1.sample_ms > s2.sample_ms) {
      const float dt_min =
          std::max(0.02f,
                   static_cast<float>(s1.sample_ms - s2.sample_ms) / 60000.0f);
      candidates[count++] = (s1.altitude_ft - s2.altitude_ft) / dt_min;
    }
  }

  return combinedEstimate(candidates, count, plane.vertical_rate_fpm);
}

enum class AltitudeTrend : uint8_t {
  kDown = 0,
  kStable = 1,
  kUp = 2,
};

unsigned long interpolationRawElapsedMs() {
  const unsigned long base_ms = services::adsb::lastFetchUpdateMs();
  if (base_ms == 0) {
    return 0;
  }
  return millis() - base_ms;
}

unsigned long interpolationExtrapolationElapsedMs(unsigned long raw_elapsed_ms) {
  if (!services::settings::adsbInterpolationEnabled()) {
    return 0;
  }
  const unsigned long delay_ms =
      static_cast<unsigned long>(services::settings::interpolationDelayMs());
  if (raw_elapsed_ms <= delay_ms) {
    return 0;
  }
  unsigned long elapsed_ms = raw_elapsed_ms - delay_ms;
  // Limit prediction horizon to a short segment of the fetch interval.
  // This favors continuity over aggressive forward projection.
  const unsigned long adaptive_cap_ms =
      std::min(kInterpolationMaxMs,
               static_cast<unsigned long>(config::kAdsbFetchIntervalMs) / 5UL);
  elapsed_ms = std::min(elapsed_ms, adaptive_cap_ms);
  return elapsed_ms;
}

float interpolationBaseBlendAlpha(const services::adsb::Aircraft& plane,
                                  unsigned long raw_elapsed_ms) {
  if (!services::settings::adsbInterpolationEnabled() ||
      !plane.has_prev_sample) {
    return 1.0f;
  }

  const unsigned long delay_ms =
      static_cast<unsigned long>(services::settings::interpolationDelayMs());
  if (delay_ms == 0 || raw_elapsed_ms >= delay_ms) {
    return 1.0f;
  }

  const float sample_ms =
      std::max(1.0f, static_cast<float>(config::kAdsbFetchIntervalMs));
  const float delayed_offset_ms = sample_ms - static_cast<float>(delay_ms) +
                                  static_cast<float>(raw_elapsed_ms);
  float alpha = delayed_offset_ms / sample_ms;
  alpha = std::max(0.0f, std::min(1.0f, alpha));
  return alpha;
}

bool interpolationDisplayAltitudeFt(const services::adsb::Aircraft& plane,
                                    unsigned long raw_elapsed_ms,
                                    unsigned long extrapolation_elapsed_ms,
                                    float* out_ft) {
  if (out_ft == nullptr || plane.on_ground || !plane.has_altitude) {
    return false;
  }
  const float alpha = interpolationBaseBlendAlpha(plane, raw_elapsed_ms);
  float altitude_ft = plane.altitude_ft;
  if (plane.has_prev_sample && plane.prev_has_altitude) {
    altitude_ft =
        plane.prev_altitude_ft + (plane.altitude_ft - plane.prev_altitude_ft) * alpha;
  }
  altitude_ft += effectiveVerticalRateFpm(plane) *
                 ((static_cast<float>(extrapolation_elapsed_ms) *
                   kExtrapolationGain) /
                  60000.0f);
  altitude_ft += services::settings::altitudeOffsetFeet();
  *out_ft = altitude_ft;
  return true;
}

bool keyMatchesFocusFilter(const char* key) {
  if (config::kInterpolationDebugFocusKey[0] == '\0') {
    return true;
  }
  return key != nullptr && key[0] != '\0' &&
         strcmp(key, config::kInterpolationDebugFocusKey) == 0;
}

void maybeLogInterpolationDebug(const services::adsb::Aircraft& plane,
                                unsigned long raw_elapsed_ms,
                                unsigned long extrapolation_elapsed_ms,
                                float display_lat, float display_lon,
                                float dist_km) {
  if (!config::kInterpolationDebugLogEnabled) {
    return;
  }

  char key[9] = {};
  makeAircraftTrackKey(plane, key, sizeof(key));
  if (!keyMatchesFocusFilter(key)) {
    return;
  }

  const unsigned long now = millis();
  if (s_interp_debug.last_log_ms != 0 &&
      now - s_interp_debug.last_log_ms < config::kInterpolationDebugLogIntervalMs) {
    return;
  }
  s_interp_debug.last_log_ms = now;

  if (!s_interp_debug.header_printed) {
    Serial.println(
        "interpdbg,ms,key,raw_ms,delay_ms,alpha,extrap_ms,dist_km,"
        "prev_lat,cur_lat,disp_lat,prev_lon,cur_lon,disp_lon,"
        "gs_knots,track_deg,vr_fpm,prev_alt_ft,cur_alt_ft,disp_alt_ft,jump_m");
    s_interp_debug.header_printed = true;
  }

  const unsigned long delay_ms =
      static_cast<unsigned long>(services::settings::interpolationDelayMs());
  const float alpha = interpolationBaseBlendAlpha(plane, raw_elapsed_ms);
  const float prev_lat = plane.has_prev_sample ? plane.prev_lat : plane.lat;
  const float prev_lon = plane.has_prev_sample ? plane.prev_lon : plane.lon;
  const float prev_alt_ft =
      (plane.has_prev_sample && plane.prev_has_altitude) ? plane.prev_altitude_ft : plane.altitude_ft;
  float disp_alt_ft = NAN;
  interpolationDisplayAltitudeFt(plane, raw_elapsed_ms, extrapolation_elapsed_ms,
                                 &disp_alt_ft);

  float jump_m = 0.0f;
  if (s_interp_debug.has_prev && strcmp(s_interp_debug.key, key) == 0) {
    const float avg_lat = (display_lat + s_interp_debug.prev_lat) * 0.5f;
    const float dx_km =
        (display_lon - s_interp_debug.prev_lon) * lonKmPerDegAtLat(avg_lat);
    const float dy_km = (display_lat - s_interp_debug.prev_lat) * kKmPerDeg;
    const float dz_m =
        std::isfinite(disp_alt_ft)
            ? (disp_alt_ft - s_interp_debug.prev_alt_ft) * 0.3048f
            : 0.0f;
    jump_m = sqrtf(dx_km * dx_km + dy_km * dy_km) * 1000.0f;
    if (std::isfinite(disp_alt_ft)) {
      jump_m = sqrtf(jump_m * jump_m + dz_m * dz_m);
    }
  }

  Serial.printf(
      "interpdbg,%lu,%s,%lu,%lu,%.3f,%lu,%.3f,"
      "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
      "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f\n",
      static_cast<unsigned long>(now), key,
      static_cast<unsigned long>(raw_elapsed_ms),
      static_cast<unsigned long>(delay_ms), alpha,
      static_cast<unsigned long>(extrapolation_elapsed_ms), dist_km, prev_lat,
      plane.lat, display_lat, prev_lon, plane.lon, display_lon, plane.gs_knots,
      plane.track_deg, plane.vertical_rate_fpm, prev_alt_ft, plane.altitude_ft,
      std::isfinite(disp_alt_ft) ? disp_alt_ft : NAN, jump_m);

  strncpy(s_interp_debug.key, key, sizeof(s_interp_debug.key) - 1);
  s_interp_debug.key[sizeof(s_interp_debug.key) - 1] = '\0';
  s_interp_debug.prev_lat = display_lat;
  s_interp_debug.prev_lon = display_lon;
  s_interp_debug.prev_alt_ft = std::isfinite(disp_alt_ft) ? disp_alt_ft : 0.0f;
  s_interp_debug.has_prev = true;
}

void interpolatedLatLon(const services::adsb::Aircraft& plane,
                        unsigned long raw_elapsed_ms,
                        unsigned long extrapolation_elapsed_ms, float* lat,
                        float* lon) {
  if (lat == nullptr || lon == nullptr) {
    return;
  }

  const float alpha = interpolationBaseBlendAlpha(plane, raw_elapsed_ms);
  if (plane.has_prev_sample) {
    *lat = plane.prev_lat + (plane.lat - plane.prev_lat) * alpha;
    *lon = plane.prev_lon + (plane.lon - plane.prev_lon) * alpha;
  } else {
    *lat = plane.lat;
    *lon = plane.lon;
  }
    const float elapsed_h =
      (static_cast<float>(extrapolation_elapsed_ms) * kExtrapolationGain) /
      3600000.0f;
  if (elapsed_h <= 0.0f) {
    return;
  }

  float vx_kmh = 0.0f;
  float vy_kmh = 0.0f;
  if (!estimateMotionKmh(plane, &vx_kmh, &vy_kmh)) {
    return;
  }

  const float dx_km = vx_kmh * elapsed_h;
  const float dy_km = vy_kmh * elapsed_h;
  *lat += dy_km / kKmPerDeg;
  *lon += dx_km / lonKmPerDegAtLat(*lat);
}

void formatInterpolatedAltitude(const services::adsb::Aircraft& plane,
                                unsigned long raw_elapsed_ms,
                                unsigned long extrapolation_elapsed_ms,
                                char* out,
                                size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';

  if (plane.on_ground) {
    strncpy(out, "GND", out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }
  if (!plane.has_altitude) {
    strncpy(out, plane.alt, out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }

  const float alpha = interpolationBaseBlendAlpha(plane, raw_elapsed_ms);
  float altitude_ft = plane.altitude_ft;
  if (plane.has_prev_sample && plane.prev_has_altitude) {
    altitude_ft =
        plane.prev_altitude_ft + (plane.altitude_ft - plane.prev_altitude_ft) * alpha;
  }

  altitude_ft += effectiveVerticalRateFpm(plane) *
                 ((static_cast<float>(extrapolation_elapsed_ms) *
                   kExtrapolationGain) /
                  60000.0f);
  altitude_ft += services::settings::altitudeOffsetFeet();

  if (services::units::useImperialDistance()) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(altitude_ft)));
    return;
  }
  snprintf(out, out_len, "%d m",
           static_cast<int>(lroundf(altitude_ft * 0.3048f)));
}

AltitudeTrend altitudeTrendState(const services::adsb::Aircraft& plane,
                                 unsigned long raw_elapsed_ms,
                                 unsigned long extrapolation_elapsed_ms) {
  if (plane.on_ground || !plane.has_altitude) {
    return AltitudeTrend::kStable;
  }

  const float alpha = interpolationBaseBlendAlpha(plane, raw_elapsed_ms);
  float current_ft = plane.altitude_ft;
  if (plane.has_prev_sample && plane.prev_has_altitude) {
    current_ft =
        plane.prev_altitude_ft + (plane.altitude_ft - plane.prev_altitude_ft) * alpha;
  }
  const float vertical_rate_fpm = effectiveVerticalRateFpm(plane);
  current_ft += vertical_rate_fpm *
                ((static_cast<float>(extrapolation_elapsed_ms) *
                  kExtrapolationGain) /
                 60000.0f);

  if (plane.has_prev_sample && plane.prev_has_altitude) {
    const float previous_ft = plane.prev_altitude_ft;
    const float delta_ft = current_ft - previous_ft;
    const float denom_ft = std::max(1.0f, fabsf(previous_ft));
    const float relative_change = fabsf(delta_ft) / denom_ft;
    if (relative_change < 0.01f) {
      // Keep 1% stability rule but override when vertical rate is clearly
      // climbing/descending so active changes are not shown as stable.
      if (vertical_rate_fpm > 200.0f) {
        return AltitudeTrend::kUp;
      }
      if (vertical_rate_fpm < -200.0f) {
        return AltitudeTrend::kDown;
      }
      return AltitudeTrend::kStable;
    }
    return delta_ft > 0.0f ? AltitudeTrend::kUp : AltitudeTrend::kDown;
  }

  if (vertical_rate_fpm > 50.0f) {
    return AltitudeTrend::kUp;
  }
  if (vertical_rate_fpm < -50.0f) {
    return AltitudeTrend::kDown;
  }
  return AltitudeTrend::kStable;
}

uint16_t altitudeTrendColor(AltitudeTrend trend) {
  auto panelColor = [](uint8_t r, uint8_t g, uint8_t b) -> uint16_t {
    if (config::kDisplayRgbOrder) {
      return s_draw->color565(b, g, r);
    }
    return s_draw->color565(r, g, b);
  };

  switch (trend) {
    case AltitudeTrend::kUp:
      return panelColor(64, 220, 96);
    case AltitudeTrend::kDown:
      return panelColor(235, 70, 70);
    case AltitudeTrend::kStable:
    default:
      return radar::kColorLabel;
  }
}

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  *dx_km =
      static_cast<float>(lon - services::location::lon()) * kKmPerDeg;
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return distSqFromCenter(x, y) <= max_r * max_r;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y) {
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx,
                           radar::kColorAircraft);
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color) {
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const int base_x =
      cx - static_cast<int>(lroundf(sin_h * static_cast<float>(radar::kAircraftTailLenPx)));
  const int base_y =
      cy + static_cast<int>(lroundf(cos_h * static_cast<float>(radar::kAircraftTailLenPx)));

  const int wing_x = static_cast<int>(lroundf(cos_h * radar::kAircraftTailHalfPx));
  const int wing_y = static_cast<int>(lroundf(sin_h * radar::kAircraftTailHalfPx));

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_tag_vlw_size * configuredTextScale());
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
    applyBitmapTextScale(*s_draw);
  }
}

uint32_t hashTagCString(uint32_t hash, const char* text) {
  constexpr uint32_t kFnvPrime = 16777619u;
  if (text == nullptr) {
    return (hash ^ 0xFFu) * kFnvPrime;
  }
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
       *p != '\0'; ++p) {
    hash ^= static_cast<uint32_t>(*p);
    hash *= kFnvPrime;
  }
  hash ^= 0xFFu;
  hash *= kFnvPrime;
  return hash;
}

uint32_t buildTagContentHash(const services::adsb::Aircraft& plane,
                             const char* altitude_text) {
  uint32_t hash = 2166136261u;
  const char* identity =
      plane.route[0] != '\0' ? plane.route : plane.callsign;
  hash = hashTagCString(hash, identity);
  hash = hashTagCString(hash, plane.type);
  hash = hashTagCString(hash, altitude_text);
  return hash;
}

void invalidateTagWidthCache() {
  memset(s_cached_tag_block_valid, 0, sizeof(s_cached_tag_block_valid));
}

void refreshTagWidthCacheStyle() {
  const int scale_percent = services::settings::textScalePercent();
  if (s_cached_tag_text_scale_percent == scale_percent &&
      s_cached_tag_style_vlw == s_tag_use_vlw) {
    return;
  }
  s_cached_tag_text_scale_percent = scale_percent;
  s_cached_tag_style_vlw = s_tag_use_vlw;
  invalidateTagWidthCache();
}

int measureTagBlockWidth(size_t index, const services::adsb::Aircraft& plane,
                         const char* altitude_text) {
  refreshTagWidthCacheStyle();
  const uint32_t content_hash = buildTagContentHash(plane, altitude_text);
  if (index < services::adsb::kMaxAircraft &&
      s_cached_tag_block_valid[index] &&
      s_cached_tag_block_hash[index] == content_hash) {
    return s_cached_tag_block_width[index];
  }

  int max_w = 0;
  const char* identity =
      plane.route[0] != '\0' ? plane.route : plane.callsign;
  if (identity[0] != '\0') {
    const int w = s_draw->textWidth(identity);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.type[0] != '\0') {
    const int w = s_draw->textWidth(plane.type);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (altitude_text != nullptr && altitude_text[0] != '\0') {
    const int w = s_draw->textWidth(altitude_text);
    if (w > max_w) {
      max_w = w;
    }
  }

  if (index < services::adsb::kMaxAircraft) {
    s_cached_tag_block_hash[index] = content_hash;
    s_cached_tag_block_width[index] = max_w;
    s_cached_tag_block_valid[index] = true;
  }

  return max_w;
}

void drawAircraftTag(size_t index, int x, int y,
                     const services::adsb::Aircraft& plane,
                     const char* altitude_text, uint16_t altitude_color) {
  initTagLabelMetrics();
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  const int block_w = measureTagBlockWidth(index, plane, altitude_text);
  const int block_h = line_h * 3;
  int ly = y - block_h / 2;

  const int symbol_half =
      radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx;
  // West (left): tag toward center on the right; east (right): tag on the left.
  const bool tag_on_right = x < radar::kCenterX;
  int anchor_x = 0;
  if (tag_on_right) {
    anchor_x = x + symbol_half + radar::kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, radar::kSize - block_w - 1);
    s_draw->setTextDatum(textdatum_t::top_left);
  } else {
    anchor_x = x - symbol_half - radar::kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 1);
    s_draw->setTextDatum(textdatum_t::top_right);
  }
  ly = std::max(1, std::min(ly, radar::kSize - block_h - 1));

  const char* identity =
      plane.route[0] != '\0' ? plane.route : plane.callsign;
  if (identity[0] != '\0') {
    s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
    s_draw->drawString(identity, anchor_x, ly);
  }
  ly += line_h;

  if (plane.type[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagType, radar::kColorBackground);
    s_draw->drawString(plane.type, anchor_x, ly);
  }
  ly += line_h;

  if (altitude_text != nullptr && altitude_text[0] != '\0') {
    s_draw->setTextColor(altitude_color, radar::kColorBackground);
    s_draw->drawString(altitude_text, anchor_x, ly);
  }
}

void applyFooterStyle() {
  initFooterMetrics();
  if (s_footer_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_footer_vlw_size * configuredTextScale());
  } else {
    s_draw->setFont(&lgfx_fonts::Font0);
    applyBitmapTextScale(*s_draw);
  }
}

void fitFooterText(const char* source, char* out, size_t out_len,
                   int max_width) {
  if (out_len == 0) {
    return;
  }
  strncpy(out, source != nullptr ? source : "", out_len - 1);
  out[out_len - 1] = '\0';
  if (s_draw->textWidth(out) <= max_width) {
    return;
  }

  size_t length = strlen(out);
  while (length > 3) {
    length -= 1;
    out[length] = '\0';
    if (length >= 3) {
      out[length - 3] = '.';
      out[length - 2] = '.';
      out[length - 1] = '.';
    }
    if (s_draw->textWidth(out) <= max_width) {
      return;
    }
  }
}

void drawFooterLine(const char* text, int y, int max_width, uint16_t color) {
  if (text == nullptr || text[0] == '\0') {
    return;
  }
  applyFooterStyle();
  char fitted[32] = {};
  fitFooterText(text, fitted, sizeof(fitted), max_width);
  s_draw->setTextDatum(textdatum_t::top_center);
  s_draw->setTextColor(color, radar::kColorFooterBackground);
  s_draw->drawString(fitted, radar::kCenterX, y);
}

void drawHealthBadge() {
  if (!services::settings::weatherEnabled()) {
    return;
  }

  const char* badge_text = nullptr;
  uint16_t badge_bg = radar::kColorBackground;
  if (!services::weather::valid()) {
    badge_text = "WX!";
    badge_bg = s_draw->color565(136, 0, 0);
  } else if (services::weather::stale()) {
    badge_text = "STALE";
    badge_bg = s_draw->color565(120, 80, 0);
  }

  if (badge_text == nullptr) {
    return;
  }

  s_draw->setFont(&lgfx_fonts::Font0);
  applyBitmapTextScale(*s_draw);
  const int text_w = s_draw->textWidth(badge_text);
  const int text_h = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;
  const int box_w = text_w + kPadX * 2;
  const int box_h = text_h + kPadY * 2;
  const int x = radar::kSize - box_w - 4;
  const int y = 4;

  s_draw->fillRect(x, y, box_w, box_h, badge_bg);
  s_draw->setTextDatum(textdatum_t::top_left);
  s_draw->setTextColor(radar::kColorLabel, badge_bg);
  s_draw->drawString(badge_text, x + kPadX, y + kPadY);
}

void drawFooter() {
  if (!services::settings::footerEnabled()) {
    return;
  }

  updateFooterCacheIfNeeded();

  // The trapezoid follows the narrowing bottom edge of the round panel.
  s_draw->fillTriangle(28, radar::kFooterTopY, 212, radar::kFooterTopY, 168,
                       radar::kFooterBottomY,
                       radar::kColorFooterBackground);
  s_draw->fillTriangle(28, radar::kFooterTopY, 168, radar::kFooterBottomY, 72,
                       radar::kFooterBottomY,
                       radar::kColorFooterBackground);
  s_draw->drawFastHLine(44, radar::kFooterTopY, 152, radar::kColorGrid);

  if (services::settings::weatherEnabled()) {
    drawFooterLine(s_cached_weather_line, radar::kFooterWeatherY, 176,
                   s_cached_weather_line_color);
  }

  const int time_y = services::settings::weatherEnabled()
                         ? radar::kFooterTimeY
                         : radar::kFooterTimeOnlyY;
  drawFooterLine(s_cached_date_time, time_y, 128, s_cached_date_time_color);
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void drawAircraft() {
  initLabelMetrics();
  const bool interpolation_enabled =
      services::settings::adsbInterpolationEnabled();
  if (interpolation_enabled) {
    syncAircraftScreenTrackDomain();
  }

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  updateMotionHistoriesIfNeeded(planes, n);

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  char altitude_labels[services::adsb::kMaxAircraft][20] = {};
  uint16_t altitude_colors[services::adsb::kMaxAircraft] = {};
  float display_lats[services::adsb::kMaxAircraft] = {};
  float display_lons[services::adsb::kMaxAircraft] = {};
  float display_dist_km[services::adsb::kMaxAircraft] = {};
  size_t draw_count = 0;
  size_t dot_count = 0;
  size_t debug_index = services::adsb::kMaxAircraft;
  float debug_best_dist = 1e9f;
  bool debug_sticky_found = false;
  const unsigned long raw_elapsed_ms = interpolationRawElapsedMs();
  const unsigned long extrapolation_elapsed_ms =
      interpolationExtrapolationElapsedMs(raw_elapsed_ms);

  for (size_t i = 0; i < n; ++i) {
    float lat = 0.0f;
    float lon = 0.0f;
    interpolatedLatLon(planes[i], raw_elapsed_ms, extrapolation_elapsed_ms,
                       &lat, &lon);

    bool has_display_altitude_ft = false;
    float display_altitude_ft = 0.0f;
    has_display_altitude_ft = interpolationDisplayAltitudeFt(
        planes[i], raw_elapsed_ms, extrapolation_elapsed_ms,
        &display_altitude_ft);

    if (interpolation_enabled) {
      float filtered_lat = lat;
      float filtered_lon = lon;
      bool filtered_has_altitude = has_display_altitude_ft;
      float filtered_altitude_ft = display_altitude_ft;
      applyDisplayContinuityFilter(planes[i], lat, lon, has_display_altitude_ft,
                                   display_altitude_ft, &filtered_lat,
                                   &filtered_lon, &filtered_has_altitude,
                                   &filtered_altitude_ft);
      lat = filtered_lat;
      lon = filtered_lon;

      if (planes[i].on_ground) {
        strncpy(altitude_labels[i], "GND", sizeof(altitude_labels[i]) - 1);
        altitude_labels[i][sizeof(altitude_labels[i]) - 1] = '\0';
      } else if (filtered_has_altitude) {
        formatAltitudeFromFeet(filtered_altitude_ft, altitude_labels[i],
                               sizeof(altitude_labels[i]));
      } else {
        strncpy(altitude_labels[i], planes[i].alt, sizeof(altitude_labels[i]) - 1);
        altitude_labels[i][sizeof(altitude_labels[i]) - 1] = '\0';
      }
    } else {
      formatInterpolatedAltitude(planes[i], raw_elapsed_ms,
                                 extrapolation_elapsed_ms, altitude_labels[i],
                                 sizeof(altitude_labels[i]));
    }

    display_lats[i] = lat;
    display_lons[i] = lon;
    altitude_colors[i] = altitudeTrendColor(
      altitudeTrendState(planes[i], raw_elapsed_ms, extrapolation_elapsed_ms));

    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
    display_dist_km[i] = dist_km;
    char debug_key[9] = {};
    makeAircraftTrackKey(planes[i], debug_key, sizeof(debug_key));
    if (config::kInterpolationDebugFocusKey[0] == '\0' &&
        s_interp_debug.has_prev &&
        strcmp(s_interp_debug.key, debug_key) == 0) {
      debug_index = i;
      debug_sticky_found = true;
    } else if (!debug_sticky_found && dist_km < debug_best_dist) {
      debug_best_dist = dist_km;
      debug_index = i;
    }

    if (isInsideOuterRingKm(dist_km)) {
      int target_x = 0;
      int target_y = 0;
      latLonToScreen(lat, lon, &target_x, &target_y);
      int x = target_x;
      int y = target_y;
      if (interpolation_enabled) {
        smoothAircraftScreenPosition(planes[i], target_x, target_y, &x, &y);
      }
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(lat, lon, &dot_x,
                                     &dot_y)) {
      continue;
    }
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = distSqFromCenter(dot_x, dot_y);
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    drawBeyondRingDot(dots[d].x, dots[d].y);
  }

  sortDrawItemsFarFirst(items, draw_count);
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    drawSpeedVector(x, y, planes[i].nose_deg, planes[i].track_deg,
                    planes[i].gs_knots, radar::kColorTrackVector);
    drawHeadingTriangle(x, y, planes[i].nose_deg, radar::kColorAircraft);
  }
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    drawAircraftTag(i, items[d].x, items[d].y, planes[i], altitude_labels[i],
                    altitude_colors[i]);
  }

  if (debug_index < n) {
    maybeLogInterpolationDebug(planes[debug_index], raw_elapsed_ms,
                               extrapolation_elapsed_ms,
                               display_lats[debug_index],
                               display_lons[debug_index],
                               display_dist_km[debug_index]);
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_cardinal_vlw_size * configuredTextScale());
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
    applyBitmapTextScale(*s_draw);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_scale_vlw_size * configuredTextScale());
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
    applyBitmapTextScale(*s_draw);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th / 2 - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2,
                   radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void updateScaleLabelCache() {
  const uint8_t range_index = radar::rangeIndex();
  const bool use_miles = radar::useMiles();
  if (s_cached_scale_label_valid && s_cached_range_index == range_index &&
      s_cached_scale_use_miles == use_miles) {
    return;
  }

  radar::formatCurrentRing3Label(s_cached_scale_label,
                                 sizeof(s_cached_scale_label));
  s_cached_scale_label_valid = true;
  s_cached_range_index = range_index;
  s_cached_scale_use_miles = use_miles;
}

void resetAircraftScreenTracks() {
  memset(s_aircraft_screen_tracks, 0, sizeof(s_aircraft_screen_tracks));
  s_aircraft_track_replace_cursor = 0;
}

void resetAircraftDisplayTracks() {
  memset(s_aircraft_display_tracks, 0, sizeof(s_aircraft_display_tracks));
  s_aircraft_display_track_replace_cursor = 0;
}

void syncAircraftScreenTrackDomain() {
  const uint8_t range_index = radar::rangeIndex();
  const bool use_miles = radar::useMiles();
  if (s_aircraft_track_range_index == range_index &&
      s_aircraft_track_use_miles == use_miles) {
    return;
  }
  s_aircraft_track_range_index = range_index;
  s_aircraft_track_use_miles = use_miles;
  resetAircraftScreenTracks();
  resetAircraftDisplayTracks();
}

void makeAircraftTrackKey(const services::adsb::Aircraft& plane, char* out,
                         size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  out[0] = '\0';
  const char* source = plane.hex[0] != '\0' ? plane.hex : plane.callsign;
  if (source == nullptr || source[0] == '\0') {
    return;
  }
  size_t n = strnlen(source, out_len - 1);
  memcpy(out, source, n);
  out[n] = '\0';
}

AircraftScreenTrack* findAircraftTrackSlot(const char* key) {
  if (key == nullptr || key[0] == '\0') {
    return nullptr;
  }

  for (auto& track : s_aircraft_screen_tracks) {
    if (track.valid && strcmp(track.key, key) == 0) {
      return &track;
    }
  }

  for (auto& track : s_aircraft_screen_tracks) {
    if (!track.valid) {
      strncpy(track.key, key, sizeof(track.key) - 1);
      track.key[sizeof(track.key) - 1] = '\0';
      track.valid = true;
      track.has_pos = false;
      return &track;
    }
  }

  AircraftScreenTrack* slot =
      &s_aircraft_screen_tracks[s_aircraft_track_replace_cursor %
                                services::adsb::kMaxAircraft];
  s_aircraft_track_replace_cursor =
      (s_aircraft_track_replace_cursor + 1) % services::adsb::kMaxAircraft;
  strncpy(slot->key, key, sizeof(slot->key) - 1);
  slot->key[sizeof(slot->key) - 1] = '\0';
  slot->valid = true;
  slot->has_pos = false;
  return slot;
}

AircraftDisplayTrack* findAircraftDisplayTrackSlot(const char* key) {
  if (key == nullptr || key[0] == '\0') {
    return nullptr;
  }

  for (auto& track : s_aircraft_display_tracks) {
    if (track.valid && strcmp(track.key, key) == 0) {
      return &track;
    }
  }

  for (auto& track : s_aircraft_display_tracks) {
    if (!track.valid) {
      track = {};
      track.valid = true;
      strncpy(track.key, key, sizeof(track.key) - 1);
      track.key[sizeof(track.key) - 1] = '\0';
      return &track;
    }
  }

  AircraftDisplayTrack* slot =
      &s_aircraft_display_tracks[s_aircraft_display_track_replace_cursor %
                                 services::adsb::kMaxAircraft];
  s_aircraft_display_track_replace_cursor =
      (s_aircraft_display_track_replace_cursor + 1) % services::adsb::kMaxAircraft;
  *slot = {};
  slot->valid = true;
  strncpy(slot->key, key, sizeof(slot->key) - 1);
  slot->key[sizeof(slot->key) - 1] = '\0';
  return slot;
}

void applyDisplayContinuityFilter(const services::adsb::Aircraft& plane,
                                  float target_lat, float target_lon,
                                  bool has_target_altitude,
                                  float target_altitude_ft, float* out_lat,
                                  float* out_lon, bool* out_has_altitude,
                                  float* out_altitude_ft) {
  if (out_lat == nullptr || out_lon == nullptr || out_has_altitude == nullptr ||
      out_altitude_ft == nullptr) {
    return;
  }

  char key[9] = {};
  makeAircraftTrackKey(plane, key, sizeof(key));
  AircraftDisplayTrack* track = findAircraftDisplayTrackSlot(key);
  if (track == nullptr) {
    *out_lat = target_lat;
    *out_lon = target_lon;
    *out_has_altitude = has_target_altitude;
    *out_altitude_ft = target_altitude_ft;
    return;
  }

  const unsigned long now = millis();
  if (track->last_ms == 0) {
    track->lat = target_lat;
    track->lon = target_lon;
    track->has_altitude = has_target_altitude;
    track->altitude_ft = target_altitude_ft;
    track->last_ms = now;
  } else {
    const unsigned long dt_ms = now - track->last_ms;
    track->last_ms = now;
    const float dt_s = std::max(0.001f, std::min(0.35f, dt_ms / 1000.0f));

    const float pos_gain =
        std::max(0.03f, std::min(0.40f, 1.0f - expf(-dt_s / kDisplayPositionTauSec)));
    track->lat += (target_lat - track->lat) * pos_gain;
    track->lon += (target_lon - track->lon) * pos_gain;

    if (has_target_altitude) {
      if (!track->has_altitude) {
        track->altitude_ft = target_altitude_ft;
        track->has_altitude = true;
      } else {
        const float alt_gain = std::max(
            0.03f, std::min(0.35f, 1.0f - expf(-dt_s / kDisplayAltitudeTauSec)));
        track->altitude_ft += (target_altitude_ft - track->altitude_ft) * alt_gain;
      }
    } else {
      track->has_altitude = false;
    }
  }

  *out_lat = track->lat;
  *out_lon = track->lon;
  *out_has_altitude = track->has_altitude;
  *out_altitude_ft = track->altitude_ft;
}

void formatAltitudeFromFeet(float altitude_ft, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  if (services::units::useImperialDistance()) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(altitude_ft)));
    return;
  }
  snprintf(out, out_len, "%d m",
           static_cast<int>(lroundf(altitude_ft * 0.3048f)));
}

void smoothAircraftScreenPosition(const services::adsb::Aircraft& plane,
                                  int target_x, int target_y, int* out_x,
                                  int* out_y) {
  if (out_x == nullptr || out_y == nullptr) {
    return;
  }

  char key[9] = {};
  makeAircraftTrackKey(plane, key, sizeof(key));
  AircraftScreenTrack* track = findAircraftTrackSlot(key);
  if (track == nullptr) {
    *out_x = target_x;
    *out_y = target_y;
    return;
  }

  if (!track->has_pos) {
    track->x = target_x;
    track->y = target_y;
    track->has_pos = true;
    *out_x = target_x;
    *out_y = target_y;
    return;
  }

  const int dx = target_x - track->x;
  const int dy = target_y - track->y;
  const float dist = sqrtf(static_cast<float>(dx * dx + dy * dy));

  constexpr float kSnapJumpPx = 36.0f;
  constexpr float kMaxStepPx = 6.0f;
  if (dist > kSnapJumpPx || dist <= kMaxStepPx) {
    track->x = target_x;
    track->y = target_y;
  } else {
    const float scale = kMaxStepPx / dist;
    track->x += static_cast<int>(lroundf(dx * scale));
    track->y += static_cast<int>(lroundf(dy * scale));
  }

  *out_x = track->x;
  *out_y = track->y;
}

void updateFooterCacheIfNeeded() {
  const unsigned long now = millis();
  const bool weather_enabled = services::settings::weatherEnabled();
  const bool show_seconds = services::settings::showTimeSeconds();
    const unsigned long display_delay_ms =
      services::settings::clockFollowsInterpolationDelay()
        ? static_cast<unsigned long>(services::settings::interpolationDelayMs())
        : 0UL;
  // Flash the last successful weather fetch's date/time in place of the
  // weather line for 1 out of every 10 seconds, when enabled.
  const bool show_last_fix = weather_enabled &&
      services::settings::showLastWeatherFixTime() &&
      services::weather::hasLastFix() && (now % 10000UL) < 1000UL;
  // Replace the live clock with the last successful ADS-B fetch's date/time
  // (in green), when enabled.
  const bool show_adsb_time = services::settings::showLastAdsbFetchTime() &&
      services::adsb::lastFetchUpdateMs() != 0;
  if (s_cached_footer_valid &&
      s_cached_footer_weather_enabled == weather_enabled &&
      s_cached_footer_show_seconds == show_seconds &&
      s_cached_footer_delay_ms == display_delay_ms &&
      s_cached_footer_show_last_fix == show_last_fix &&
      s_cached_footer_show_adsb_time == show_adsb_time &&
      now - s_cached_footer_ms < 1000UL) {
    return;
  }

  if (weather_enabled) {
    if (show_last_fix) {
      services::weather::formatLastFixDateTimeLine(
          s_cached_weather_line, sizeof(s_cached_weather_line), show_seconds);
      const bool fix_fresh =
          services::weather::valid() && !services::weather::stale() &&
          services::weather::lastSuccessAgeSec() <
              config::kWeatherFetchIntervalMs / 1000UL;
      s_cached_weather_line_color = fix_fresh
                                        ? radar::kColorTagType
                                        : radar::kColorWeatherFixTimeStale;
    } else {
      services::weather::formatWeatherLine(s_cached_weather_line,
                                           sizeof(s_cached_weather_line), 176);
      s_cached_weather_line_color = radar::kColorTagType;
    }
  } else {
    s_cached_weather_line[0] = '\0';
  }

  char with_seconds[24] = {};
  if (show_adsb_time) {
    const unsigned long adsb_fix_ms = services::adsb::lastFetchUpdateMs();
    if (show_seconds) {
      services::weather::formatDateTimeAtMillis(
          adsb_fix_ms, with_seconds, sizeof(with_seconds), true);
      applyFooterStyle();
      if (s_draw->textWidth(with_seconds) <= 128) {
        strncpy(s_cached_date_time, with_seconds, sizeof(s_cached_date_time) - 1);
        s_cached_date_time[sizeof(s_cached_date_time) - 1] = '\0';
      } else {
        services::weather::formatDateTimeAtMillis(
            adsb_fix_ms, s_cached_date_time, sizeof(s_cached_date_time), false);
      }
    } else {
      services::weather::formatDateTimeAtMillis(
          adsb_fix_ms, s_cached_date_time, sizeof(s_cached_date_time), false);
    }
    s_cached_date_time_color = radar::kColorAdsbFixTime;
  } else if (show_seconds) {
    services::weather::formatDateTimeLine(with_seconds, sizeof(with_seconds),
                                          true, display_delay_ms);
    applyFooterStyle();
    if (s_draw->textWidth(with_seconds) <= 128) {
      strncpy(s_cached_date_time, with_seconds, sizeof(s_cached_date_time) - 1);
      s_cached_date_time[sizeof(s_cached_date_time) - 1] = '\0';
    } else {
      services::weather::formatDateTimeLine(s_cached_date_time,
                                            sizeof(s_cached_date_time), false,
                                            display_delay_ms);
    }
    s_cached_date_time_color = radar::kColorTagAltitude;
  } else {
    services::weather::formatDateTimeLine(s_cached_date_time,
                                          sizeof(s_cached_date_time), false,
                                          display_delay_ms);
    s_cached_date_time_color = radar::kColorTagAltitude;
  }
  s_cached_footer_valid = true;
  s_cached_footer_weather_enabled = weather_enabled;
  s_cached_footer_show_seconds = show_seconds;
  s_cached_footer_delay_ms = display_delay_ms;
  s_cached_footer_show_last_fix = show_last_fix;
  s_cached_footer_show_adsb_time = show_adsb_time;
  s_cached_footer_ms = now;
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    s_draw->drawCircle(cx, cy, r - i, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenter);
}

void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  updateScaleLabelCache();
  drawScaleLabelWithBackground(s_cached_scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  initPalette();
  runway::drawLargeAirportRunways(gfx);
  drawCenterDot(cx, cy);
  drawCardinalLabels();
  drawScaleLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  s_frame.setColorDepth(16);
  if (!s_frame.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: frame sprite alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

// Double-buffered frame: composite the grid AND aircraft into the off-screen
// sprite, then blit it to the panel in a single pushSprite. Because the panel
// is updated in one pass, labels never show an erase/redraw gap — no flicker.
void renderFrame() {
  drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)
  {
    const DrawScope scope(s_frame);
    drawAircraft();
    drawFooter();
    drawHealthBadge();
  }
  s_frame.pushSprite(0, 0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  const DrawScope scope(tft);
  drawStaticGrid(tft);
  drawAircraft();
  drawFooter();
  drawHealthBadge();
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  radarDisplayDraw();
}

}  // namespace ui
