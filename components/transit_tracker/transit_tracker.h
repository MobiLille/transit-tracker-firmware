#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esphome/components/font/font.h"
#include "esphome/components/time/real_time_clock.h"

#include "schedule_state.h"
#include "localization.h"
#include "websocket_client.h"
#include "alerts.h"
#include "vlille.h"

namespace esphome {
namespace transit_tracker {

struct RouteStyle {
  std::string name;
  Color color;
};

enum ClockAnimation : uint8_t {
  CLOCK_ANIMATION_NONE = 0,
  CLOCK_ANIMATION_SLIDE = 1,
  CLOCK_ANIMATION_FADE = 2,
};

enum AlertHeaderStyle : uint8_t {
  ALERT_HEADER_FILLED = 0,
  ALERT_HEADER_TEXT = 1,
};

/// it, alert, index (0-based), count, elapsed ms since this alert started showing
using AlertDrawFunction = std::function<void(display::Display &, const Alert &, int, int, uint32_t)>;

class TransitTracker : public Component {
  public:
    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

    bool is_connected() const { return this->ws_client_.is_connected(); }
    void reconnect(const char *reason);
    void close(bool fully = false);

    void draw_schedule();

    /// Fired once at boot, when the loading screen gives way to the schedule
    Trigger<> *get_loaded_trigger() { return &this->loaded_trigger_; }

    Localization* get_localization() { return &this->localization_; }

    void set_display(display::Display *display) { display_ = display; }
    void set_font(font::Font *font) { font_ = font; }
    void set_rtc(time::RealTimeClock *rtc) { rtc_ = rtc; }

    void set_base_url(const std::string &base_url) { base_url_ = base_url; }
    void set_feed_code(const std::string &feed_code) { feed_code_ = feed_code; }
    void set_display_departure_times(bool display_departure_times) { display_departure_times_ = display_departure_times; }
    void set_schedule_string(const std::string &schedule_string) { schedule_string_ = schedule_string; }
    void set_list_mode(const std::string &list_mode) { list_mode_ = list_mode; }
    void set_limit(int limit) { limit_ = limit; }
    /// Number of departure pages (limit rows each); resubscribes when changed after connecting
    void set_pages(int pages);
    /// How long each departure page stays on screen
    void set_page_duration(uint32_t ms) { page_duration_ms_ = std::max<uint32_t>(ms, 1000); }
    void set_scroll_headsigns(bool scroll_headsigns) { scroll_headsigns_ = scroll_headsigns; }

    void set_header_text(const std::string &header_text) { header_text_ = header_text; }
    void set_unit_display(UnitDisplay unit_display) { this->localization_.set_unit_display(unit_display); }
    void add_abbreviation(const std::string &from, const std::string &to) { abbreviations_[from] = to; }
    void add_header(const std::string &name, const std::string &value) { extra_headers_.emplace_back(name, value); }
    void set_default_route_color(const Color &color) { default_route_color_ = color; }
    void add_route_style(const std::string &route_id, const std::string &name, const Color &color) { route_styles_[route_id] = RouteStyle{name, color}; }

    void set_abbreviations_from_text(const std::string &text);
    void set_route_styles_from_text(const std::string &text);

    void set_realtime_color(const Color &color);

    // --- Cancelled trips ---
    /// Asks the API to keep cancelled trips (flagged) instead of dropping them
    void set_show_cancelled(bool show) { this->show_cancelled_ = show; }
    /// Label for cancelled metro/tram/train trips, e.g. "Interrompu"
    void set_cancelled_rail_text(const std::string &text) { this->cancelled_rail_text_ = text; }
    /// Label for cancelled bus trips, e.g. "Non desservi"
    void set_cancelled_bus_text(const std::string &text) { this->cancelled_bus_text_ = text; }
    void set_cancelled_color(const Color &color) { this->cancelled_color_ = color; }
    void set_cancelled_headsign_color(const Color &color) { this->cancelled_headsign_color_ = color; }
    /// How long the cross and the label are each shown
    void set_cancelled_alternate_interval(uint32_t ms) { this->cancelled_alternate_ms_ = std::max<uint32_t>(ms, 250); }
    /// Label used when the API doesn't report a route type
    void set_cancelled_unknown_is_rail(bool rail) { this->cancelled_unknown_is_rail_ = rail; }
    /// Forces the rail label for these route IDs, whatever their route type
    void add_cancelled_rail_route(const std::string &route_id) { this->cancelled_rail_routes_.push_back(route_id); }

    // --- Alert screen ---
    void set_alerts_url(const std::string &url);
    void set_alerts_configured() { this->alerts_configured_ = true; }
    std::string get_alerts_url() { return this->alert_fetcher_.get_url(); }
    void set_alerts_update_interval(uint32_t ms) { this->alert_fetcher_.set_interval_ms(ms); }
    void set_alerts_timeout(uint32_t ms) { this->alert_fetcher_.set_timeout_ms(ms); }
    void set_alerts_max_response_size(size_t size) { this->alert_fetcher_.set_max_response_size(size); }
    void set_alerts_language(const std::string &language) { this->alert_fetcher_.set_language(language); }
    void add_alerts_header(const std::string &name, const std::string &value) { this->alert_fetcher_.add_header(name, value); }
    AlertFieldKeys &alerts_keys() { return this->alert_fetcher_.keys(); }

    void set_alerts_schedule_duration(uint32_t ms) { this->alerts_schedule_duration_ms_ = ms; }
    void set_alerts_alert_duration(uint32_t ms) { this->alerts_alert_duration_ms_ = ms; }
    void set_alerts_page_duration(uint32_t ms) { this->alerts_page_duration_ms_ = std::max<uint32_t>(ms, 500); }
    void set_alerts_header_style(AlertHeaderStyle style) { this->alerts_header_style_ = style; }
    /// Header text per severity (used when the header shows the severity)
    void set_alerts_info_title(const std::string &title) { this->alerts_info_title_ = title; }
    void set_alerts_warning_title(const std::string &title) { this->alerts_warning_title_ = title; }
    void set_alerts_severe_title(const std::string &title) { this->alerts_severe_title_ = title; }
    /// true: header shows the severity label and the body the title + message.
    /// false: header shows the alert title and the body the message.
    void set_alerts_header_shows_severity(bool severity) { this->alerts_header_shows_severity_ = severity; }
    void set_alerts_show_counter(bool show) { this->alerts_show_counter_ = show; }
    void set_alerts_info_color(const Color &color) { this->alerts_info_color_ = color; }
    void set_alerts_warning_color(const Color &color) { this->alerts_warning_color_ = color; }
    void set_alerts_severe_color(const Color &color) { this->alerts_severe_color_ = color; }
    void set_alerts_text_color(const Color &color) { this->alerts_text_color_ = color; }
    void set_alerts_draw_lambda(AlertDrawFunction &&f) { this->alerts_draw_lambda_ = std::move(f); }
    /// Font for the alert screen (defaults to the schedule font)
    void set_alerts_font(font::Font *font) { this->alerts_font_ = font; }

    /// Enables/disables the rotation at runtime (alerts keep being fetched).
    void set_alerts_enabled(bool enabled) { this->alerts_enabled_ = enabled; }
    bool get_alerts_enabled() const { return this->alerts_enabled_; }
    /// Number of alerts returned by the endpoint (including ones outside their start/end window).
    int get_alert_count() const { return static_cast<int>(this->alert_fetcher_.count()); }
    bool is_showing_alert() const { return this->showing_alert_; }
    void refresh_alerts() { this->alert_fetcher_.refresh(); }
    /// Jumps straight to the first alert on the next frame.
    void show_alerts_now() { this->alerts_rotation_reset_pending_ = true; }

    // --- Clock screen ---
    /// Enables/disables the date/time screen at runtime.
    void set_clock_enabled(bool enabled);
    bool get_clock_enabled() const { return this->clock_enabled_; }
    /// Time between two appearances of the clock
    void set_clock_interval(uint32_t ms) { this->clock_interval_ms_ = ms; }
    /// How long the clock stays on screen
    void set_clock_duration(uint32_t ms) { this->clock_duration_ms_ = std::max<uint32_t>(ms, 1000); }
    void set_clock_date_color(const Color &color) { this->clock_date_color_ = color; }
    void set_clock_time_color(const Color &color) { this->clock_time_color_ = color; }
    /// Enter/exit transition of the date and time
    void set_clock_animation(ClockAnimation animation) { this->clock_animation_ = animation; }

    // --- V'Lille screen ---
    void set_vlille_url(const std::string &url) { this->vlille_fetcher_.set_url(url); }
    void set_vlille_update_interval(uint32_t ms) { this->vlille_fetcher_.set_interval_ms(ms); }
    void set_vlille_timeout(uint32_t ms) { this->vlille_fetcher_.set_timeout_ms(ms); }
    void set_vlille_configured() { this->vlille_configured_ = true; }
    /// Station names (or parts of names) or IDs, separated by commas or semicolons. One screen per station.
    void set_vlille_stations(const std::string &text) { this->vlille_fetcher_.set_stations(text); }
    void set_vlille_enabled(bool enabled);
    bool get_vlille_enabled() const { return this->vlille_enabled_; }
    /// Time between two passes over the stations
    void set_vlille_interval(uint32_t ms) { this->vlille_interval_ms_ = ms; }
    /// How long each station stays on screen
    void set_vlille_duration(uint32_t ms) { this->vlille_duration_ms_ = std::max<uint32_t>(ms, 1000); }
    void set_vlille_logo_color(const Color &color) { this->vlille_logo_color_ = color; }
    /// Human-readable summary of the matched stations, for a text sensor.
    std::string get_vlille_status();

  protected:
    static constexpr int scroll_speed = 10; // pixels/second
    static constexpr int idle_time_left = 5000;
    static constexpr int idle_time_right = 1000;

    std::string from_now_(time_t unix_timestamp, uint rtc_now) const;
    void draw_text_centered_(const char *text, Color color);
    void draw_realtime_icon_(int bottom_right_x, int bottom_right_y, unsigned long now);

    void draw_trip(
      const Trip &trip, int y_offset, int font_height, unsigned long uptime, uint rtc_now,
      bool no_draw = false, int *headsign_overflow_out = nullptr, int scroll_cycle_duration = 0,
      int row_index = 0
    );
    bool is_rail_trip_(const Trip &trip) const;
    void draw_cross_icon_(int right_x, int top, Color color);

    Localization localization_{};
    ScheduleState schedule_state_;

    display::Display *display_;
    font::Font *font_;
    time::RealTimeClock *rtc_;

    WebSocketClient ws_client_;

    void handle_message_(const std::string &payload);
    std::string apply_abbreviations_(const std::string &headsign) const;
    void send_subscribe_();
    void on_disconnect_();

    std::atomic<int> consecutive_disconnects_{0};
    std::atomic<unsigned long> last_heartbeat_{0};
    std::atomic<bool> has_ever_connected_{false};
    std::atomic<bool> pending_subscribe_{false};
    std::atomic<bool> fully_closed_{false};
    /// Set once the first schedule has been received; until then the loading screen is shown
    std::atomic<bool> schedule_loaded_{false};

    void draw_loading_();
    unsigned long loading_start_ = 0;
    float loading_bar_shown_ = 0;
    bool loading_done_ = false;
    Trigger<> loaded_trigger_;

    std::string base_url_;
    std::vector<std::pair<std::string, std::string>> extra_headers_;
    std::string feed_code_;
    std::string schedule_string_;
    std::string list_mode_;
    bool display_departure_times_ = true;
    int limit_;
    int pages_ = 1;
    uint32_t page_duration_ms_ = 8000;
    /// Start of the current stay on the schedule screen (pages restart from the first one)
    unsigned long schedule_page_start_ = 0;
    unsigned long schedule_last_draw_ = 0;

    std::string header_text_;
    std::map<std::string, std::string> abbreviations_;
    Color default_route_color_ = Color(0x028e51);
    std::map<std::string, RouteStyle> route_styles_;
    bool scroll_headsigns_ = false;

    struct PreparedAlert {
      Alert alert;
      std::string header;
      std::vector<std::string> lines;
      uint32_t duration_ms;
    };

    bool select_alert_(unsigned long now_ms, time_t rtc_now, const PreparedAlert *&alert, int &index, int &count,
                       uint32_t &elapsed);
    void prepare_alerts_(unsigned long now_ms);
    void draw_alert_(const PreparedAlert &prepared, int index, int count, uint32_t elapsed);
    void draw_alert_icon_(int x, int y, AlertSeverity severity, Color color);
    std::string strip_unsupported_chars_(const std::string &text);
    std::vector<std::string> wrap_text_(const std::string &text, int max_width);
    int text_width_(const std::string &text);
    Color alert_color_(const Alert &alert) const;
    int alert_body_lines_() const;

    AlertFetcher alert_fetcher_;
    bool alerts_configured_ = false;
    bool setup_done_ = false;
    bool alerts_enabled_ = true;
    bool showing_alert_ = false;
    bool alerts_rotation_reset_pending_ = false;
    uint32_t alerts_generation_seen_ = 0;
    unsigned long alerts_rotation_start_ = 0;
    std::vector<PreparedAlert> prepared_alerts_;

    uint32_t alerts_schedule_duration_ms_ = 20000;
    uint32_t alerts_alert_duration_ms_ = 8000;
    uint32_t alerts_page_duration_ms_ = 4000;
    AlertHeaderStyle alerts_header_style_ = ALERT_HEADER_FILLED;
    std::string alerts_info_title_ = "Information";
    std::string alerts_warning_title_ = "Perturbation";
    std::string alerts_severe_title_ = "Perturbation";
    bool alerts_header_shows_severity_ = true;
    const std::string &alert_severity_title_(AlertSeverity severity) const {
      return severity == ALERT_SEVERITY_INFO     ? this->alerts_info_title_
             : severity == ALERT_SEVERITY_SEVERE ? this->alerts_severe_title_
                                                 : this->alerts_warning_title_;
    }
    bool alerts_show_counter_ = true;
    Color alerts_info_color_ = Color(0x0080FF);
    Color alerts_warning_color_ = Color(0xFFA000);
    Color alerts_severe_color_ = Color(0xFF2020);
    Color alerts_text_color_ = Color(0xFFFFFF);
    AlertDrawFunction alerts_draw_lambda_;
    font::Font *alerts_font_{nullptr};
    font::Font *alert_font_() const { return this->alerts_font_ != nullptr ? this->alerts_font_ : this->font_; }
    int alert_header_height_() const;

    bool select_clock_(unsigned long now_ms, uint32_t &elapsed) const;
    void draw_clock_(uint32_t elapsed);

    bool clock_enabled_ = false;
    unsigned long clock_rotation_start_ = 0;
    uint32_t clock_interval_ms_ = 60000;
    uint32_t clock_duration_ms_ = 5000;
    Color clock_date_color_ = Color(0xFFFFFF);
    Color clock_time_color_ = Color(0xFFFFFF);
    ClockAnimation clock_animation_ = CLOCK_ANIMATION_SLIDE;

    bool select_vlille_(unsigned long now_ms, const VlilleStation *&station, uint32_t &elapsed);
    void draw_vlille_(const VlilleStation &station, uint32_t elapsed);

    VlilleFetcher vlille_fetcher_;
    bool vlille_configured_ = false;
    bool vlille_enabled_ = false;
    unsigned long vlille_rotation_start_ = 0;
    uint32_t vlille_interval_ms_ = 60000;
    uint32_t vlille_duration_ms_ = 6000;
    uint32_t vlille_generation_seen_ = 0;
    std::vector<VlilleStation> vlille_stations_;
    Color vlille_logo_color_ = Color(0xE73137);

    bool show_cancelled_ = true;
    std::string cancelled_rail_text_ = "Interrompu";
    std::string cancelled_bus_text_ = "Non desservi";
    Color cancelled_color_ = Color(0xFF2020);
    Color cancelled_headsign_color_ = Color(0x707070);
    uint32_t cancelled_alternate_ms_ = 2000;
    bool cancelled_unknown_is_rail_ = false;
    std::vector<std::string> cancelled_rail_routes_;

    Color realtime_color_ = Color(0x20FF00);
    Color realtime_color_dark_ = Color(0x00A700);
};


}  // namespace transit_tracker
}  // namespace esphome