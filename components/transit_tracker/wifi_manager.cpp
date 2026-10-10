#include "wifi_manager.h"

#ifdef USE_TRANSIT_TRACKER_WIFI_MANAGER

#include <algorithm>
#include <vector>

#include "esp_wifi.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/components/wifi/scan_list.h"

namespace esphome {
namespace transit_tracker {

static const char *const TAG = "transit_tracker.wifi_manager";

void WifiManager::setup() {
  wifi::global_wifi_component->add_scan_results_listener(this);
  if (this->status_sensor_ != nullptr) {
    this->status_sensor_->add_on_state_callback([this](auto &&...) { this->dirty_ = true; });
  }
  web_server_base::global_web_server_base->add_handler(this);
}

void WifiManager::dump_config() {
  ESP_LOGCONFIG(TAG, "Transit Tracker Wi-Fi manager:");
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
}

void WifiManager::loop() {
  const uint32_t now = millis();

  if (this->scan_requested_.exchange(false)) {
    this->start_scan_();
  }
  if (this->scanning_ && now - this->scan_started_ > SCAN_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Wi-Fi scan timed out");
    this->scanning_ = false;
    this->dirty_ = true;
  }

  if (this->connect_requested_.exchange(false)) {
    std::string ssid, password;
    {
      std::lock_guard<std::mutex> lock(this->mutex_);
      ssid.swap(this->pending_ssid_);
      password.swap(this->pending_password_);
    }
    ESP_LOGI(TAG, "Connecting to '%s' from the web dashboard", ssid.c_str());
    // Same path as the dashboard entities: the button runs wifi.configure with its fallback
    this->ssid_text_->make_call().set_value(ssid).perform();
    this->password_text_->make_call().set_value(password).perform();
    this->connect_button_->press();
    this->dirty_ = true;
  }

  // The current network and signal change on their own: refresh regularly
  if (this->dirty_.exchange(false) || now - this->last_snapshot_ > SNAPSHOT_INTERVAL_MS) {
    this->last_snapshot_ = now;
    this->rebuild_snapshot_();
  }
}

void WifiManager::start_scan_() {
  if (this->scanning_) return;

  // Same settings as ESPHome's own scans; the driver keeps the current connection while scanning
  wifi_scan_config_t config{};
  config.show_hidden = false;
  config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  config.scan_time.active.min = 100;
  config.scan_time.active.max = 300;
  esp_err_t err = esp_wifi_scan_start(&config, false);
  if (err != ESP_OK) {
    // Usually another scan (roaming, reconnection) is already running: its results will arrive too
    ESP_LOGW(TAG, "Could not start a Wi-Fi scan: %s", esp_err_to_name(err));
  }
  this->scanning_ = true;
  this->scan_started_ = millis();
  this->dirty_ = true;
}

void WifiManager::on_wifi_scan_results(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results) {
  struct Network {
    std::string ssid;
    int8_t rssi;
    bool lock;
  };
  std::vector<Network> networks;
  for (const auto &scan : results) {
    bool with_auth = false;
    if (!wifi::should_show_scan_entry(results, scan, with_auth)) continue;
    networks.push_back({scan.get_ssid().str(), scan.get_rssi(), with_auth});
  }
  std::sort(networks.begin(), networks.end(), [](const Network &a, const Network &b) { return a.rssi > b.rssi; });

  this->networks_json_ = json::build_json([&networks](JsonObject root) {
    auto list = root["n"].to<JsonArray>();
    for (const auto &n : networks) {
      auto item = list.add<JsonObject>();
      item["ssid"] = n.ssid;
      item["rssi"] = n.rssi;
      item["lock"] = n.lock;
    }
  });
  this->scanning_ = false;
  this->dirty_ = true;
}

void WifiManager::rebuild_snapshot_() {
  char ssid[wifi::SSID_BUFFER_SIZE] = {};
  char ip[network::IP_ADDRESS_BUFFER_SIZE] = {};
  const bool connected = wifi::global_wifi_component->is_connected();
  if (connected) {
    wifi::global_wifi_component->wifi_ssid_to(ssid);
    auto addresses = network::get_ip_addresses();
    if (!addresses.empty()) addresses[0].str_to(ip);
  }

  std::string json = json::build_json([&](JsonObject root) {
    root["connected"] = connected;
    root["ssid"] = ssid;
    root["ip"] = ip;
    root["rssi"] = connected ? wifi::global_wifi_component->wifi_rssi() : 0;
    root["scanning"] = this->scanning_;
    root["status"] = this->status_sensor_ != nullptr ? this->status_sensor_->get_state() : std::string();
  });
  // Splice in the last scan, kept as ready-made JSON: {"n":[...]} -> ,"networks":[...]
  json.pop_back();
  json += ",\"networks\":";
  json.append(this->networks_json_, 5, this->networks_json_.size() - 6);
  json += "}";

  std::lock_guard<std::mutex> lock(this->mutex_);
  this->snapshot_ = std::move(json);
}

bool WifiManager::canHandle(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const char *url = request->url_to(buf).c_str();
  if (request->method() == HTTP_GET) return this->path_ == url;
  if (request->method() == HTTP_POST) return this->path_ + "/scan" == url || this->path_ + "/connect" == url;
  return false;
}

void WifiManager::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                             size_t total) {
  if (index == 0) {
    this->body_.clear();
    this->body_too_large_ = total > MAX_BODY_SIZE;
  }
  if (!this->body_too_large_) this->body_.append(reinterpret_cast<const char *>(data), len);
}

void WifiManager::handleRequest(AsyncWebServerRequest *request) {
  if (request->method() == HTTP_GET) {
    std::string snapshot;
    {
      std::lock_guard<std::mutex> lock(this->mutex_);
      snapshot = this->snapshot_;
    }
    auto *response = request->beginResponse(200, "application/json", snapshot);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    return;
  }

  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  if (this->path_ + "/scan" == request->url_to(buf).c_str()) {
    this->scan_requested_ = true;
    request->send(202, "application/json", "{\"ok\":true}");
    return;
  }

  // POST <path>/connect
  std::string body;
  body.swap(this->body_);
  if (this->body_too_large_) {
    request->send(413, "text/plain; charset=utf-8", "Requête trop volumineuse");
    return;
  }
  std::string ssid, password;
  bool valid = json::parse_json(body, [&](JsonObject root) -> bool {
    ssid = root["ssid"] | "";
    password = root["password"] | "";
    return true;
  });
  const char *error = nullptr;
  if (!valid) {
    error = "JSON invalide";
  } else if (ssid.empty() || ssid.size() > 32) {
    error = "Nom de réseau invalide (1 à 32 caractères)";
  } else if (!password.empty() && (password.size() < 8 || password.size() > 64)) {
    error = "Mot de passe invalide (8 à 64 caractères, ou vide pour un réseau ouvert)";
  }
  if (error != nullptr) {
    request->send(400, "text/plain; charset=utf-8", error);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->pending_ssid_ = std::move(ssid);
    this->pending_password_ = std::move(password);
  }
  this->connect_requested_ = true;
  request->send(202, "application/json", "{\"ok\":true}");
}

}  // namespace transit_tracker
}  // namespace esphome

#endif  // USE_TRANSIT_TRACKER_WIFI_MANAGER
