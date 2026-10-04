#include "transit_tracker.h"
#include "string_utils.h"

#include <cstdlib>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "mbedtls/sha256.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace transit_tracker {

static const char *const TAG = "transit_tracker.component";

static constexpr int CONNECT_FAILURE_ERROR_THRESHOLD = 3;
static constexpr int CONNECT_FAILURE_REBOOT_THRESHOLD = 15;
static constexpr unsigned long HEARTBEAT_TIMEOUT_MS = 60000;
static constexpr int STALE_TRIP_SECONDS = 60;
static constexpr int CROSS_ICON_SIZE = 7;

static std::string compute_device_id() {
  uint8_t mac[6];
  esphome::get_mac_address_raw(mac);
  uint8_t hash[32];
  mbedtls_sha256(mac, sizeof(mac), hash, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hex + i * 2, 3, "%02x", hash[i]);
  }
  return std::string(hex);
}

static bool parse_hex_color(const std::string &str, uint32_t &out) {
  if (str.empty()) {
    return false;
  }
  char *end = nullptr;
  unsigned long value = std::strtoul(str.c_str(), &end, 16);
  if (end == str.c_str() || *end != '\0') {
    return false;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

void TransitTracker::setup() {
  this->ws_client_.set_on_message([this](const std::string &payload) {
    this->handle_message_(payload);
  });

  this->ws_client_.set_on_connected([this]() {
    // defer the actual subscribe send and status update to loop()
    this->last_heartbeat_ = millis();
    this->has_ever_connected_ = true;
    this->consecutive_disconnects_ = 0;
    this->pending_subscribe_ = true;
  });

  this->ws_client_.set_on_disconnected([this]() {
    this->on_disconnect_();
  });

  {
    std::string user_agent;
    std::string headers;
    bool has_device_id = false;
    for (const auto &h : this->extra_headers_) {
      if (strcasecmp(h.first.c_str(), "user-agent") == 0) {
        user_agent = h.second;
      } else {
        if (strcasecmp(h.first.c_str(), "x-device-id") == 0) {
          has_device_id = true;
        }
        headers += h.first + ": " + h.second + "\r\n";
      }
    }

    if (!has_device_id) {
      auto device_id = compute_device_id();
      if (!device_id.empty()) {
        headers += "X-Device-Id: " + device_id + "\r\n";
      }
    }

    if (user_agent.empty()) {
#ifdef ESPHOME_PROJECT_NAME
      user_agent = ESPHOME_PROJECT_NAME "/" ESPHOME_PROJECT_VERSION
                   " ESPHome/" ESPHOME_VERSION " (" ESPHOME_VARIANT ") esp-idf/" IDF_VER;
#else
      user_agent = "ESPHome/" ESPHOME_VERSION " (" ESPHOME_VARIANT ") esp-idf/" IDF_VER;
#endif
    }

    this->ws_client_.set_user_agent(user_agent);
    this->ws_client_.set_headers(headers);
    this->alert_fetcher_.set_user_agent(user_agent);
    this->vlille_fetcher_.set_user_agent(user_agent);
  }

  // The alerts task is always started once an URL has been configured (in YAML or at runtime);
  // it idles while the URL is empty.
  if (this->alerts_configured_) {
    this->alert_fetcher_.start();
  }
  if (this->vlille_configured_) {
    this->vlille_fetcher_.start();
  }
  this->setup_done_ = true;

  if (this->base_url_.empty()) {
    ESP_LOGW(TAG, "No base URL set; websocket will not start");
  } else {
    this->ws_client_.set_uri(this->base_url_);
    this->ws_client_.start();
  }

  this->set_interval("check_stale_trips", 10000, [this]() {
    if (!this->ws_client_.is_connected()) {
      return;
    }

    auto now = this->rtc_->now();
    if (!now.is_valid()) {
      return;
    }

    bool has_stale_trips = false;
    {
      std::lock_guard<std::mutex> lock(this->schedule_state_.mutex);
      for (const auto &trip : this->schedule_state_.trips) {
        if (now.timestamp - trip.departure_time > STALE_TRIP_SECONDS) {
          has_stale_trips = true;
          break;
        }
      }
    }

    if (has_stale_trips) {
      ESP_LOGW(TAG, "Stale trips detected (rtc=%lld, last_heartbeat=%lu, uptime=%lu)",
               (long long) now.timestamp, this->last_heartbeat_.load(), millis());
    }
  });
}

void TransitTracker::loop() {
  if (this->pending_subscribe_.exchange(false)) {
    this->status_clear_error();
    this->send_subscribe_();
  }

  unsigned long heartbeat = this->last_heartbeat_.load();
  if (heartbeat != 0 && millis() - heartbeat > HEARTBEAT_TIMEOUT_MS) {
    ESP_LOGW(TAG, "No heartbeat for %lu ms (last_heartbeat=%lu, uptime=%lu)",
             millis() - heartbeat, heartbeat, millis());
    this->last_heartbeat_ = 0;
  }
}

void TransitTracker::dump_config() {
  ESP_LOGCONFIG(TAG, "Transit Tracker:");
  ESP_LOGCONFIG(TAG, "  Base URL: %s", this->base_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Schedule: %s", this->schedule_string_.c_str());
  ESP_LOGCONFIG(TAG, "  Limit: %d", this->limit_);
  ESP_LOGCONFIG(TAG, "  List mode: %s", this->list_mode_.c_str());
  ESP_LOGCONFIG(TAG, "  Display departure times: %s", this->display_departure_times_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Scroll Headsigns: %s", this->scroll_headsigns_ ? "true" : "false");
  if (this->alerts_configured_) {
    ESP_LOGCONFIG(TAG, "  Alerts URL: %s", this->alert_fetcher_.get_url().c_str());
    ESP_LOGCONFIG(TAG, "  Alerts rotation: schedule %us, alert %us, page %us",
                  (unsigned) (this->alerts_schedule_duration_ms_ / 1000),
                  (unsigned) (this->alerts_alert_duration_ms_ / 1000),
                  (unsigned) (this->alerts_page_duration_ms_ / 1000));
  }
}

void TransitTracker::reconnect(const char *reason) {
  if (this->fully_closed_) {
    return;
  }

  ESP_LOGI(TAG, "Reconnecting websocket (reason: %s)", reason);
  this->last_heartbeat_ = 0;
  this->ws_client_.stop();

  if (this->base_url_.empty()) {
    ESP_LOGW(TAG, "Base URL is not set - cannot reconnect");
  } else {
    this->ws_client_.set_uri(this->base_url_); 
    this->ws_client_.start();
  }
}

void TransitTracker::close(bool fully) {
  if (fully) {
    this->fully_closed_ = true;
  }
  this->ws_client_.stop();
}

void TransitTracker::on_shutdown() {
  this->cancel_interval("check_stale_trips");
  this->close(true);
  this->alert_fetcher_.stop();
  this->vlille_fetcher_.stop();
}

void TransitTracker::on_disconnect_() {
  if (this->fully_closed_) {
    return;
  }

  int attempts = ++this->consecutive_disconnects_;
  ESP_LOGW(TAG, "Websocket disconnected (consecutive=%d, network_connected=%s, free_heap=%u)",
           attempts, esphome::network::is_connected() ? "yes" : "no",
           static_cast<unsigned>(esp_get_free_heap_size()));

  if (attempts >= CONNECT_FAILURE_ERROR_THRESHOLD) {
    this->status_set_error(LOG_STR("Failed to connect to WebSocket server"));
  }

  if (attempts >= CONNECT_FAILURE_REBOOT_THRESHOLD) {
    ESP_LOGE(TAG, "Could not connect to WebSocket server within %d attempts; rebooting to recover",
             CONNECT_FAILURE_REBOOT_THRESHOLD);
    App.reboot();
  }
}

void TransitTracker::send_subscribe_() {
  auto message = json::build_json([this](JsonObject root) {
    root["event"] = "schedule:subscribe";

    auto data = root["data"].to<JsonObject>();
    if (!this->feed_code_.empty()) {
      data["feedCode"] = this->feed_code_;
    }
    data["routeStopPairs"] = this->schedule_string_;
    data["limit"] = this->limit_;
    data["sortByDeparture"] = this->display_departure_times_;
    data["listMode"] = this->list_mode_;
    if (this->show_cancelled_) {
      // Servers that don't know this option simply ignore it
      data["includeCancelled"] = true;
    }
  });

  ESP_LOGD(TAG, "Subscribing (%u bytes)", static_cast<unsigned>(message.size()));
  ESP_LOGV(TAG, "Subscribe payload: %s", message.c_str());
  if (!this->ws_client_.send_text(message)) {
    ESP_LOGW(TAG, "Subscribe send failed");
  }
}

void TransitTracker::handle_message_(const std::string &payload) {
  ESP_LOGV(TAG, "Received message (%u bytes): %s", static_cast<unsigned>(payload.size()), payload.c_str());

  bool valid = json::parse_json(payload, [this, &payload](JsonObject root) -> bool {
    auto event = root["event"].as<std::string>();

    if (event == "heartbeat") {
      ESP_LOGD(TAG, "Received heartbeat");
      this->last_heartbeat_ = millis();
      return true;
    }

    if (event != "schedule") {
      ESP_LOGW(TAG, "Ignoring unknown event '%s' (%u bytes)", event.c_str(),
               static_cast<unsigned>(payload.size()));
      return true;
    }

    ESP_LOGD(TAG, "Received schedule update (%u bytes)", static_cast<unsigned>(payload.size()));

    std::vector<Trip> new_trips;
    auto data = root["data"].as<JsonObject>();
    auto trip_array = data["trips"].as<JsonArray>();
    new_trips.reserve(trip_array.size());

    for (auto trip : trip_array) {
      std::string headsign = this->apply_abbreviations_(trip["headsign"].as<std::string>());

      auto route_id = trip["routeId"].as<std::string>();
      auto route_style = this->route_styles_.find(route_id);

      Color route_color = this->default_route_color_;
      std::string route_name = trip["routeName"].as<std::string>();

      if (route_style != this->route_styles_.end()) {
        route_color = route_style->second.color;
        route_name = route_style->second.name;
      } else if (!trip["routeColor"].isNull()) {
        auto color_str = trip["routeColor"].as<std::string>();
        uint32_t parsed_color;
        if (parse_hex_color(color_str, parsed_color)) {
          route_color = Color(parsed_color);
        } else if (!color_str.empty()) {
          ESP_LOGW(TAG, "Ignoring invalid routeColor '%s' for route %s",
                   color_str.c_str(), route_id.c_str());
        }
      }

      new_trips.push_back({
        .route_id = route_id,
        .route_name = route_name,
        .route_color = route_color,
        .headsign = headsign,
        .arrival_time = trip["arrivalTime"].as<time_t>(),
        .departure_time = trip["departureTime"].as<time_t>(),
        .is_realtime = trip["isRealtime"].as<bool>(),
        .is_cancelled = !trip["cancellation"].isNull() && !trip["cancellation"].as<std::string>().empty(),
        .route_type = trip["routeType"].is<int>() ? trip["routeType"].as<int>() : -1,
      });
    }

    {
      std::lock_guard<std::mutex> lock(this->schedule_state_.mutex);
      this->schedule_state_.trips = std::move(new_trips);
    }

    return true;
  });

  if (!valid) {
    ESP_LOGW(TAG, "Failed to parse message (%u bytes); preview: %.120s",
             static_cast<unsigned>(payload.size()), payload.c_str());
    this->status_set_error(LOG_STR("Failed to parse schedule data"));
  }
}

// Single left-to-right pass: at each position the longest matching abbreviation wins, so that
// "Foo Bar Baz" is not partially replaced by a shorter "Foo Bar" rule. Replaced text is not rescanned.
std::string TransitTracker::apply_abbreviations_(const std::string &headsign) const {
  if (this->abbreviations_.empty()) {
    return headsign;
  }

  std::string result;
  result.reserve(headsign.size());
  size_t i = 0;
  while (i < headsign.size()) {
    const std::pair<const std::string, std::string> *best = nullptr;
    for (const auto &abbr : this->abbreviations_) {
      const std::string &from = abbr.first;
      if (from.empty() || (best != nullptr && from.size() <= best->first.size())) {
        continue;
      }
      if (headsign.compare(i, from.size(), from) == 0) {
        best = &abbr;
      }
    }

    if (best != nullptr) {
      ESP_LOGV(TAG, "Applying abbreviation '%s' -> '%s'", best->first.c_str(), best->second.c_str());
      result += best->second;
      i += best->first.size();
    } else {
      result += headsign[i++];
    }
  }
  return result;
}

void TransitTracker::set_abbreviations_from_text(const std::string &text) {
  this->abbreviations_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');

    if (parts.size() == 1) {
      // If only one part is provided, treat it as a removal (replace with empty string)
      this->add_abbreviation(parts[0], "");
      continue;
    }

    if (parts.size() != 2) {
      ESP_LOGW(TAG, "Invalid abbreviation line: %s", line.c_str());
      continue;
    }

    this->add_abbreviation(parts[0], parts[1]);
  }
}

void TransitTracker::set_route_styles_from_text(const std::string &text) {
  this->route_styles_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');
    if (parts.size() != 3) {
      ESP_LOGW(TAG, "Invalid route style line: %s", line.c_str());
      continue;
    }
    uint32_t color;
    if (!parse_hex_color(parts[2], color)) {
      ESP_LOGW(TAG, "Invalid route style color '%s' in line: %s", parts[2].c_str(), line.c_str());
      continue;
    }
    this->add_route_style(parts[0], parts[1], Color(color));
  }
}

void TransitTracker::draw_text_centered_(const char *text, Color color) {
  int display_center_x = this->display_->get_width() / 2;
  int display_center_y = this->display_->get_height() / 2;
  this->display_->print(display_center_x, display_center_y, this->font_, color, display::TextAlign::CENTER, text);
}

void TransitTracker::set_realtime_color(const Color &color) {
  this->realtime_color_ = color;
  this->realtime_color_dark_ = Color(
    (color.r * 0.5),
    (color.g * 0.5),
    (color.b * 0.5)
  );
}

const uint8_t realtime_icon[6][6] = {
  {0, 0, 0, 3, 3, 3},
  {0, 0, 3, 0, 0, 0},
  {0, 3, 0, 0, 2, 2},
  {3, 0, 0, 2, 0, 0},
  {3, 0, 2, 0, 0, 1},
  {3, 0, 2, 0, 1, 1}
};

void HOT TransitTracker::draw_realtime_icon_(int bottom_right_x, int bottom_right_y, unsigned long uptime) {
  const int num_frames = 6;
  const int idle_frame_duration = 3000;
  const int anim_frame_duration = 200;
  const int cycle_duration = idle_frame_duration + (num_frames - 1) * anim_frame_duration;

  unsigned long cycle_time = uptime % cycle_duration;

  int frame;
  if (cycle_time < idle_frame_duration) {
    frame = 0;
  } else {
    frame = 1 + (cycle_time - idle_frame_duration) / anim_frame_duration;
  }

  auto is_segment_lit = [frame](uint8_t segment) {
    switch (segment) {
      case 1: return frame >= 1 && frame <= 3;
      case 2: return frame >= 2 && frame <= 4;
      case 3: return frame >= 3 && frame <= 5;
      default: return false;
    }
  };

  for (uint8_t i = 0; i < 6; ++i) {
    for (uint8_t j = 0; j < 6; ++j) {
      uint8_t segment_number = realtime_icon[i][j];
      if (segment_number == 0) {
        continue;
      }

      Color icon_color = is_segment_lit(segment_number) ? this->realtime_color_ : this->realtime_color_dark_;
      this->display_->draw_pixel_at(bottom_right_x - (5 - j), bottom_right_y - (5 - i), icon_color);
    }
  }
}

bool TransitTracker::is_rail_trip_(const Trip &trip) const {
  for (const auto &route_id : this->cancelled_rail_routes_) {
    if (route_id == trip.route_id) return true;
  }

  int t = trip.route_type;
  if (t < 0) return this->cancelled_unknown_is_rail_;

  // Bus-like route types (basic + extended GTFS types): bus, trolleybus, coach, bus services, taxi
  bool is_bus = t == 3 || t == 11 || (t >= 200 && t < 300) || (t >= 700 && t < 900) || (t >= 1500 && t < 1600);
  return !is_bus;
}

// 7x7 cross drawn with 2px-thick diagonals
void TransitTracker::draw_cross_icon_(int right_x, int top, Color color) {
  const int size = CROSS_ICON_SIZE;
  int left = right_x - size + 1;
  for (int i = 0; i < size; i++) {
    this->display_->draw_pixel_at(left + i, top + i, color);
    this->display_->draw_pixel_at(left + size - 1 - i, top + i, color);
    if (i + 1 < size) {
      this->display_->draw_pixel_at(left + i + 1, top + i, color);
      this->display_->draw_pixel_at(left + size - 2 - i, top + i, color);
    }
  }
}

void TransitTracker::draw_trip(
    const Trip &trip, int y_offset, int font_height, unsigned long uptime, uint rtc_now,
    bool no_draw, int *headsign_overflow_out, int scroll_cycle_duration, int row_index
) {
  if (!no_draw) {
    this->display_->print(0, y_offset, this->font_, trip.route_color, display::TextAlign::TOP_LEFT, trip.route_name.c_str());
  }

  int route_width, _;
  this->font_->measure(trip.route_name.c_str(), &route_width, &_, &_, &_);

  int headsign_clipping_start = route_width + 3;
  int headsign_clipping_end;

  if (trip.is_cancelled) {
    // Cancelled trip / skipped stop: a cross alternating with a label replaces the countdown
    const std::string &label = this->is_rail_trip_(trip) ? this->cancelled_rail_text_ : this->cancelled_bus_text_;
    int label_width = 0;
    if (!label.empty()) this->font_->measure(label.c_str(), &label_width, &_, &_, &_);

    // Reserve the widest of the two so the headsign doesn't jump when they alternate
    int slot_width = std::max(label_width, CROSS_ICON_SIZE);
    headsign_clipping_end = this->display_->get_width() - slot_width - 2;

    if (!no_draw) {
      // Odd rows are out of phase with even rows so that stacked cancellations
      // form a staggered (quincunx) pattern.
      bool show_label = !label.empty() && ((uptime / this->cancelled_alternate_ms_) + row_index) % 2 == 1;
      if (show_label) {
        this->display_->print(this->display_->get_width() + 1, y_offset, this->font_, this->cancelled_color_,
                              display::TextAlign::TOP_RIGHT, label.c_str());
      } else {
        // Center the cross on the capital letters / digits of the row
        int cap_height = this->font_->get_capheight() > 0 ? this->font_->get_capheight() : this->font_->get_ascender();
        int text_top = y_offset + this->font_->get_ascender() - cap_height;
        int cross_top = text_top + (cap_height - CROSS_ICON_SIZE + 1) / 2;
        this->draw_cross_icon_(this->display_->get_width() - 1, cross_top, this->cancelled_color_);
      }
    }
  } else {
    auto time_display = this->localization_.fmt_duration_from_now(
      this->display_departure_times_ ? trip.departure_time : trip.arrival_time,
      rtc_now
    );

    int time_width;
    this->font_->measure(time_display.c_str(), &time_width, &_, &_, &_);

    headsign_clipping_end = this->display_->get_width() - time_width - 2;

    if (!no_draw) {
      Color time_color = trip.is_realtime ? this->realtime_color_ : Color(0xa7a7a7);
      this->display_->print(this->display_->get_width() + 1, y_offset, this->font_, time_color, display::TextAlign::TOP_RIGHT, time_display.c_str());
    }

    if (trip.is_realtime) {
      headsign_clipping_end -= 8;

      if(!no_draw) {
        int icon_bottom_right_x = this->display_->get_width() - time_width - 2;
        int icon_bottom_right_y = y_offset + font_height - 6;

        this->draw_realtime_icon_(icon_bottom_right_x, icon_bottom_right_y, uptime);
      }
    }
  }

  int headsign_max_width = headsign_clipping_end - headsign_clipping_start;

  int headsign_actual_width;
  this->font_->measure(trip.headsign.c_str(), &headsign_actual_width, &_, &_, &_);

  int headsign_overflow = headsign_actual_width - headsign_max_width;
  if (headsign_overflow_out) {
    *headsign_overflow_out = headsign_overflow;
  }

  if (no_draw) {
    return;
  }

  int scroll_offset = 0;
  if (headsign_overflow > 0 && scroll_cycle_duration > 0) {
    /// Note: The scroll may jump if headsign_clipping_end changes (e.g. due to the width of the arrival time changing).
    /// This is probably not a big deal, since the display makes sudden changes anyway (e.g. when routes are updated)
    /// and this happens relatively infrequently.

    int scroll_time = headsign_overflow * 1000 / scroll_speed;
    int scroll_cycle_time = uptime % scroll_cycle_duration;

    // Scroll idle (left side - default)
    if(scroll_cycle_time < idle_time_left) {
      // scroll_offset = 0; do nothing
    } else if (scroll_cycle_time < idle_time_left + scroll_time) {
      // Scrolling left
      int time_since_scroll_start = scroll_cycle_time - idle_time_left;
      scroll_offset = time_since_scroll_start * scroll_speed / 1000;
    } else if (scroll_cycle_time < idle_time_left + scroll_time + idle_time_right) {
      // Scroll idle (right side)
      scroll_offset = headsign_overflow;
    } else if (scroll_cycle_time < idle_time_left + 2 * scroll_time + idle_time_right){
      // Scrolling right
      int time_since_scroll_start = scroll_cycle_time - (idle_time_left + scroll_time + idle_time_right);
      scroll_offset = headsign_overflow - (time_since_scroll_start * scroll_speed / 1000);
    } else {
      // Waiting for other headsigns to finish scrolling
      // scroll_offset = 0; do nothing
    }
  }

  this->display_->start_clipping(headsign_clipping_start, 0, headsign_clipping_end, this->display_->get_height());
  if (trip.is_cancelled) {
    this->display_->print(headsign_clipping_start - scroll_offset, y_offset, this->font_, this->cancelled_headsign_color_,
                          display::TextAlign::TOP_LEFT, trip.headsign.c_str());
  } else {
    this->display_->print(headsign_clipping_start - scroll_offset, y_offset, this->font_, trip.headsign.c_str());
  }
  this->display_->end_clipping();
}

void HOT TransitTracker::draw_schedule() {
  if (this->display_ == nullptr) [[unlikely]] {
    ESP_LOGW(TAG, "No display attached, cannot draw schedule");
    return;
  }

  if (!esphome::network::is_connected()) {
    this->draw_text_centered_("Attente du réseau", Color(0x252627));
    return;
  }

  if (!this->rtc_->now().is_valid()) {
    this->draw_text_centered_("Attente de l'heure", Color(0x252627));
    return;
  }

  {
    const PreparedAlert *alert = nullptr;
    int index = 0, count = 0;
    uint32_t elapsed = 0;
    this->showing_alert_ =
        this->select_alert_(millis(), this->rtc_->now().timestamp, alert, index, count, elapsed);
    if (this->showing_alert_) {
      this->draw_alert_(*alert, index, count, elapsed);
      return;
    }
  }

  // Alerts take priority: the clock only replaces the schedule
  {
    uint32_t elapsed = 0;
    if (this->select_clock_(millis(), elapsed)) {
      this->draw_clock_(elapsed);
      return;
    }
  }

  {
    const VlilleStation *station = nullptr;
    uint32_t elapsed = 0;
    if (this->select_vlille_(millis(), station, elapsed)) {
      this->draw_vlille_(*station, elapsed);
      return;
    }
  }

  if (this->base_url_.empty()) {
    this->draw_text_centered_("URL non configurée", Color(0x252627));
    return;
  }

  if (this->status_has_error()) {
    this->draw_text_centered_("Erreur de chargement", Color(0xFE4C5C));
    return;
  }

  if (!this->has_ever_connected_.load()) {
    this->draw_text_centered_("Chargement...", Color(0x252627));
    return;
  }

  std::lock_guard<std::mutex> lock(this->schedule_state_.mutex);

  if (this->schedule_state_.trips.empty()) {
    auto message = "Aucun départ prévu";
    this->draw_text_centered_(message, Color(0x252627));
    return;
  }

  int nominal_font_height = this->font_->get_ascender() + this->font_->get_descender();
  unsigned long uptime = millis();
  uint rtc_now = this->rtc_->now().timestamp;

  int scroll_cycle_duration = 0;
  if (this->scroll_headsigns_) {
    int largest_headsign_overflow = 0;
    for (const Trip &trip : this->schedule_state_.trips) {
      int headsign_overflow;
      this->draw_trip(trip, 0, nominal_font_height, uptime, rtc_now, true, &headsign_overflow);
      largest_headsign_overflow = std::max(largest_headsign_overflow, headsign_overflow);
    }

    if (largest_headsign_overflow > 0) {
      int longest_scroll_time = largest_headsign_overflow * 1000 / scroll_speed;
      scroll_cycle_duration = idle_time_left + idle_time_right + 2*longest_scroll_time;
    }
  }

  int max_trips_height = (this->limit_ * this->font_->get_ascender()) + ((this->limit_ - 1) * this->font_->get_descender());
  int y_offset = (this->display_->get_height() % max_trips_height) / 2;

  bool has_header_text = !this->header_text_.empty();
  if (has_header_text) {
    this->display_->print(0, y_offset, this->font_, Color(0x00bdbd), display::TextAlign::LEFT, this->header_text_.c_str());
    y_offset += nominal_font_height;
  }

  int row_index = 0;
  for (const Trip &trip : this->schedule_state_.trips) {
    this->draw_trip(trip, y_offset, nominal_font_height, uptime, rtc_now, false, nullptr, scroll_cycle_duration,
                    row_index++);
    y_offset += nominal_font_height;
  }
}

}  // namespace transit_tracker
}  // namespace esphome
