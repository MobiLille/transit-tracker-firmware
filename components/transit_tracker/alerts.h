#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/color.h"

namespace esphome {
namespace transit_tracker {

enum AlertSeverity : uint8_t {
  ALERT_SEVERITY_INFO = 0,
  ALERT_SEVERITY_WARNING = 1,
  ALERT_SEVERITY_SEVERE = 2,
};

/// A single alert as returned by the custom alerts endpoint.
struct Alert {
  std::string id;
  std::string title;
  std::string message;
  std::string route;
  AlertSeverity severity{ALERT_SEVERITY_WARNING};
  bool has_color{false};
  Color color{};
  time_t start{0};  // 0 = no lower bound
  time_t end{0};    // 0 = no upper bound

  bool is_active_at(time_t now) const {
    if (now <= 0) return true;  // no valid time yet: don't filter
    if (this->start > 0 && now < this->start) return false;
    if (this->end > 0 && now >= this->end) return false;
    return true;
  }
};

/// JSON keys used to read the endpoint response. Dotted paths ("data.alerts") are supported.
struct AlertFieldKeys {
  std::string list = "alerts";
  std::string id = "id";
  std::string title = "title";
  std::string message = "message";
  std::string severity = "severity";
  std::string route = "route";
  std::string color = "color";
  std::string active = "active";
  std::string start = "start";
  std::string end = "end";
  /// Boolean field that forces the severe level when true (e.g. "important": true)
  std::string important = "important";
};

/// Parses an endpoint response body into alerts. Pure function, usable in host tests.
/// Accepted shapes: a JSON array of alerts, an object containing the array at `keys.list`,
/// or a single alert object. Returns false if the body is not valid JSON.
bool parse_alerts_json(const std::string &body, const AlertFieldKeys &keys, const std::string &language,
                       std::vector<Alert> &out);

/// Replaces typographic characters that the bitmap font does not contain and strips emoji.
std::string sanitize_alert_text(const std::string &text);

AlertSeverity parse_alert_severity(const std::string &value);

/// Removes `title` from the beginning of `message` (plus the punctuation that follows it),
/// for endpoints whose message repeats the title. Returns the message unchanged otherwise.
std::string strip_title_prefix(const std::string &message, const std::string &title);

/// Full alert text for the body when the header shows the severity: the title followed by the
/// message, without repeating the title when the message already starts with it.
std::string compose_alert_body(const std::string &title, const std::string &message);

#ifdef USE_ESP32
/// Periodically fetches alerts from an HTTP(S) endpoint in a background FreeRTOS task,
/// so that network latency never blocks the display loop.
class AlertFetcher {
 public:
  void set_url(const std::string &url);
  std::string get_url();
  void set_interval_ms(uint32_t ms) { this->interval_ms_ = ms; }
  void set_timeout_ms(uint32_t ms) { this->timeout_ms_ = ms; }
  void set_max_response_size(size_t size) { this->max_response_size_ = size; }
  void set_language(const std::string &language) { this->language_ = language; }
  void set_user_agent(const std::string &ua) { this->user_agent_ = ua; }
  void add_header(const std::string &name, const std::string &value) { this->headers_.emplace_back(name, value); }
  AlertFieldKeys &keys() { return this->keys_; }

  void start();
  void stop();
  /// Wakes the background task to fetch immediately.
  void refresh();

  /// Thread-safe copy of the latest alerts.
  std::vector<Alert> get_alerts();
  /// Incremented each time the alert list content changes.
  uint32_t generation() const { return this->generation_.load(); }
  size_t count() const { return this->count_.load(); }
  bool last_fetch_ok() const { return this->last_fetch_ok_.load(); }

 protected:
  static void task_entry_(void *arg);
  void task_loop_();
  bool fetch_(const std::string &url, std::string &body);
  void publish_(std::vector<Alert> &&alerts);

  std::mutex mutex_;
  std::string url_;
  std::vector<Alert> alerts_;
  std::vector<std::pair<std::string, std::string>> headers_;
  AlertFieldKeys keys_;
  std::string language_;
  std::string user_agent_;

  uint32_t interval_ms_{60000};
  uint32_t timeout_ms_{10000};
  size_t max_response_size_{8192};

  void *task_handle_{nullptr};
  std::atomic<bool> stop_requested_{false};
  std::atomic<uint32_t> generation_{0};
  std::atomic<size_t> count_{0};
  std::atomic<bool> last_fetch_ok_{false};
  int consecutive_failures_{0};
};
#endif

}  // namespace transit_tracker
}  // namespace esphome
