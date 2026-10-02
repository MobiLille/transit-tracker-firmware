// V'Lille screen: bikes and free docks at the chosen stations, one screen per station.
#include "transit_tracker.h"
#include "vlille_logo.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace transit_tracker {

// Station name scrolling (when the name doesn't fit next to the logo)
static constexpr int VLILLE_SCROLL_SPEED = 20;  // pixels/second
static constexpr int VLILLE_SCROLL_IDLE_MS = 1000;
// Left edge -> margin -> zone reserved for the logo (logo centered in it) -> gap -> text
static constexpr int VLILLE_LOGO_LEFT_MARGIN = 3;
static constexpr int VLILLE_LOGO_ZONE_WIDTH = 27;
static constexpr int VLILLE_LOGO_MARGIN = 4;

static const Color VLILLE_TEXT_COLOR = Color(0xFFFFFF);
static const Color VLILLE_AVAILABLE_COLOR = Color(0x20FF00);
static const Color VLILLE_EMPTY_COLOR = Color(0xFF2020);

void TransitTracker::set_vlille_enabled(bool enabled) {
  if (enabled && !this->vlille_enabled_) {
    // First pass halfway through the interval, so it doesn't land on top of the clock screen
    this->vlille_rotation_start_ = millis() - this->vlille_interval_ms_ / 2;
  }
  this->vlille_enabled_ = enabled;
  this->vlille_fetcher_.set_active(enabled);
}

bool TransitTracker::select_vlille_(unsigned long now_ms, const VlilleStation *&station, uint32_t &elapsed) {
  if (!this->vlille_configured_ || !this->vlille_enabled_ || this->vlille_interval_ms_ == 0) return false;

  if (this->vlille_fetcher_.generation() != this->vlille_generation_seen_) {
    this->vlille_generation_seen_ = this->vlille_fetcher_.generation();
    this->vlille_stations_ = this->vlille_fetcher_.get_stations();
  }
  // Hidden while no station is chosen, before the first data and whenever the API is unreachable
  if (this->vlille_stations_.empty() || !this->vlille_fetcher_.has_data() || !this->vlille_fetcher_.is_available())
    return false;

  uint32_t cycle_ms = this->vlille_interval_ms_ + this->vlille_stations_.size() * this->vlille_duration_ms_;
  uint32_t t = static_cast<uint32_t>(now_ms - this->vlille_rotation_start_) % cycle_ms;
  if (t < this->vlille_interval_ms_) return false;
  t -= this->vlille_interval_ms_;

  station = &this->vlille_stations_[t / this->vlille_duration_ms_];
  elapsed = t % this->vlille_duration_ms_;
  return true;
}

void HOT TransitTracker::draw_vlille_(const VlilleStation &station, uint32_t elapsed) {
  const int width = this->display_->get_width();
  const int height = this->display_->get_height();
  const int line_height = this->font_->get_ascender() + this->font_->get_descender();

  // --- Logo, centered in its zone on the left; intermediate values smooth the edges ---
  const int logo_x = VLILLE_LOGO_LEFT_MARGIN + (VLILLE_LOGO_ZONE_WIDTH - VLILLE_LOGO_WIDTH) / 2;
  const int logo_y = (height - VLILLE_LOGO_HEIGHT) / 2;
  for (int row = 0; row < VLILLE_LOGO_HEIGHT; row++) {
    for (int col = 0; col < VLILLE_LOGO_WIDTH; col++) {
      uint8_t alpha = VLILLE_LOGO[row * VLILLE_LOGO_WIDTH + col];
      if (alpha == 0) continue;
      const Color &c = this->vlille_logo_color_;
      this->display_->draw_pixel_at(logo_x + col, logo_y + row,
                                    Color(c.r * alpha / 255, c.g * alpha / 255, c.b * alpha / 255));
    }
  }

  const int text_x = VLILLE_LOGO_LEFT_MARGIN + VLILLE_LOGO_ZONE_WIDTH + VLILLE_LOGO_MARGIN;
  int y = std::max(0, (height - 3 * line_height) / 2);

  // --- Line 1: station name, scrolled back and forth if too long ---
  const std::string &name = station.found ? station.name : station.query;
  int name_width, x_offset, baseline, text_height;
  this->font_->measure(name.c_str(), &name_width, &x_offset, &baseline, &text_height);
  int overflow = name_width - (width - text_x);
  int scroll_offset = 0;
  if (overflow > 0) {
    int scroll_ms = overflow * 1000 / VLILLE_SCROLL_SPEED;
    int cycle = VLILLE_SCROLL_IDLE_MS * 2 + scroll_ms;
    int t = static_cast<int>(elapsed % cycle);
    if (t >= VLILLE_SCROLL_IDLE_MS + scroll_ms) {
      scroll_offset = overflow;
    } else if (t > VLILLE_SCROLL_IDLE_MS) {
      scroll_offset = (t - VLILLE_SCROLL_IDLE_MS) * VLILLE_SCROLL_SPEED / 1000;
    }
  }
  this->display_->start_clipping(text_x, 0, width, height);
  this->display_->print(text_x - scroll_offset, y, this->font_, VLILLE_TEXT_COLOR, display::TextAlign::TOP_LEFT,
                        name.c_str());
  this->display_->end_clipping();
  y += line_height;

  // --- Lines 2 and 3: bikes and free docks, or the reason they can't be shown ---
  if (!station.found) {
    this->display_->print(text_x, y, this->font_, VLILLE_EMPTY_COLOR, display::TextAlign::TOP_LEFT, "Introuvable");
    return;
  }
  if (!station.online) {
    this->display_->print(text_x, y, this->font_, VLILLE_EMPTY_COLOR, display::TextAlign::TOP_LEFT,
                          "Hors service");
    return;
  }

  struct CountLine {
    int count;
    const char *singular;
    const char *plural;
  };
  const CountLine lines[2] = {{station.bikes, "vélo", "vélos"}, {station.docks, "place", "places"}};
  for (const auto &line : lines) {
    char number[8];
    snprintf(number, sizeof(number), "%d", line.count);
    int number_width;
    this->font_->measure(number, &number_width, &x_offset, &baseline, &text_height);
    this->display_->print(text_x, y, this->font_, line.count > 0 ? VLILLE_AVAILABLE_COLOR : VLILLE_EMPTY_COLOR,
                          display::TextAlign::TOP_LEFT, number);
    this->display_->print(text_x + number_width + 3, y, this->font_, VLILLE_TEXT_COLOR, display::TextAlign::TOP_LEFT,
                          line.count > 1 ? line.plural : line.singular);
    y += line_height;
  }
}

std::string TransitTracker::get_vlille_status() {
  auto stations = this->vlille_fetcher_.get_stations();
  if (stations.empty()) return "Aucune station choisie";
  if (!this->vlille_fetcher_.is_available()) return "API V'Lille indisponible (écran masqué)";
  if (!this->vlille_fetcher_.has_data()) return "Chargement...";

  std::string status;
  for (const auto &s : stations) {
    if (!status.empty()) status += " | ";
    char buf[96];
    if (!s.found) {
      snprintf(buf, sizeof(buf), "\"%s\" introuvable", s.query.c_str());
    } else if (!s.online) {
      snprintf(buf, sizeof(buf), "%s (%ld) : hors service", s.name.c_str(), s.id);
    } else {
      snprintf(buf, sizeof(buf), "%s (%ld) : %d vélos, %d places", s.name.c_str(), s.id, s.bikes, s.docks);
    }
    status += buf;
  }
  // Text sensor states are limited to 255 characters in Home Assistant
  if (status.size() > 255) status = status.substr(0, 252) + "...";
  return status;
}

}  // namespace transit_tracker
}  // namespace esphome
