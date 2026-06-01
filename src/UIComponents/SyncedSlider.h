#pragma once

#include "Engine/DasherBridge.h"
#include <gtkmm/scale.h>
#include <memory>

class SyncedSlider : public Gtk::Scale {
public:
    SyncedSlider(int parameter_key, std::shared_ptr<DasherBridge> bridge, long min, long max, long step)
        : m_bridge(bridge), m_key(parameter_key)
    {
        set_digits(0);
        set_range(min, max);
        set_increments(step, step * 5);
        set_value(m_bridge->get_long_parameter(m_key));

        signal_value_changed().connect([this]() {
            m_bridge->set_long_parameter(m_key, static_cast<long>(get_value()));
        });
    }

    void update_from_bridge() {
        set_value(m_bridge->get_long_parameter(m_key));
    }

protected:
    std::shared_ptr<DasherBridge> m_bridge;
    int m_key;
};
