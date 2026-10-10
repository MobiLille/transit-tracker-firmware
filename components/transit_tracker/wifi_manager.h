#pragma once

#include "esphome/core/defines.h"

#ifdef USE_TRANSIT_TRACKER_WIFI_MANAGER

#include <atomic>
#include <mutex>
#include <string>

#include "esphome/core/component.h"
#include "esphome/components/button/button.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome {
namespace transit_tracker {

/// Wi-Fi panel of the web dashboard: scans the networks around (without dropping the current
/// connection) and connects to one through the dashboard's "new network" entities, whose button
/// runs `wifi.configure` (which falls back to the previous network on failure).
///
///   GET  <path>          current network, last scan and status, as JSON
///   POST <path>/scan     starts a scan
///   POST <path>/connect  {"ssid": "...", "password": "..."}
class WifiManager : public Component, public AsyncWebHandler, public wifi::WiFiScanResultsListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_path(const std::string &path) { this->path_ = path; }
  void set_ssid_text(text::Text *text) { this->ssid_text_ = text; }
  void set_password_text(text::Text *text) { this->password_text_ = text; }
  void set_connect_button(button::Button *button) { this->connect_button_ = button; }
  void set_status_sensor(text_sensor::TextSensor *sensor) { this->status_sensor_ = sensor; }

  void on_wifi_scan_results(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results) override;

  // AsyncWebHandler (called from the HTTP server task)
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  static constexpr size_t MAX_BODY_SIZE = 512;
  static constexpr uint32_t SCAN_TIMEOUT_MS = 15000;
  static constexpr uint32_t SNAPSHOT_INTERVAL_MS = 2000;

  void start_scan_();
  void rebuild_snapshot_();

  std::string path_{"/transit-tracker/wifi"};
  text::Text *ssid_text_{nullptr};
  text::Text *password_text_{nullptr};
  button::Button *connect_button_{nullptr};
  text_sensor::TextSensor *status_sensor_{nullptr};

  // Request body, only touched by the HTTP server task
  std::string body_;
  bool body_too_large_{false};

  // Main loop state
  std::string networks_json_{"{\"n\":[]}"};  // {"n":[...]}, as built after a scan
  bool scanning_{false};
  uint32_t scan_started_{0};
  uint32_t last_snapshot_{0};

  std::mutex mutex_;
  std::string snapshot_{"{}"};     // guarded by mutex_
  std::string pending_ssid_;       // guarded by mutex_
  std::string pending_password_;   // guarded by mutex_
  std::atomic<bool> scan_requested_{false};
  std::atomic<bool> connect_requested_{false};
  std::atomic<bool> dirty_{true};
};

}  // namespace transit_tracker
}  // namespace esphome

#endif  // USE_TRANSIT_TRACKER_WIFI_MANAGER
