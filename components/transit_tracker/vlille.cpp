#include "vlille.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef USE_ESP32
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <ArduinoJson.h>
#define ESP_LOGD(...)
#define ESP_LOGI(...)
#define ESP_LOGW(...)
#endif

namespace esphome {
namespace transit_tracker {

static const char *const TAG = "transit_tracker.vlille";

// A station object is ~250 bytes; anything much larger isn't one.
static constexpr size_t MAX_OBJECT_SIZE = 2048;

std::vector<std::string> parse_vlille_queries(const std::string &text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find_first_of(",;\n", start);
    if (end == std::string::npos) end = text.size();
    size_t a = start, b = end;
    while (a < b && isspace(static_cast<unsigned char>(text[a]))) a++;
    while (b > a && isspace(static_cast<unsigned char>(text[b - 1]))) b--;
    if (b > a) out.push_back(text.substr(a, b - a));
    start = end + 1;
  }
  return out;
}

// Base letter(s) for the Latin-1 supplement (second byte of 0xC3 xx), uppercase and lowercase alike.
static const char *latin1_base(unsigned char b) {
  if (b == 0xBF) return "Y";   // ÿ
  if (b == 0x9F) return "SS";  // ß
  if (b >= 0xA0) b -= 0x20;    // lowercase -> uppercase
  if (b <= 0x85) return "A";
  if (b == 0x86) return "AE";
  if (b == 0x87) return "C";
  if (b <= 0x8B) return "E";
  if (b <= 0x8F) return "I";
  if (b == 0x90) return "D";
  if (b == 0x91) return "N";
  if (b <= 0x96 || b == 0x98) return "O";
  if (b >= 0x99 && b <= 0x9C) return "U";
  if (b == 0x9D) return "Y";
  return " ";
}

std::string normalize_vlille_name(const std::string &text) {
  std::string folded;
  folded.reserve(text.size());
  for (size_t i = 0; i < text.size(); i++) {
    auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      folded += isalnum(c) ? static_cast<char>(toupper(c)) : ' ';
    } else if (c == 0xC3 && i + 1 < text.size()) {
      folded += latin1_base(static_cast<unsigned char>(text[++i]));
    } else if (c == 0xC5 && i + 1 < text.size() && (text[i + 1] == '\x92' || text[i + 1] == '\x93')) {
      folded += "OE";  // Œ œ
      i++;
    } else {
      // Other non-ASCII characters: skip the continuation bytes
      while (i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) i++;
      folded += ' ';
    }
  }

  // Collapse spaces and trim
  std::string out;
  for (char ch : folded) {
    if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += ch;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

static bool is_all_digits(const std::string &s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

// The endpoint sends numbers as strings ("velos": "23"); accept both.
static long json_number(JsonVariantConst v) {
  if (v.is<const char *>()) return strtol(v.as<const char *>(), nullptr, 10);
  return v.as<long>();
}

VlilleMatcher::VlilleMatcher(const std::vector<std::string> &queries) {
  for (const auto &q : queries) {
    Query query;
    query.normalized = normalize_vlille_name(q);
    if (is_all_digits(q)) query.id = strtol(q.c_str(), nullptr, 10);
    query.exact.query = q;
    query.partial.query = q;
    this->queries_.push_back(std::move(query));
  }
}

void VlilleMatcher::feed(const char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = data[i];
    if (this->depth_ >= 2) {
      if (this->object_.size() < MAX_OBJECT_SIZE) {
        this->object_ += c;
      } else {
        this->overflow_ = true;
      }
    }

    if (this->in_string_) {
      if (this->escape_) {
        this->escape_ = false;
      } else if (c == '\\') {
        this->escape_ = true;
      } else if (c == '"') {
        this->in_string_ = false;
      }
      continue;
    }

    switch (c) {
      case '"':
        this->in_string_ = true;
        break;
      case '{':
      case '[':
        if (this->depth_ == 1 && c == '{') {
          // A station object starts inside the top-level array
          this->object_.assign(1, '{');
          this->overflow_ = false;
        }
        this->depth_++;
        break;
      case '}':
      case ']':
        this->depth_--;
        if (this->depth_ == 1 && c == '}') {
          if (!this->overflow_) this->handle_object_(this->object_);
          this->object_.clear();
        }
        break;
      default:
        break;
    }
  }
}

void VlilleMatcher::handle_object_(const std::string &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return;
  JsonObjectConst obj = doc.as<JsonObjectConst>();
  if (obj.isNull()) return;
  this->stations_seen_++;

  VlilleStation station;
  station.found = true;
  station.id = json_number(obj["id"]);
  station.name = obj["title"].as<std::string>();
  station.bikes = static_cast<int>(json_number(obj["velos"]));
  station.docks = static_cast<int>(json_number(obj["places"]));
  const char *state = obj["etat"].as<const char *>();
  station.online = state == nullptr || strcmp(state, "online") == 0;
  const std::string normalized = normalize_vlille_name(station.name);

  for (auto &q : this->queries_) {
    if (q.exact.found) continue;
    bool exact = q.id >= 0 ? station.id == q.id : normalized == q.normalized;
    if (exact) {
      std::string query = q.exact.query;
      q.exact = station;
      q.exact.query = std::move(query);
    } else if (q.id < 0 && !q.partial.found && !q.normalized.empty() &&
               normalized.find(q.normalized) != std::string::npos) {
      std::string query = q.partial.query;
      q.partial = station;
      q.partial.query = std::move(query);
    }
  }
}

std::vector<VlilleStation> VlilleMatcher::finish() {
  std::vector<VlilleStation> out;
  out.reserve(this->queries_.size());
  for (auto &q : this->queries_) out.push_back(q.exact.found ? q.exact : q.partial);
  return out;
}

// ---------------------------------------------------------------------------
// Background fetcher
// ---------------------------------------------------------------------------
#ifdef USE_ESP32

void VlilleFetcher::set_url(const std::string &url) {
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->url_ == url) return;
    this->url_ = url;
  }
  this->refresh();
}

void VlilleFetcher::set_stations(const std::string &text) {
  auto queries = parse_vlille_queries(text);
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (queries == this->queries_) return;
    this->queries_ = queries;
    // Show the new stations right away (as "loading") instead of the old results
    this->stations_.clear();
    for (const auto &q : queries) {
      VlilleStation station;
      station.query = q;
      this->stations_.push_back(std::move(station));
    }
    this->has_data_ = false;
    this->generation_++;
  }
  this->refresh();
}

void VlilleFetcher::set_active(bool active) {
  bool was_active = this->active_.exchange(active);
  if (active && !was_active) this->refresh();
}

void VlilleFetcher::start() {
  if (this->task_handle_ != nullptr) return;
  this->stop_requested_ = false;
  TaskHandle_t handle = nullptr;
  // TLS handshakes need a generous stack
  BaseType_t res = xTaskCreate(&VlilleFetcher::task_entry_, "tt_vlille", 8192, this, 1, &handle);
  if (res != pdPASS) {
    ESP_LOGE(TAG, "Failed to start V'Lille task");
    return;
  }
  this->task_handle_ = handle;
}

void VlilleFetcher::stop() {
  this->stop_requested_ = true;
  this->refresh();
}

void VlilleFetcher::refresh() {
  if (this->task_handle_ != nullptr) xTaskNotifyGive(static_cast<TaskHandle_t>(this->task_handle_));
}

std::vector<VlilleStation> VlilleFetcher::get_stations() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  return this->stations_;
}

void VlilleFetcher::task_entry_(void *arg) {
  static_cast<VlilleFetcher *>(arg)->task_loop_();
  static_cast<VlilleFetcher *>(arg)->task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

static bool stations_equal(const std::vector<VlilleStation> &a, const std::vector<VlilleStation> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    const VlilleStation &x = a[i], &y = b[i];
    if (x.query != y.query || x.found != y.found || x.id != y.id || x.name != y.name || x.bikes != y.bikes ||
        x.docks != y.docks || x.online != y.online)
      return false;
  }
  return true;
}

void VlilleFetcher::publish_(std::vector<VlilleStation> &&stations) {
  std::lock_guard<std::mutex> lock(this->mutex_);
  // Drop results computed for a station list that changed during the request
  if (stations.size() != this->queries_.size()) return;
  for (size_t i = 0; i < stations.size(); i++)
    if (stations[i].query != this->queries_[i]) return;

  bool first = !this->has_data_;
  this->has_data_ = true;
  if (!first && stations_equal(this->stations_, stations)) return;
  this->stations_ = std::move(stations);
  this->generation_++;
  for (const auto &s : this->stations_) {
    if (s.found) {
      ESP_LOGD(TAG, "%s (%ld): %d vélos, %d places%s", s.name.c_str(), s.id, s.bikes, s.docks,
               s.online ? "" : ", hors service");
    }
  }
}

// "https://host/v1/vlille/" + "simple=1" -> "https://host/v1/vlille/?simple=1"
static std::string with_query(const std::string &url, const std::string &query) {
  return url + (url.find('?') == std::string::npos ? "?" : "&") + query;
}

void VlilleFetcher::task_loop_() {
  // Let Wi-Fi and the main websocket settle first
  vTaskDelay(pdMS_TO_TICKS(3000));

  while (!this->stop_requested_) {
    std::string url;
    std::vector<std::string> queries;
    {
      std::lock_guard<std::mutex> lock(this->mutex_);
      url = this->url_;
      queries = this->queries_;
    }

    bool idle = !this->active_ || url.empty() || queries.empty() || !esphome::network::is_connected();
    bool ok = false;
    if (!idle) {
      // Names only need resolving again when the station list changes
      ok = queries == this->resolved_queries_ || this->resolve_(url, queries);
      std::vector<VlilleStation> stations;
      if (ok) ok = this->poll_(url, stations);
      if (ok) this->publish_(std::move(stations));
      if (ok != this->available_) ESP_LOGI(TAG, "V'Lille API %s", ok ? "disponible" : "indisponible");
      this->available_ = ok;
    }

    // Sleep until the next poll, or until refresh() wakes us up
    uint32_t wait_ms = this->interval_ms_;
    if (!idle && !ok) wait_ms = std::min<uint32_t>(wait_ms, 15000);  // retry sooner after a failure
    if (idle) wait_ms = std::min<uint32_t>(wait_ms, 5000);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
  }
}

bool VlilleFetcher::resolve_(const std::string &base_url, const std::vector<std::string> &queries) {
  VlilleMatcher matcher(queries);
  if (!this->fetch_(with_query(base_url, "simple=1"), matcher)) return false;
  this->resolved_ = matcher.finish();
  this->resolved_queries_ = queries;
  for (const auto &r : this->resolved_) {
    if (r.found) {
      ESP_LOGI(TAG, "\"%s\" -> %s (%ld)", r.query.c_str(), r.name.c_str(), r.id);
    } else {
      ESP_LOGW(TAG, "Station introuvable : \"%s\"", r.query.c_str());
    }
  }
  return true;
}

bool VlilleFetcher::poll_(const std::string &base_url, std::vector<VlilleStation> &out) {
  // Unique IDs of the resolved stations, matched back to the queries below
  std::vector<std::string> ids;
  for (const auto &r : this->resolved_) {
    if (!r.found) continue;
    std::string id = std::to_string(r.id);
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  }

  std::vector<VlilleStation> live;
  if (!ids.empty()) {
    std::string joined;
    for (const auto &id : ids) joined += (joined.empty() ? "" : ",") + id;
    VlilleMatcher matcher(ids);
    if (!this->fetch_(with_query(base_url, "ids=" + joined), matcher)) return false;
    live = matcher.finish();
  }

  out.clear();
  for (const auto &r : this->resolved_) {
    VlilleStation station;
    station.query = r.query;
    if (r.found) {
      auto it = std::find_if(live.begin(), live.end(), [&](const VlilleStation &s) { return s.found && s.id == r.id; });
      if (it != live.end()) {
        station = *it;
        station.query = r.query;
      }
    }
    out.push_back(std::move(station));
  }
  return true;
}

bool VlilleFetcher::fetch_(const std::string &url, VlilleMatcher &matcher) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = static_cast<int>(this->timeout_ms_);
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.buffer_size = 1024;
  config.buffer_size_tx = 1024;
  config.keep_alive_enable = false;
  if (!this->user_agent_.empty()) config.user_agent = this->user_agent_.c_str();

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Failed to init HTTP client");
    return false;
  }
  esp_http_client_set_header(client, "Accept", "application/json");

  bool ok = false;
  esp_err_t err = ESP_OK;
  int status = 0;

  // Open the connection, following up to 3 redirects
  for (int attempt = 0; attempt < 4; attempt++) {
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) break;
    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    bool is_redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (!is_redirect || attempt == 3) break;
    esp_http_client_flush_response(client, nullptr);
    esp_http_client_close(client);
    if (esp_http_client_set_redirection(client) != ESP_OK) {
      err = ESP_FAIL;
      break;
    }
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "V'Lille request failed: %s", esp_err_to_name(err));
  } else if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "V'Lille endpoint returned HTTP %d", status);
  } else {
    // Parse while downloading: only the requested stations are kept
    char buf[512];
    size_t total = 0;
    ok = true;
    while (true) {
      int n = esp_http_client_read(client, buf, sizeof(buf));
      if (n < 0) {
        ESP_LOGW(TAG, "Error reading V'Lille response");
        ok = false;
        break;
      }
      if (n == 0) break;
      matcher.feed(buf, n);
      total += n;
    }
    if (ok && matcher.stations_seen() == 0) {
      ESP_LOGW(TAG, "V'Lille response contains no stations (%u bytes)", static_cast<unsigned>(total));
      ok = false;
    }
    if (ok) ESP_LOGD(TAG, "Fetched %d stations (%u bytes)", matcher.stations_seen(), static_cast<unsigned>(total));
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok;
}

#endif  // USE_ESP32

}  // namespace transit_tracker
}  // namespace esphome
