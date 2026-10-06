#include "alerts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

static const char *const TAG = "transit_tracker.alerts";

static constexpr uint32_t STARTUP_POLL_MS = 250;
static constexpr uint32_t STARTUP_MAX_WAIT_MS = 90000;

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::string sanitize_alert_text(const std::string &text) {
  // Typographic characters commonly found in French/English alert texts that
  // the Pixolletta bitmap font doesn't include.
  // Longer patterns first: French guillemets usually come with a (narrow) no-break space inside.
  static const std::pair<const char *, const char *> REPLACEMENTS[] = {
      {"\u00AB\u00A0", "\""}, {"\u00AB\u202F", "\""}, {"\u00AB ", "\""},
      {"\u00A0\u00BB", "\""}, {"\u202F\u00BB", "\""}, {" \u00BB", "\""},
      {"’", "'"},  {"‘", "'"},  {"“", "\""}, {"”", "\""}, {"«", "\""},
      {"»", "\""}, {"–", "-"},  {"—", "-"},  {"…", "..."}, {" ", " "},
      {" ", " "},  {" ", " "},  {"•", "-"},  {"·", "-"},  {"→", "->"},
      {"€", "EUR"}, {"°", "o"}, {"\r", ""},       {"\t", " "},
  };

  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    bool replaced = false;
    for (const auto &r : REPLACEMENTS) {
      size_t len = strlen(r.first);
      if (text.compare(i, len, r.first) == 0) {
        out += r.second;
        i += len;
        replaced = true;
        break;
      }
    }
    if (replaced) continue;

    auto c = static_cast<unsigned char>(text[i]);
    size_t seq_len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + seq_len > text.size()) break;  // truncated UTF-8 sequence

    if (seq_len == 4) {
      // Emoji and other astral-plane symbols: not renderable, drop them.
      i += seq_len;
      continue;
    }
    if (seq_len == 3 && c == 0xEF && static_cast<unsigned char>(text[i + 1]) == 0xB8) {
      // Variation selectors (U+FE0x) that often follow symbols
      i += seq_len;
      continue;
    }

    out.append(text, i, seq_len);
    i += seq_len;
  }

  // Collapse runs of spaces and trim
  std::string collapsed;
  collapsed.reserve(out.size());
  for (char ch : out) {
    if (ch == ' ' && (collapsed.empty() || collapsed.back() == ' ' || collapsed.back() == '\n')) continue;
    if (ch == '\n' && !collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
    collapsed += ch;
  }
  while (!collapsed.empty() && (collapsed.back() == ' ' || collapsed.back() == '\n')) collapsed.pop_back();
  while (!collapsed.empty() && collapsed.front() == '\n') collapsed.erase(0, 1);
  return collapsed;
}

static std::string to_lower(std::string s) {
  for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

AlertSeverity parse_alert_severity(const std::string &value) {
  std::string v = to_lower(value);
  if (v == "severe" || v == "critical" || v == "high" || v == "major" || v == "error" || v == "danger" ||
      v == "grave" || v == "critique" || v == "interruption" || v == "coupure" || v == "suspendu" ||
      v == "suspension" || v == "bloquant" || v == "important" || v == "urgent" || v == "perturbation" ||
      v == "2" || v == "3")
    return ALERT_SEVERITY_SEVERE;
  if (v == "info" || v == "information" || v == "informational" || v == "low" || v == "minor" || v == "notice" ||
      v == "0" || v == "unknown_severity")
    return ALERT_SEVERITY_INFO;
  return ALERT_SEVERITY_WARNING;
}

std::string strip_title_prefix(const std::string &message, const std::string &title) {
  if (title.empty() || message.size() <= title.size() || message.compare(0, title.size(), title) != 0)
    return message;
  size_t i = title.size();
  while (i < message.size() && strchr(" !.:;,-\n", message[i]) != nullptr) i++;
  if (i >= message.size()) return message;
  return message.substr(i);
}

std::string compose_alert_body(const std::string &title, const std::string &message) {
  if (message.empty()) return title;
  if (title.empty()) return message;
  if (message.compare(0, title.size(), title) == 0) return message;
  char last = title.back();
  bool has_punctuation = last == '.' || last == '!' || last == '?' || last == ':' || last == ';';
  return title + (has_punctuation ? " " : ". ") + message;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static JsonVariantConst get_path(JsonVariantConst root, const std::string &path) {
  if (path.empty()) return JsonVariantConst();
  JsonVariantConst cur = root;
  size_t start = 0;
  while (start <= path.size()) {
    size_t dot = path.find('.', start);
    std::string part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (!cur.is<JsonObjectConst>()) return JsonVariantConst();
    cur = cur[part];
    if (cur.isNull()) return cur;
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return cur;
}

/// Extracts text from a string, number, {"fr": "...", "en": "..."} object, or GTFS-RT style
/// translated string ({"translation": [{"text": "...", "language": "fr"}]}).
static std::string get_text(JsonVariantConst v, const std::string &language, int depth = 0) {
  if (v.isNull() || depth > 3) return "";
  if (v.is<const char *>()) return v.as<const char *>();
  if (v.is<long long>()) return std::to_string(v.as<long long>());
  if (v.is<double>()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", v.as<double>());
    return buf;
  }

  if (v.is<JsonArrayConst>()) {
    std::string first;
    for (JsonVariantConst item : v.as<JsonArrayConst>()) {
      std::string text = item.is<JsonObjectConst>() ? get_text(item["text"], language, depth + 1)
                                                     : get_text(item, language, depth + 1);
      if (text.empty()) continue;
      if (first.empty()) first = text;
      if (!language.empty() && item.is<JsonObjectConst>() && item["language"].is<const char *>() &&
          to_lower(item["language"].as<const char *>()).rfind(to_lower(language), 0) == 0)
        return text;
    }
    return first;
  }

  if (v.is<JsonObjectConst>()) {
    auto obj = v.as<JsonObjectConst>();
    if (!obj["translation"].isNull()) return get_text(obj["translation"], language, depth + 1);
    if (!obj["text"].isNull()) return get_text(obj["text"], language, depth + 1);
    if (!language.empty() && !obj[language].isNull()) return get_text(obj[language], language, depth + 1);
    for (JsonPairConst kv : obj) {
      std::string text = get_text(kv.value(), language, depth + 1);
      if (!text.empty()) return text;
    }
  }
  return "";
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/// Parses "YYYY-MM-DDTHH:MM[:SS][.fff][Z|±HH:MM]". Times without an offset are treated as UTC.
static time_t parse_iso8601(const char *s) {
  int y, mo, d, h = 0, mi = 0, sec = 0;
  int n = 0;
  if (sscanf(s, "%4d-%2d-%2d%n", &y, &mo, &d, &n) != 3) return 0;
  const char *p = s + n;
  if (*p == 'T' || *p == ' ') {
    p++;
    int consumed = 0;
    if (sscanf(p, "%2d:%2d%n", &h, &mi, &consumed) != 2) return 0;
    p += consumed;
    if (*p == ':') {
      p++;
      if (sscanf(p, "%2d%n", &sec, &consumed) != 1) return 0;
      p += consumed;
    }
    if (*p == '.') {
      p++;
      while (std::isdigit(static_cast<unsigned char>(*p))) p++;
    }
  }
  int64_t offset = 0;
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    int sign = *p == '-' ? -1 : 1;
    p++;
    if (sscanf(p, "%2d:%2d", &oh, &om) < 1 && sscanf(p, "%2d%2d", &oh, &om) < 1) return 0;
    offset = sign * (oh * 3600 + om * 60);
  }
  int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - offset;
  return static_cast<time_t>(t);
}

static time_t get_time(JsonVariantConst v) {
  if (v.isNull()) return 0;
  if (v.is<long long>() || v.is<double>()) {
    double t = v.as<double>();
    if (t > 1e12) t /= 1000.0;  // milliseconds
    return static_cast<time_t>(t);
  }
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    char *end = nullptr;
    double num = strtod(s, &end);
    if (end != s && *end == '\0') return static_cast<time_t>(num > 1e12 ? num / 1000.0 : num);
    return parse_iso8601(s);
  }
  return 0;
}

static bool get_color(JsonVariantConst v, Color &out) {
  if (v.isNull()) return false;
  if (v.is<long long>()) {
    out = Color(static_cast<uint32_t>(v.as<long long>()));
    return true;
  }
  if (!v.is<const char *>()) return false;
  const char *s = v.as<const char *>();
  if (*s == '#') s++;
  if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0) s += 2;
  if (strlen(s) != 6) return false;
  char *end = nullptr;
  unsigned long value = strtoul(s, &end, 16);
  if (*end != '\0') return false;
  out = Color(static_cast<uint32_t>(value));
  return true;
}

static bool get_active(JsonVariantConst v) {
  if (v.isNull()) return true;
  if (v.is<bool>()) return v.as<bool>();
  if (v.is<long long>()) return v.as<long long>() != 0;
  if (v.is<const char *>()) {
    std::string s = to_lower(v.as<const char *>());
    return !(s == "false" || s == "0" || s == "no" || s == "off" || s == "inactive");
  }
  return true;
}

static bool parse_one(JsonObjectConst obj, const AlertFieldKeys &keys, const std::string &language, Alert &alert) {
  if (!get_active(get_path(obj, keys.active))) return false;

  alert.title = sanitize_alert_text(get_text(get_path(obj, keys.title), language));
  alert.message = sanitize_alert_text(get_text(get_path(obj, keys.message), language));
  if (alert.title.empty() && alert.message.empty()) return false;

  alert.id = get_text(get_path(obj, keys.id), language);
  alert.route = sanitize_alert_text(get_text(get_path(obj, keys.route), language));

  JsonVariantConst sev = get_path(obj, keys.severity);
  if (!sev.isNull()) alert.severity = parse_alert_severity(get_text(sev, language));

  JsonVariantConst important = get_path(obj, keys.important);
  if (!important.isNull() && (important.is<bool>() || important.is<long long>()) && important.as<bool>())
    alert.severity = ALERT_SEVERITY_SEVERE;

  alert.has_color = get_color(get_path(obj, keys.color), alert.color);
  alert.start = get_time(get_path(obj, keys.start));
  alert.end = get_time(get_path(obj, keys.end));
  return true;
}

bool parse_alerts_json(const std::string &body, const AlertFieldKeys &keys, const std::string &language,
                       std::vector<Alert> &out) {
  out.clear();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    ESP_LOGW(TAG, "Invalid alerts JSON: %s", err.c_str());
    return false;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  JsonVariantConst list;
  if (root.is<JsonArrayConst>()) {
    list = root;
  } else if (root.is<JsonObjectConst>()) {
    JsonVariantConst nested = get_path(root, keys.list);
    if (nested.is<JsonArrayConst>()) {
      list = nested;
    } else if (nested.is<JsonObjectConst>()) {
      Alert alert;
      if (parse_one(nested.as<JsonObjectConst>(), keys, language, alert)) out.push_back(std::move(alert));
      return true;
    } else if (!get_path(root, keys.title).isNull() || !get_path(root, keys.message).isNull()) {
      // Single alert object at the root
      Alert alert;
      if (parse_one(root.as<JsonObjectConst>(), keys, language, alert)) out.push_back(std::move(alert));
      return true;
    } else {
      // Object without a list or alert fields (e.g. {"alerts": null} or {}): no alerts
      return true;
    }
  } else {
    return root.isNull();  // "null" means no alerts
  }

  for (JsonVariantConst item : list.as<JsonArrayConst>()) {
    if (!item.is<JsonObjectConst>()) continue;
    Alert alert;
    if (parse_one(item.as<JsonObjectConst>(), keys, language, alert)) out.push_back(std::move(alert));
  }
  return true;
}

// ---------------------------------------------------------------------------
// Background fetcher
// ---------------------------------------------------------------------------
#ifdef USE_ESP32

static constexpr int FAILURES_BEFORE_CLEAR = 5;

void AlertFetcher::set_url(const std::string &url) {
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->url_ == url) return;
    this->url_ = url;
  }
  if (url.empty()) this->publish_({});
  this->refresh();
}

std::string AlertFetcher::get_url() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  return this->url_;
}

void AlertFetcher::start() {
  if (this->task_handle_ != nullptr) return;
  this->stop_requested_ = false;
  TaskHandle_t handle = nullptr;
  // TLS handshakes need a generous stack
  BaseType_t res = xTaskCreate(&AlertFetcher::task_entry_, "tt_alerts", 8192, this, 1, &handle);
  if (res != pdPASS) {
    ESP_LOGE(TAG, "Failed to start alerts task");
    return;
  }
  this->task_handle_ = handle;
}

void AlertFetcher::stop() {
  this->stop_requested_ = true;
  this->refresh();
}

void AlertFetcher::refresh() {
  if (this->task_handle_ != nullptr) xTaskNotifyGive(static_cast<TaskHandle_t>(this->task_handle_));
}

std::vector<Alert> AlertFetcher::get_alerts() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  return this->alerts_;
}

void AlertFetcher::task_entry_(void *arg) {
  static_cast<AlertFetcher *>(arg)->task_loop_();
  static_cast<AlertFetcher *>(arg)->task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

static bool alerts_equal(const std::vector<Alert> &a, const std::vector<Alert> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    const Alert &x = a[i], &y = b[i];
    if (x.id != y.id || x.title != y.title || x.message != y.message || x.route != y.route ||
        x.severity != y.severity || x.has_color != y.has_color || x.start != y.start || x.end != y.end ||
        (x.has_color && x.color.raw_32 != y.color.raw_32))
      return false;
  }
  return true;
}

void AlertFetcher::publish_(std::vector<Alert> &&alerts) {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (alerts_equal(this->alerts_, alerts)) return;
  this->alerts_ = std::move(alerts);
  this->count_ = this->alerts_.size();
  this->generation_++;
  ESP_LOGI(TAG, "Alerts updated: %u active", static_cast<unsigned>(this->alerts_.size()));
}

void AlertFetcher::task_loop_() {
  // Wait for the main websocket to connect (or give up waiting after a while if the server is down),
  // so that this request doesn't compete with it at boot
  for (uint32_t waited = 0; !this->network_ready_ && !this->stop_requested_ && waited < STARTUP_MAX_WAIT_MS;
       waited += STARTUP_POLL_MS) {
    vTaskDelay(pdMS_TO_TICKS(STARTUP_POLL_MS));
  }

  while (!this->stop_requested_) {
    std::string url = this->get_url();

    if (!url.empty() && esphome::network::is_connected()) {
      std::string body;
      std::vector<Alert> parsed;
      bool ok = this->fetch_(url, body) && parse_alerts_json(body, this->keys_, this->language_, parsed);
      body.clear();
      body.shrink_to_fit();

      if (ok) {
        this->consecutive_failures_ = 0;
        this->last_fetch_ok_ = true;
        this->publish_(std::move(parsed));
      } else {
        this->last_fetch_ok_ = false;
        if (++this->consecutive_failures_ == FAILURES_BEFORE_CLEAR) {
          ESP_LOGW(TAG, "Alerts endpoint failed %d times in a row; clearing alerts", FAILURES_BEFORE_CLEAR);
          this->publish_({});
        }
      }
    }

    // Sleep until the next poll, or until refresh() wakes us up
    uint32_t wait_ms = this->interval_ms_;
    if (this->consecutive_failures_ > 0 && this->consecutive_failures_ < FAILURES_BEFORE_CLEAR) {
      wait_ms = std::min<uint32_t>(wait_ms, 15000);  // retry sooner after a failure
    }
    if (url.empty() || !esphome::network::is_connected()) wait_ms = std::min<uint32_t>(wait_ms, 5000);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
  }
}

bool AlertFetcher::fetch_(const std::string &url, std::string &body) {
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

  for (const auto &h : this->headers_) esp_http_client_set_header(client, h.first.c_str(), h.second.c_str());
  esp_http_client_set_header(client, "Accept", "application/json");

  bool ok = false;
  esp_err_t err = ESP_OK;
  int64_t content_length = 0;
  int status = 0;

  // Open the connection, following up to 3 redirects
  for (int attempt = 0; attempt < 4; attempt++) {
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) break;
    content_length = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    bool is_redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (!is_redirect || attempt == 3) break;
    ESP_LOGD(TAG, "Following HTTP %d redirect", status);
    esp_http_client_flush_response(client, nullptr);
    esp_http_client_close(client);
    if (esp_http_client_set_redirection(client) != ESP_OK) {
      err = ESP_FAIL;
      break;
    }
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Alerts request failed: %s", esp_err_to_name(err));
  } else {
    if (status == 204) {
      body = "[]";
      ok = true;
    } else if (status < 200 || status >= 300) {
      ESP_LOGW(TAG, "Alerts endpoint returned HTTP %d", status);
    } else if (content_length > static_cast<int64_t>(this->max_response_size_)) {
      ESP_LOGW(TAG, "Alerts response too large (%lld bytes > max_response_size %u)", (long long) content_length,
               static_cast<unsigned>(this->max_response_size_));
    } else {
      char buf[512];
      ok = true;
      while (true) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
          ESP_LOGW(TAG, "Error reading alerts response");
          ok = false;
          break;
        }
        if (n == 0) break;
        if (body.size() + n > this->max_response_size_) {
          ESP_LOGW(TAG, "Alerts response exceeds max_response_size (%u bytes)",
                   static_cast<unsigned>(this->max_response_size_));
          ok = false;
          break;
        }
        body.append(buf, n);
      }
      if (ok && body.empty()) body = "[]";
      ESP_LOGD(TAG, "Fetched alerts (HTTP %d, %u bytes)", status, static_cast<unsigned>(body.size()));
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok;
}

#endif  // USE_ESP32

}  // namespace transit_tracker
}  // namespace esphome
