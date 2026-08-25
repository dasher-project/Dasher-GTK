#pragma once

#include <string>

namespace ui {

// Frontend-local UI preferences that aren't engine parameters, persisted to
// <XDG_CONFIG_HOME>/dasher/ui.conf (same directory as analytics.conf).
// Currently: keyboard-mode window opacity (RFC 0015 parity with
// Dasher-Windows' "Keyboard Mode Opacity" and Apple's directOpacity slider).
class UiSettings {
  public:
    static UiSettings load(const std::string& path = default_path());
    void save() const;

    // Keyboard-mode window opacity, 0.2–1.0. Windows defaults 0.85, Apple
    // 0.75; we match Windows (canvas over the target app should be mostly
    // opaque but let the user dial it down).
    double keyboard_opacity() const { return m_keyboard_opacity; }
    void set_keyboard_opacity(double v);

    static std::string default_path();

  private:
    std::string m_path;
    double m_keyboard_opacity = 0.85;
};

} // namespace ui
