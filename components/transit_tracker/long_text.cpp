#include "long_text.h"

#ifdef USE_TRANSIT_TRACKER_LONG_TEXT

#include <vector>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace transit_tracker {

static const char *const TAG = "transit_tracker.long_text";

// Value stored as a 2-byte little-endian length followed by max_length bytes
static constexpr size_t LENGTH_PREFIX = 2;
// Layout of `platform: template` text with restore_value and the default max_length
static constexpr size_t LEGACY_MAX_LENGTH = 255;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

void LongText::setup() {
  const size_t max_length = this->traits.get_max_length();
  uint32_t key = this->get_preference_hash() ^ fnv1_hash("transit_tracker.long_text");
  key += static_cast<uint32_t>(max_length) << 4;
  this->pref_ = global_preferences->make_preference(LENGTH_PREFIX + max_length, key);

  std::string value = this->initial_value_;
  std::vector<uint8_t> buffer(LENGTH_PREFIX + max_length);
  if (this->pref_.load(buffer.data(), buffer.size())) {
    size_t len = std::min<size_t>(buffer[0] | (buffer[1] << 8), max_length);
    value.assign(reinterpret_cast<const char *>(buffer.data() + LENGTH_PREFIX), len);
  } else if (this->load_legacy_template_value_(value)) {
    ESP_LOGI(TAG, "'%s': value migrated from the template text entity", this->get_name().c_str());
    this->control(value);
    return;
  }

  if (!value.empty()) this->publish_state(value);
}

bool LongText::load_legacy_template_value_(std::string &value) {
  // Same key as TemplateText::setup() with min_length 0, max_length 255 and no pattern
  uint32_t key = this->get_preference_hash();
  key += 0 << 2;
  key += static_cast<uint32_t>(LEGACY_MAX_LENGTH) << 4;
  key += fnv1_hash("") << 6;

  uint8_t buffer[LEGACY_MAX_LENGTH + 1];
  if (!global_preferences->load_from_key(key, buffer, sizeof(buffer))) return false;
  value.assign(reinterpret_cast<const char *>(buffer + 1), std::min<size_t>(buffer[0], LEGACY_MAX_LENGTH));
  return true;
}

#pragma GCC diagnostic pop

void LongText::control(const std::string &value) {
  this->publish_state(value);

  const size_t max_length = this->traits.get_max_length();
  if (value.size() > max_length) {
    ESP_LOGW(TAG, "'%s': value too long to save (%u > %u)", this->get_name().c_str(), (unsigned) value.size(),
             (unsigned) max_length);
    return;
  }
  std::vector<uint8_t> buffer(LENGTH_PREFIX + max_length, 0);
  buffer[0] = value.size() & 0xFF;
  buffer[1] = (value.size() >> 8) & 0xFF;
  memcpy(buffer.data() + LENGTH_PREFIX, value.data(), value.size());
  this->pref_.save(buffer.data(), buffer.size());
}

void LongText::dump_config() {
  LOG_TEXT("", "Transit Tracker long text", this);
  ESP_LOGCONFIG(TAG, "  Max length: %d", this->traits.get_max_length());
}

}  // namespace transit_tracker
}  // namespace esphome

#endif  // USE_TRANSIT_TRACKER_LONG_TEXT
