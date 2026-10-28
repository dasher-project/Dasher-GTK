#pragma once

#include "Engine/DasherBridge.h"
#include "Analytics/UiTelemetry.h"
#include <gtkmm/switch.h>
#include <memory>

class SyncedSwitch : public Gtk::Switch {
public:
  SyncedSwitch(int parameter_key, const std::string& param_name, std::shared_ptr<DasherBridge> bridge,
               const std::string& surface = "settings")
      : m_bridge(bridge), m_key(parameter_key), m_name(param_name), m_surface(surface) {
      set_halign(Gtk::Align::CENTER);
      set_valign(Gtk::Align::CENTER);
      set_active(m_bridge->get_bool_parameter(m_key));

      property_active().signal_changed().connect([this]() {
          m_bridge->set_bool_parameter(m_key, get_active());
          analytics::track_setting_bool(m_name, get_active(), m_surface);
      });
  }

    void update_from_bridge() {
        set_active(m_bridge->get_bool_parameter(m_key));
    }

protected:
    std::shared_ptr<DasherBridge> m_bridge;
    int m_key;
    std::string m_name;
    std::string m_surface;
};
