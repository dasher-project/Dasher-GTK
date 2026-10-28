#pragma once

#include "Engine/DasherBridge.h"
#include "Analytics/UiTelemetry.h"
#include <gtkmm/spinbutton.h>
#include <memory>

class SyncedSpinButton : public Gtk::SpinButton {
public:
  SyncedSpinButton(int parameter_key, const std::string& param_name, std::shared_ptr<DasherBridge> bridge, long min,
                   long max, long step, const std::string& surface = "settings")
      : m_bridge(bridge), m_key(parameter_key), m_name(param_name), m_surface(surface) {
      set_digits(0);
      set_range(min, max);
      set_increments(step, step * 5);
      set_value(m_bridge->get_long_parameter(m_key));

      signal_value_changed().connect([this]() {
          m_bridge->set_long_parameter(m_key, get_value_as_int());
          analytics::track_setting_number(m_name, get_value_as_int(), m_surface);
      });
  }

    void update_from_bridge() {
        set_value(m_bridge->get_long_parameter(m_key));
    }

protected:
    std::shared_ptr<DasherBridge> m_bridge;
    int m_key;
    std::string m_name;
    std::string m_surface;
};
