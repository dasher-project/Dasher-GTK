#pragma once

#include "Engine/DasherBridge.h"
#include "Analytics/UiTelemetry.h"
#include <gtkmm/entry.h>
#include <memory>

class SyncedTextbox : public Gtk::Entry {
public:
    SyncedTextbox(int parameter_key, const std::string& param_name, std::shared_ptr<DasherBridge> bridge, const std::string& surface = "settings")
        : m_bridge(bridge), m_key(parameter_key), m_name(param_name), m_surface(surface)
    {
        set_text(m_bridge->get_string_parameter(m_key));

        signal_activate().connect([this]() {
            m_bridge->set_string_parameter(m_key, get_text());
            // RFC 0001 amendment: free text — the VALUE is never collected.
            analytics::track_setting_string_changed(m_name, m_surface);
        }, false);
    }

    void update_from_bridge() {
        set_text(m_bridge->get_string_parameter(m_key));
    }

protected:
    std::shared_ptr<DasherBridge> m_bridge;
    int m_key;
    std::string m_name;
    std::string m_surface;
};
