#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace esphome {
namespace transit_tracker {

/// One station requested by the user, and what the endpoint reported for it.
struct VlilleStation {
  std::string query;  // as typed by the user (name, part of a name or numeric ID)
  bool found{false};
  long id{0};
  std::string name;
  int bikes{0};
  int docks{0};
  bool online{true};
};

/// Splits the user's station list ("PLACE RICHEBE, 278308; rivière") into trimmed, non-empty queries.
std::vector<std::string> parse_vlille_queries(const std::string &text);

/// Uppercase, accents removed, punctuation turned into single spaces: "Porte d'Arras" -> "PORTE D ARRAS".
std::string normalize_vlille_name(const std::string &text);

/// Streaming matcher: fed the endpoint body chunk by chunk, it keeps only the requested stations,
/// so the ~50 KB station list never has to fit in RAM at once.
/// Matching per query: numeric ID, else exact name, else first name containing the query.
class VlilleMatcher {
 public:
  explicit VlilleMatcher(const std::vector<std::string> &queries);
  void feed(const char *data, size_t len);
  /// One result per query, in the same order.
  std::vector<VlilleStation> finish();
  /// Number of station objects seen (0 means the body wasn't a station list).
  int stations_seen() const { return this->stations_seen_; }

 protected:
  void handle_object_(const std::string &json);

  struct Query {
    std::string normalized;
    long id{-1};
    VlilleStation exact;
    VlilleStation partial;
  };
  std::vector<Query> queries_;
  std::string object_;
  int depth_{0};
  bool in_string_{false};
  bool escape_{false};
  bool overflow_{false};
  int stations_seen_{0};
};

#ifdef USE_ESP32
/// Periodically fetches the chosen V'Lille stations in a background FreeRTOS task.
/// Names are resolved to IDs once with the light station list (`?simple=1`), then only the chosen
/// stations are polled (`?ids=1,2,3`).
class VlilleFetcher {
 public:
  void set_url(const std::string &url);
  void set_interval_ms(uint32_t ms) { this->interval_ms_ = ms; }
  void set_timeout_ms(uint32_t ms) { this->timeout_ms_ = ms; }
  void set_user_agent(const std::string &ua) { this->user_agent_ = ua; }
  /// Comma/semicolon-separated list of station names or IDs.
  void set_stations(const std::string &text);
  /// The task only polls the endpoint while active (screen enabled and stations chosen).
  void set_active(bool active);

  void start();
  void stop();
  void refresh();

  /// Thread-safe copy of the latest results, one per requested station.
  std::vector<VlilleStation> get_stations();
  /// Incremented each time the results change.
  uint32_t generation() const { return this->generation_.load(); }
  bool has_data() const { return this->has_data_.load(); }
  /// True when the last request to the endpoint succeeded.
  bool is_available() const { return this->available_.load(); }

 protected:
  static void task_entry_(void *arg);
  void task_loop_();
  /// Streams the response of `url` into `matcher`. False on network/HTTP error or if no station was seen.
  bool fetch_(const std::string &url, VlilleMatcher &matcher);
  /// Resolves the user's queries to station IDs with the light station list.
  bool resolve_(const std::string &base_url, const std::vector<std::string> &queries);
  /// Fetches live data for the resolved stations, one result per query.
  bool poll_(const std::string &base_url, std::vector<VlilleStation> &out);
  void publish_(std::vector<VlilleStation> &&stations);

  std::mutex mutex_;
  std::string url_;
  std::vector<std::string> queries_;
  std::vector<VlilleStation> stations_;
  std::string user_agent_;

  // Owned by the background task
  std::vector<std::string> resolved_queries_;
  std::vector<VlilleStation> resolved_;  // one per query; found = the name matched a station

  uint32_t interval_ms_{60000};
  uint32_t timeout_ms_{10000};

  void *task_handle_{nullptr};
  std::atomic<bool> active_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> has_data_{false};
  std::atomic<bool> available_{false};
  std::atomic<uint32_t> generation_{0};
};
#endif

}  // namespace transit_tracker
}  // namespace esphome
