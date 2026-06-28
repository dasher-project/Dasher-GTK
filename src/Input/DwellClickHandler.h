#pragma once

#include <sigc++/signal.h>
#include <glibmm/main.h>
#include <chrono>

class DwellClickHandler {
public:
    DwellClickHandler();
    ~DwellClickHandler();

    void set_enabled(bool enabled);
    bool get_enabled() const;

    void set_duration_ms(int ms);
    int get_duration_ms() const;

    void set_radius(float radius);
    float get_radius() const;

    void on_pointer_move(float x, float y);
    void on_frame();

    float get_progress() const;
    bool is_active() const;
    float get_center_x() const;
    float get_center_y() const;

    sigc::signal<void()>& signal_dwell_click();
    sigc::signal<void()>& signal_dwell_unclick();

private:
    bool m_enabled = false;
    int m_duration_ms = 800;
    float m_radius = 20.0f;

    float m_start_x = 0.0f;
    float m_start_y = 0.0f;
    float m_current_x = 0.0f;
    float m_current_y = 0.0f;

    bool m_tracking = false;
    bool m_clicked = false;
    std::chrono::steady_clock::time_point m_start_time;

    sigc::signal<void()> m_signal_click;
    sigc::signal<void()> m_signal_unclick;
};
