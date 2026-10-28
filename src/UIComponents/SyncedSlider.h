#pragma once

#include "Engine/DasherBridge.h"
#include "Analytics/UiTelemetry.h"
#include <gtkmm/scale.h>
#include <gtkmm/gesturedrag.h>
#include <memory>

class SyncedSlider : public Gtk::Scale {
public:
    SyncedSlider(int parameter_key, const std::string& param_name, std::shared_ptr<DasherBridge> bridge, long min, long max, long step, const std::string& surface = "settings")
        : m_bridge(bridge), m_key(parameter_key), m_name(param_name), m_surface(surface)
    {
        set_digits(0);
        set_range(min, max);
        set_increments(step, step * 5);
        set_value(m_bridge->get_long_parameter(m_key));

        signal_value_changed().connect([this]() {
            m_bridge->set_long_parameter(m_key, static_cast<long>(get_value()));
            // RFC 0001 amendment: one telemetry event per COMMITTED change —
            // during a pointer drag the engine write stays live but the
            // telemetry waits for drag end (per-frame events would inflate
            // the UX counts 30x per drag).
            if (!m_dragging) analytics::track_setting_number(m_name, static_cast<long>(get_value()), m_surface);
        });

        auto drag = Gtk::GestureDrag::create();
        add_controller(drag);
        drag->signal_drag_begin().connect([this](double, double) { m_dragging = true; });
        drag->signal_drag_end().connect([this](double, double) {
            m_dragging = false;
            analytics::track_setting_number(m_name, static_cast<long>(get_value()), m_surface);
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
    bool m_dragging = false;
};
