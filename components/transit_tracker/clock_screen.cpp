// Clock screen: date and time shown at a regular interval between the schedule and alerts.
#include "transit_tracker.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace transit_tracker {

static const char *const CLOCK_DAYS[7] = {"Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi"};
static const char *const CLOCK_MONTHS[12] = {"janvier", "février", "mars",      "avril",   "mai",      "juin",
                                             "juillet", "août",    "septembre", "octobre", "novembre", "décembre"};
// Vertical gap between the date and the time
static constexpr int CLOCK_LINE_GAP = 2;
// Length of the enter and exit transitions
static constexpr uint32_t CLOCK_ANIMATION_MS = 400;

static Color scale_color(const Color &color, float factor) {
  return Color(color.r * factor, color.g * factor, color.b * factor);
}

void TransitTracker::set_clock_enabled(bool enabled) {
  if (enabled && !this->clock_enabled_) {
    // Start counting from now so the clock doesn't pop up immediately after being switched on
    this->clock_rotation_start_ = millis();
  }
  this->clock_enabled_ = enabled;
}

bool TransitTracker::select_clock_(unsigned long now_ms, uint32_t &elapsed) const {
  if (!this->clock_enabled_ || this->clock_interval_ms_ == 0) return false;
  uint32_t cycle_ms = this->clock_interval_ms_ + this->clock_duration_ms_;
  uint32_t t = static_cast<uint32_t>(now_ms - this->clock_rotation_start_) % cycle_ms;
  if (t < this->clock_interval_ms_) return false;
  elapsed = t - this->clock_interval_ms_;
  return true;
}

void HOT TransitTracker::draw_clock_(uint32_t elapsed) {
  ESPTime now = this->rtc_->now();

  const char *day = CLOCK_DAYS[(now.day_of_week + 6) % 7];
  const char *month = CLOCK_MONTHS[(now.month + 11) % 12];
  const int width = this->display_->get_width();

  // Abbreviate the day if the date is wider than the display (e.g. "Mercredi 30 septembre" with a large font)
  char date[40];
  snprintf(date, sizeof(date), "%s %d %s", day, now.day_of_month, month);
  int date_width, x_offset, baseline, height;
  this->font_->measure(date, &date_width, &x_offset, &baseline, &height);
  if (date_width > width) {
    snprintf(date, sizeof(date), "%.3s. %d %s", day, now.day_of_month, month);
  }

  char time[6];
  snprintf(time, sizeof(time), "%02d:%02d", now.hour, now.minute);

  const int line_height = this->font_->get_ascender() + this->font_->get_descender();
  const int center_x = width / 2;
  const int display_height = this->display_->get_height();
  const int date_y = std::max(0, (display_height - 2 * line_height - CLOCK_LINE_GAP) / 2);
  const int time_y = date_y + line_height + CLOCK_LINE_GAP;

  // Transition progress: 0 = off screen, 1 = in place. Ramps up on entry and back down before leaving.
  const uint32_t animation_ms = std::min(CLOCK_ANIMATION_MS, this->clock_duration_ms_ / 2);
  const uint32_t remaining = this->clock_duration_ms_ - std::min(elapsed, this->clock_duration_ms_);
  float progress = 1.0f;
  if (this->clock_animation_ != CLOCK_ANIMATION_NONE && animation_ms > 0) {
    progress = std::min(1.0f, static_cast<float>(std::min(elapsed, remaining)) / animation_ms);
  }

  int date_offset = 0, time_offset = 0;
  Color date_color = this->clock_date_color_;
  Color time_color = this->clock_time_color_;
  if (this->clock_animation_ == CLOCK_ANIMATION_SLIDE) {
    // Ease-out: fast start, gentle landing. Date comes from the top, time from the bottom.
    float eased = 1.0f - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
    date_offset = -static_cast<int>((1.0f - eased) * (date_y + line_height));
    time_offset = static_cast<int>((1.0f - eased) * (display_height - time_y));
  } else if (this->clock_animation_ == CLOCK_ANIMATION_FADE) {
    date_color = scale_color(date_color, progress);
    time_color = scale_color(time_color, progress);
  }

  this->display_->print(center_x, date_y + date_offset, this->font_, date_color, display::TextAlign::TOP_CENTER,
                        date);
  this->display_->print(center_x, time_y + time_offset, this->font_, time_color, display::TextAlign::TOP_CENTER,
                        time);
}

}  // namespace transit_tracker
}  // namespace esphome
