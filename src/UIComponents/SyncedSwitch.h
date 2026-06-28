#pragma once

#include "Engine/DasherBridge.h"
#include <gtkmm/switch.h>
#include <memory>

class SyncedSwitch : public Gtk::Switch {
public:
    SyncedSwitch(int parameter_key, std::shared_ptr<DasherBridge> bridge)
        : m_bridge(bridge), m_key(parameter_key)
    {
        set_halign(Gtk::Align::CENTER);
        set_valign(Gtk::Align::CENTER);
        set_active(m_bridge->get_bool_parameter(m_key));

        property_active().signal_changed().connect([this]() {
            m_bridge->set_bool_parameter(m_key, get_active());
        });
    }

    void update_from_bridge() {
        set_active(m_bridge->get_bool_parameter(m_key));
    }

protected:
    std::shared_ptr<DasherBridge> m_bridge;
    int m_key;
};
