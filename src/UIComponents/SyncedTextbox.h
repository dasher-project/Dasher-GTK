#pragma once

#include "Engine/DasherBridge.h"
#include <gtkmm/entry.h>
#include <memory>

class SyncedTextbox : public Gtk::Entry {
public:
    SyncedTextbox(int parameter_key, std::shared_ptr<DasherBridge> bridge)
        : m_bridge(bridge), m_key(parameter_key)
    {
        set_text(m_bridge->get_string_parameter(m_key));

        signal_activate().connect([this]() {
            m_bridge->set_string_parameter(m_key, get_text());
        }, false);
    }

    void update_from_bridge() {
        set_text(m_bridge->get_string_parameter(m_key));
    }

protected:
    std::shared_ptr<DasherBridge> m_bridge;
    int m_key;
};
