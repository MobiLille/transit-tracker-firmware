#include "config_editor.h"

#ifdef USE_TRANSIT_TRACKER_CONFIG_EDITOR

#include "transit_tracker.h"

#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/json/json_util.h"

namespace esphome {
namespace transit_tracker {

static const char *const TAG = "transit_tracker.config_editor";

void ConfigEditor::setup() {
  auto mark_dirty = [this](auto &&...) { this->dirty_ = true; };
  for (auto &t : this->texts_) t.second->add_on_state_callback(mark_dirty);
  for (auto &s : this->selects_) s.second->add_on_state_callback(mark_dirty);
  for (auto &s : this->switches_) s.second->add_on_state_callback(mark_dirty);

  this->rebuild_snapshot_();
  web_server_base::global_web_server_base->add_handler(this);
}

void ConfigEditor::loop() {
  if (this->has_pending_.exchange(false)) {
    this->apply_pending_();
  }
  if (this->dirty_.exchange(false)) {
    this->rebuild_snapshot_();
  }
}

void ConfigEditor::dump_config() {
  ESP_LOGCONFIG(TAG, "Transit Tracker config editor:");
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  Entities: %u texts, %u selects, %u switches", (unsigned) this->texts_.size(),
                (unsigned) this->selects_.size(), (unsigned) this->switches_.size());
}

void ConfigEditor::rebuild_snapshot_() {
  std::string json = json::build_json([this](JsonObject root) {
    for (auto &t : this->texts_) root[t.first] = t.second->state;
    for (auto &s : this->selects_) root[s.first] = s.second->current_option().str();
    for (auto &s : this->switches_) root[s.first] = s.second->state;
  });
  std::lock_guard<std::mutex> lock(this->mutex_);
  this->snapshot_ = std::move(json);
}

void ConfigEditor::apply_pending_() {
  std::vector<PendingValue> values;
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    values.swap(this->pending_);
  }

  // Values were validated by parse_body_(), keys are known to exist
  for (const auto &v : values) {
    for (auto &t : this->texts_) {
      if (t.first == v.key && t.second->state != v.value) t.second->make_call().set_value(v.value).perform();
    }
    for (auto &s : this->selects_) {
      if (s.first == v.key && s.second->current_option().str() != v.value)
        s.second->make_call().set_option(v.value).perform();
    }
    for (auto &s : this->switches_) {
      if (s.first == v.key) {
        bool on = v.value == "1";
        if (s.second->state != on) on ? s.second->turn_on() : s.second->turn_off();
      }
    }
  }

  global_preferences->sync();
  ESP_LOGI(TAG, "Configuration updated from the web dashboard (%u values)", (unsigned) values.size());
  if (this->tracker_ != nullptr) {
    this->tracker_->reconnect("configuration saved");
  }
  this->dirty_ = true;
}

bool ConfigEditor::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET && request->method() != HTTP_POST) return false;
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  return this->path_ == request->url_to(buf).c_str();
}

void ConfigEditor::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                              size_t total) {
  if (index == 0) {
    this->body_.clear();
    this->body_too_large_ = total > MAX_BODY_SIZE;
    if (!this->body_too_large_) this->body_.reserve(total);
  }
  if (!this->body_too_large_) this->body_.append(reinterpret_cast<const char *>(data), len);
}

bool ConfigEditor::parse_body_(std::vector<PendingValue> &values, std::string &error) {
  bool valid = json::parse_json(this->body_, [this, &values, &error](JsonObject root) -> bool {
    for (JsonPair kv : root) {
      std::string key = kv.key().c_str();
      bool known = false;

      for (auto &t : this->texts_) {
        if (t.first != key) continue;
        known = true;
        if (!kv.value().is<const char *>()) {
          error = "'" + key + "' doit être une chaîne de caractères";
          return true;
        }
        std::string value = kv.value().as<std::string>();
        int max_length = t.second->traits.get_max_length();
        int min_length = t.second->traits.get_min_length();
        if (max_length > 0 && (int) value.size() > max_length) {
          error = "'" + key + "' est trop long (" + std::to_string(value.size()) + " octets, maximum " +
                  std::to_string(max_length) + ")";
          return true;
        }
        if ((int) value.size() < min_length) {
          error = "'" + key + "' est trop court (minimum " + std::to_string(min_length) + ")";
          return true;
        }
        values.push_back({key, std::move(value)});
      }

      for (auto &s : this->selects_) {
        if (s.first != key) continue;
        known = true;
        std::string value = kv.value().is<const char *>() ? kv.value().as<std::string>() : "";
        if (!s.second->has_option(value)) {
          error = "'" + key + "' : option invalide (valeurs possibles :";
          for (const char *option : s.second->traits.get_options()) error += std::string(" ") + option;
          error += ")";
          return true;
        }
        values.push_back({key, std::move(value)});
      }

      for (auto &s : this->switches_) {
        if (s.first != key) continue;
        known = true;
        if (!kv.value().is<bool>()) {
          error = "'" + key + "' doit valoir true ou false";
          return true;
        }
        values.push_back({key, kv.value().as<bool>() ? "1" : "0"});
      }

      if (!known) {
        error = "Clé inconnue : '" + key + "'";
        return true;
      }
    }
    return true;
  });

  if (!valid && error.empty()) error = "JSON invalide";
  return valid && error.empty();
}

void ConfigEditor::handleRequest(AsyncWebServerRequest *request) {
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

  if (this->body_too_large_) {
    this->body_.clear();
    request->send(413, "text/plain; charset=utf-8", "Configuration trop volumineuse");
    return;
  }

  std::vector<PendingValue> values;
  std::string error;
  bool ok = this->parse_body_(values, error);
  this->body_.clear();
  if (!ok) {
    ESP_LOGW(TAG, "Rejected configuration: %s", error.c_str());
    request->send(400, "text/plain; charset=utf-8", error.c_str());
    return;
  }

  // Entities may only be changed from the main loop
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->pending_ = std::move(values);
  }
  this->has_pending_ = true;
  request->send(200, "application/json", "{\"ok\":true}");
}

}  // namespace transit_tracker
}  // namespace esphome

#endif  // USE_TRANSIT_TRACKER_CONFIG_EDITOR
