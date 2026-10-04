#pragma once

#include "esphome/core/defines.h"

#ifdef USE_TRANSIT_TRACKER_CONFIG_EDITOR

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text/text.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace transit_tracker {

class TransitTracker;

/// Exposes the configurator settings (the internal text/select/switch entities it writes)
/// as JSON on GET/POST <path>, so that the web dashboard can edit them live.
/// Every successful POST is applied atomically, saved to flash and reloads the tracker.
class ConfigEditor : public Component, public AsyncWebHandler {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_tracker(TransitTracker *tracker) { this->tracker_ = tracker; }
  void set_path(const std::string &path) { this->path_ = path; }
  void add_text(const std::string &key, text::Text *entity) { this->texts_.emplace_back(key, entity); }
  void add_select(const std::string &key, select::Select *entity) { this->selects_.emplace_back(key, entity); }
  void add_switch(const std::string &key, switch_::Switch *entity) { this->switches_.emplace_back(key, entity); }

  // AsyncWebHandler (called from the HTTP server task)
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  static constexpr size_t MAX_BODY_SIZE = 8192;

  struct PendingValue {
    std::string key;
    std::string value;  // text value, select option, or "1"/"0" for switches
  };

  void rebuild_snapshot_();
  void apply_pending_();
  bool parse_body_(std::vector<PendingValue> &values, std::string &error);

  TransitTracker *tracker_{nullptr};
  std::string path_{"/transit-tracker/config"};
  std::vector<std::pair<std::string, text::Text *>> texts_;
  std::vector<std::pair<std::string, select::Select *>> selects_;
  std::vector<std::pair<std::string, switch_::Switch *>> switches_;

  // Request body, only touched by the HTTP server task
  std::string body_;
  bool body_too_large_{false};

  std::mutex mutex_;
  std::string snapshot_;                // guarded by mutex_
  std::vector<PendingValue> pending_;   // guarded by mutex_
  std::atomic<bool> has_pending_{false};
  std::atomic<bool> dirty_{true};
};

}  // namespace transit_tracker
}  // namespace esphome

#endif  // USE_TRANSIT_TRACKER_CONFIG_EDITOR
