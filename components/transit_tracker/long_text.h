#pragma once

#include "esphome/core/defines.h"

#ifdef USE_TRANSIT_TRACKER_LONG_TEXT

#include "esphome/components/text/text.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace transit_tracker {

/// Optimistic text entity restored from flash, like `platform: template` with `restore_value: true`,
/// but not limited to 255 bytes (the template saver stores the length in a single byte).
class LongText : public text::Text, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_initial_value(const char *initial_value) { this->initial_value_ = initial_value; }

 protected:
  void control(const std::string &value) override;
  bool load_legacy_template_value_(std::string &value);

  const char *initial_value_{""};
  ESPPreferenceObject pref_;
};

}  // namespace transit_tracker
}  // namespace esphome

#endif  // USE_TRANSIT_TRACKER_LONG_TEXT
