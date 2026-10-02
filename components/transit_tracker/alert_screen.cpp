// Alert screen: rotation between the schedule and alerts fetched from a custom endpoint.
#include "transit_tracker.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace transit_tracker {

// Header scrolling (when the header text doesn't fit)
static constexpr int ALERT_SCROLL_SPEED = 20;  // pixels/second
static constexpr int ALERT_SCROLL_IDLE_MS = 1500;

// 7x7 icons. 1 = foreground pixel
static const uint8_t ICON_WARNING[7][7] = {
    {0, 0, 0, 1, 0, 0, 0},  //
    {0, 0, 1, 1, 1, 0, 0},  //
    {0, 0, 1, 0, 1, 0, 0},  //
    {0, 1, 1, 0, 1, 1, 0},  //
    {0, 1, 1, 1, 1, 1, 0},  //
    {1, 1, 1, 0, 1, 1, 1},  //
    {1, 1, 1, 1, 1, 1, 1},  //
};
static const uint8_t ICON_INFO[7][7] = {
    {0, 1, 1, 1, 1, 1, 0},  //
    {1, 1, 1, 0, 1, 1, 1},  //
    {1, 1, 1, 1, 1, 1, 1},  //
    {1, 1, 1, 0, 1, 1, 1},  //
    {1, 1, 1, 0, 1, 1, 1},  //
    {1, 1, 1, 0, 1, 1, 1},  //
    {0, 1, 1, 1, 1, 1, 0},  //
};
static constexpr int ICON_SIZE = 7;

void TransitTracker::set_alerts_url(const std::string &url) {
  if (!url.empty()) this->alerts_configured_ = true;
  this->alert_fetcher_.set_url(url);
  // If the URL is set at runtime (e.g. from a text entity) after setup(), make sure the task runs
  if (this->alerts_configured_ && this->setup_done_) {
    this->alert_fetcher_.start();
  }
}

Color TransitTracker::alert_color_(const Alert &alert) const {
  if (alert.has_color) return alert.color;
  switch (alert.severity) {
    case ALERT_SEVERITY_INFO:
      return this->alerts_info_color_;
    case ALERT_SEVERITY_SEVERE:
      return this->alerts_severe_color_;
    default:
      return this->alerts_warning_color_;
  }
}

int TransitTracker::text_width_(const std::string &text) {
  if (text.empty()) return 0;
  int width, x_offset, baseline, height;
  this->alert_font_()->measure(text.c_str(), &width, &x_offset, &baseline, &height);
  return width;
}

int TransitTracker::alert_header_height_() const {
  int line_height = this->alert_font_()->get_ascender() + this->alert_font_()->get_descender();
  // Leave room for the 7px icon with small fonts
  return std::max(line_height, ICON_SIZE + 1);
}

int TransitTracker::alert_body_lines_() const {
  int line_height = this->alert_font_()->get_ascender() + this->alert_font_()->get_descender();
  return std::max(1, (this->display_->get_height() - this->alert_header_height_()) / line_height);
}

static size_t utf8_char_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

std::string TransitTracker::strip_unsupported_chars_(const std::string &text) {
  // Drops every character the alert font has no glyph for (symbols, pictograms...), which would
  // otherwise be drawn as empty boxes, then collapses the double spaces this leaves behind.
  font::Font *font = this->alert_font_();
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    auto c = static_cast<unsigned char>(text[i]);
    size_t len = utf8_char_len(c);
    if (i + len > text.size()) break;  // truncated UTF-8 sequence

    uint32_t codepoint = len == 1 ? c : c & (0xFF >> (len + 1));
    for (size_t k = 1; k < len; k++) codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);

    bool keep = codepoint == '\n' || font->find_glyph(codepoint) != nullptr;
    if (keep && codepoint == ' ' && (out.empty() || out.back() == ' ' || out.back() == '\n')) keep = false;
    if (keep && codepoint == '\n' && !out.empty() && out.back() == ' ') out.pop_back();
    if (keep) out.append(text, i, len);
    i += len;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
  return out;
}

std::vector<std::string> TransitTracker::wrap_text_(const std::string &text, int max_width) {
  std::vector<std::string> lines;
  size_t para_start = 0;

  while (para_start <= text.size()) {
    size_t para_end = text.find('\n', para_start);
    if (para_end == std::string::npos) para_end = text.size();
    std::string paragraph = text.substr(para_start, para_end - para_start);

    std::string line;
    size_t pos = 0;
    while (pos < paragraph.size()) {
      size_t space = paragraph.find(' ', pos);
      if (space == std::string::npos) space = paragraph.size();
      std::string word = paragraph.substr(pos, space - pos);
      pos = space + 1;
      if (word.empty()) continue;

      std::string candidate = line.empty() ? word : line + " " + word;
      if (this->text_width_(candidate) <= max_width) {
        line = candidate;
        continue;
      }

      if (!line.empty()) {
        lines.push_back(line);
        line.clear();
      }

      if (this->text_width_(word) <= max_width) {
        line = word;
        continue;
      }

      // Word longer than the display: hard-break on UTF-8 character boundaries
      std::string chunk;
      size_t i = 0;
      while (i < word.size()) {
        size_t len = utf8_char_len(static_cast<unsigned char>(word[i]));
        std::string next = chunk + word.substr(i, len);
        if (!chunk.empty() && this->text_width_(next) > max_width) {
          lines.push_back(chunk);
          chunk = word.substr(i, len);
        } else {
          chunk = next;
        }
        i += len;
      }
      line = chunk;
    }
    if (!line.empty()) lines.push_back(line);

    if (para_end == text.size()) break;
    para_start = para_end + 1;
  }

  return lines;
}

void TransitTracker::prepare_alerts_(unsigned long now_ms) {
  auto alerts = this->alert_fetcher_.get_alerts();
  this->alerts_generation_seen_ = this->alert_fetcher_.generation();

  bool had_alerts = !this->prepared_alerts_.empty();
  this->prepared_alerts_.clear();
  this->prepared_alerts_.reserve(alerts.size());

  int width = this->display_->get_width();
  int body_lines = this->alert_body_lines_();

  for (auto &alert : alerts) {
    alert.title = this->strip_unsupported_chars_(alert.title);
    alert.message = this->strip_unsupported_chars_(alert.message);
    alert.route = this->strip_unsupported_chars_(alert.route);

    PreparedAlert prepared;
    std::string body;

    const std::string &severity_title = this->alert_severity_title_(alert.severity);
    if (this->alerts_header_shows_severity_) {
      // "Information" / "Perturbation" in the header, full text in the body
      prepared.header = severity_title;
      body = compose_alert_body(alert.title, alert.message);
    } else if (!alert.message.empty() && !alert.title.empty()) {
      prepared.header = alert.title;
      body = strip_title_prefix(alert.message, alert.title);
    } else {
      prepared.header = severity_title;
      body = alert.message.empty() ? alert.title : alert.message;
    }
    if (!alert.route.empty()) prepared.header = alert.route + " " + prepared.header;

    prepared.lines = this->wrap_text_(body, width - 1);
    int pages = std::max<int>(1, (prepared.lines.size() + body_lines - 1) / body_lines);

    if (this->alerts_draw_lambda_) {
      prepared.duration_ms = this->alerts_alert_duration_ms_;
    } else {
      prepared.duration_ms = std::max<uint32_t>(this->alerts_alert_duration_ms_, pages * this->alerts_page_duration_ms_);
    }

    prepared.alert = std::move(alert);
    this->prepared_alerts_.push_back(std::move(prepared));
  }

  // When alerts appear or change, show them right away instead of waiting for the schedule phase.
  if (!this->prepared_alerts_.empty() || had_alerts) {
    this->alerts_rotation_reset_pending_ = true;
  }
  (void) now_ms;
}

bool TransitTracker::select_alert_(unsigned long now_ms, time_t rtc_now, const PreparedAlert *&alert, int &index,
                                   int &count, uint32_t &elapsed) {
  if (!this->alerts_configured_) return false;

  if (this->alert_fetcher_.generation() != this->alerts_generation_seen_) {
    this->prepare_alerts_(now_ms);
  }

  if (!this->alerts_enabled_ || this->prepared_alerts_.empty()) return false;

  // Alerts currently inside their start/end window
  const PreparedAlert *active[16];
  int active_count = 0;
  uint32_t alerts_total_ms = 0;
  for (const auto &p : this->prepared_alerts_) {
    if (active_count >= 16) break;
    if (!p.alert.is_active_at(rtc_now)) continue;
    active[active_count++] = &p;
    alerts_total_ms += p.duration_ms;
  }
  if (active_count == 0 || alerts_total_ms == 0) return false;

  uint32_t cycle_ms = this->alerts_schedule_duration_ms_ + alerts_total_ms;

  if (this->alerts_rotation_reset_pending_) {
    this->alerts_rotation_reset_pending_ = false;
    // Position the cycle at the beginning of the first alert
    this->alerts_rotation_start_ = now_ms - this->alerts_schedule_duration_ms_;
  }

  uint32_t t = static_cast<uint32_t>(now_ms - this->alerts_rotation_start_) % cycle_ms;
  if (t < this->alerts_schedule_duration_ms_) return false;
  t -= this->alerts_schedule_duration_ms_;

  for (int i = 0; i < active_count; i++) {
    if (t < active[i]->duration_ms) {
      alert = active[i];
      index = i;
      count = active_count;
      elapsed = t;
      return true;
    }
    t -= active[i]->duration_ms;
  }
  return false;
}

void TransitTracker::draw_alert_icon_(int x, int y, AlertSeverity severity, Color color) {
  const uint8_t(*icon)[ICON_SIZE] = severity == ALERT_SEVERITY_INFO ? ICON_INFO : ICON_WARNING;
  for (int row = 0; row < ICON_SIZE; row++) {
    for (int col = 0; col < ICON_SIZE; col++) {
      if (icon[row][col]) this->display_->draw_pixel_at(x + col, y + row, color);
    }
  }
}

void HOT TransitTracker::draw_alert_(const PreparedAlert &prepared, int index, int count, uint32_t elapsed) {
  const Alert &alert = prepared.alert;

  if (this->alerts_draw_lambda_) {
    this->alerts_draw_lambda_(*this->display_, alert, index, count, elapsed);
    return;
  }

  const int width = this->display_->get_width();
  const int height = this->display_->get_height();
  const int line_height = this->alert_font_()->get_ascender() + this->alert_font_()->get_descender();
  const int body_lines = this->alert_body_lines_();
  const Color accent = this->alert_color_(alert);
  const bool filled = this->alerts_header_style_ == ALERT_HEADER_FILLED;
  // Dark text on a lit band is hard to read on LED matrices: use a dimmed band with light text instead.
  const Color header_bg = Color(accent.r * 3 / 10, accent.g * 3 / 10, accent.b * 3 / 10);
  const Color header_fg = filled ? this->alerts_text_color_ : accent;

  const int header_height = this->alert_header_height_();
  int y = std::max(0, (height - header_height - body_lines * line_height) / 2);
  // Vertical offset that centers the header text in the (possibly taller) band
  const int header_text_y = (header_height - line_height + 1) / 2;

  // --- Header: icon, route + title, counter ---
  if (filled) {
    // One extra row of background above the text so it doesn't touch the top edge of the band
    const int band_top = std::max(0, y - 1);
    this->display_->filled_rectangle(0, band_top, width, header_height + (y - band_top), header_bg);
  }

  int icon_y = y + (header_height - ICON_SIZE) / 2;
  this->draw_alert_icon_(1, icon_y, alert.severity, accent);
  int text_start = 1 + ICON_SIZE + 3;

  int text_end = width - 1;
  if (this->alerts_show_counter_ && count > 1) {
    char counter[12];
    snprintf(counter, sizeof(counter), "%d/%d", index + 1, count);
    this->display_->print(width, y + header_text_y, this->alert_font_(), header_fg, display::TextAlign::TOP_RIGHT,
                          counter);
    text_end = width - this->text_width_(counter) - 3;
  }

  int available = text_end - text_start;
  int overflow = this->text_width_(prepared.header) - available;
  int scroll_offset = 0;
  if (overflow > 0) {
    int scroll_ms = overflow * 1000 / ALERT_SCROLL_SPEED;
    int cycle = ALERT_SCROLL_IDLE_MS * 2 + scroll_ms;
    int t = static_cast<int>(elapsed % cycle);
    if (t >= ALERT_SCROLL_IDLE_MS + scroll_ms) {
      scroll_offset = overflow;
    } else if (t > ALERT_SCROLL_IDLE_MS) {
      scroll_offset = (t - ALERT_SCROLL_IDLE_MS) * ALERT_SCROLL_SPEED / 1000;
    }
  }

  this->display_->start_clipping(text_start, y, text_end, y + header_height);
  this->display_->print(text_start - scroll_offset, y + header_text_y, this->alert_font_(), header_fg,
                        display::TextAlign::TOP_LEFT, prepared.header.c_str());
  this->display_->end_clipping();

  if (!filled) {
    // Thin accent line under the header when it isn't filled
    this->display_->horizontal_line(0, y + header_height - 1, width, accent);
  }

  // --- Body: wrapped message, paginated ---
  y += header_height;
  int pages = std::max<int>(1, (prepared.lines.size() + body_lines - 1) / body_lines);
  int page = static_cast<int>(elapsed / this->alerts_page_duration_ms_) % pages;

  for (int i = 0; i < body_lines; i++) {
    size_t line_index = page * body_lines + i;
    if (line_index >= prepared.lines.size()) break;
    this->display_->print(0, y, this->alert_font_(), this->alerts_text_color_, display::TextAlign::TOP_LEFT,
                          prepared.lines[line_index].c_str());
    y += line_height;
  }

  // Page indicator: small dots in the bottom-right corner
  if (pages > 1) {
    for (int p = 0; p < pages && p < 8; p++) {
      int dot_x = width - 1 - (pages - 1 - p) * 2;
      this->display_->draw_pixel_at(dot_x, height - 1, p == page ? accent : Color(0x303030));
    }
  }
}

}  // namespace transit_tracker
}  // namespace esphome
