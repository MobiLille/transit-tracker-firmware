// Loading screen shown at boot until the first schedule arrives: Mobi logo, current step,
// progress bar and firmware version.
#include "transit_tracker.h"
#include "mobi_logo.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace transit_tracker {

// Left edge -> margin -> logo -> gap -> text column (same proportions as the V'Lille screen)
static constexpr int LOADING_LOGO_X = 3;
static constexpr int LOADING_TEXT_X = 37;
static constexpr int LOADING_LABEL_Y = 3;  // top of the capitals; accents use the 2 rows above
static constexpr int LOADING_BAR_Y = 16;
static constexpr int LOADING_BAR_HEIGHT = 2;
static constexpr int LOADING_BAR_WIDTH = 89;
static constexpr int LOADING_VERSION_Y = 22;

static constexpr int LOADING_STEPS = 5;
static constexpr uint32_t LOADING_FIRST_STEP_MS = 1500;
static constexpr uint32_t LOADING_SWEEP_PERIOD_MS = 1400;
static constexpr int LOADING_SWEEP_HALF_WIDTH = 4;

static const Color LOADING_TEXT_COLOR = Color(0xFFFFFF);
static const Color LOADING_ACCENT_COLOR = Color(0x5B3BFF);  // Mobi violet, brightened for the LEDs
static const Color LOADING_RETRY_COLOR = Color(0xFFA000);
static const Color LOADING_VERSION_COLOR = Color(0x6A6A80);

static const char *const LOADING_LABELS[LOADING_STEPS] = {
    "Démarrage", "Connexion Wi-Fi", "Mise à l'heure", "Accès au serveur", "Chargement",
};

static Color scale_color(const Color &c, float factor) {
  return Color(c.r * factor, c.g * factor, c.b * factor);
}

void HOT TransitTracker::draw_loading_() {
  const unsigned long now = millis();
  if (this->loading_start_ == 0) {
    this->loading_start_ = now == 0 ? 1 : now;
  }

  // Current step; a failed server attempt keeps step 4 but switches to the retry look
  int step;
  bool retry = false;
  if (now - this->loading_start_ < LOADING_FIRST_STEP_MS) {
    step = 0;
  } else if (!network::is_connected()) {
    step = 1;
  } else if (!this->rtc_->now().is_valid()) {
    step = 2;
  } else if (!this->ws_client_.is_connected()) {
    step = 3;
    retry = this->consecutive_disconnects_.load() > 0;
  } else {
    step = 4;
  }

  const Color accent = retry ? LOADING_RETRY_COLOR : LOADING_ACCENT_COLOR;

  // --- Logo, vertically centered; intermediate values smooth the edges ---
  const int logo_y = (this->display_->get_height() - MOBI_LOGO_HEIGHT) / 2;
  for (int row = 0; row < MOBI_LOGO_HEIGHT; row++) {
    for (int col = 0; col < MOBI_LOGO_WIDTH; col++) {
      uint8_t alpha = MOBI_LOGO[row * MOBI_LOGO_WIDTH + col];
      if (alpha == 0) continue;
      this->display_->draw_pixel_at(LOADING_LOGO_X + col, logo_y + row, scale_color(LOADING_TEXT_COLOR, alpha / 255.0f));
    }
  }

  // --- Step label ---
  this->display_->print(LOADING_TEXT_X, LOADING_LABEL_Y, this->font_, LOADING_TEXT_COLOR, display::TextAlign::TOP_LEFT,
                        retry ? "Nouvelle tentative" : LOADING_LABELS[step]);

  // --- Progress bar: one fifth per step, eased, with a highlight sweeping across the filled part ---
  const float target = LOADING_BAR_WIDTH * float(step + 1) / LOADING_STEPS;
  this->loading_bar_shown_ += (target - this->loading_bar_shown_) * 0.12f;
  const int fill = static_cast<int>(this->loading_bar_shown_ + 0.5f);
  const float sweep =
      float(now % LOADING_SWEEP_PERIOD_MS) / LOADING_SWEEP_PERIOD_MS * (fill + 4 * LOADING_SWEEP_HALF_WIDTH) -
      2 * LOADING_SWEEP_HALF_WIDTH;

  for (int x = 0; x < LOADING_BAR_WIDTH; x++) {
    Color c = scale_color(accent, 0.18f);
    if (x < fill) {
      c = accent;
      float d = std::abs(x - sweep);
      if (d < LOADING_SWEEP_HALF_WIDTH) {
        float k = 0.65f * (1.0f - d / LOADING_SWEEP_HALF_WIDTH);
        c = Color(c.r + (255 - c.r) * k, c.g + (255 - c.g) * k, c.b + (255 - c.b) * k);
      }
    }
    for (int dy = 0; dy < LOADING_BAR_HEIGHT; dy++) {
      this->display_->draw_pixel_at(LOADING_TEXT_X + x, LOADING_BAR_Y + dy, c);
    }
  }

  // --- Firmware version ---
#ifdef ESPHOME_PROJECT_VERSION
  this->display_->print(LOADING_TEXT_X, LOADING_VERSION_Y, this->font_, LOADING_VERSION_COLOR,
                        display::TextAlign::TOP_LEFT, ESPHOME_PROJECT_VERSION);
#endif
}

}  // namespace transit_tracker
}  // namespace esphome
