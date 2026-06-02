#include "DwellClickHandler.h"
#include <cmath>
#include <algorithm>

DwellClickHandler::DwellClickHandler() = default;

DwellClickHandler::~DwellClickHandler() = default;

void DwellClickHandler::set_enabled(bool enabled) {
    m_enabled = enabled;
    if (!enabled) {
        m_tracking = false;
        m_clicked = false;
    }
}

bool DwellClickHandler::get_enabled() const {
    return m_enabled;
}

void DwellClickHandler::set_duration_ms(int ms) {
    m_duration_ms = std::max(100, ms);
}

int DwellClickHandler::get_duration_ms() const {
    return m_duration_ms;
}

void DwellClickHandler::set_radius(float radius) {
    m_radius = std::max(1.0f, radius);
}

float DwellClickHandler::get_radius() const {
    return m_radius;
}

void DwellClickHandler::on_pointer_move(float x, float y) {
    if (!m_enabled) return;

    m_current_x = x;
    m_current_y = y;

    if (m_clicked) return;

    if (!m_tracking) {
        m_tracking = true;
        m_start_x = x;
        m_start_y = y;
        m_start_time = std::chrono::steady_clock::now();
        return;
    }

    float dx = x - m_start_x;
    float dy = y - m_start_y;
    float dist = std::sqrt(dx * dx + dy * dy);

    if (dist > m_radius) {
        m_start_x = x;
        m_start_y = y;
        m_start_time = std::chrono::steady_clock::now();
    }
}

void DwellClickHandler::on_frame() {
    if (!m_enabled || !m_tracking || m_clicked) return;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_start_time).count();

    if (elapsed >= m_duration_ms) {
        m_clicked = true;
        m_signal_click.emit();

        Glib::signal_timeout().connect_once([this]() {
            m_signal_unclick.emit();
            m_clicked = false;
            m_tracking = false;
        }, 150);
    }
}

float DwellClickHandler::get_progress() const {
    if (!m_enabled || !m_tracking || m_clicked) return 0.0f;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_start_time).count();

    return std::min(1.0f, static_cast<float>(elapsed) / static_cast<float>(m_duration_ms));
}

bool DwellClickHandler::is_active() const {
    return m_enabled && m_tracking && !m_clicked;
}

float DwellClickHandler::get_center_x() const {
    return m_start_x;
}

float DwellClickHandler::get_center_y() const {
    return m_start_y;
}

sigc::signal<void()>& DwellClickHandler::signal_dwell_click() {
    return m_signal_click;
}

sigc::signal<void()>& DwellClickHandler::signal_dwell_unclick() {
    return m_signal_unclick;
}
