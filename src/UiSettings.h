#pragma once

#include <string>

namespace ui {

// Frontend-local UI preferences that aren't engine parameters, persisted to
// <XDG_CONFIG_HOME>/dasher/ui.conf (same directory as analytics.conf).
// Main-thread only — the update checker's background thread writes its own
// separate state file (update-check.conf) so there is no cross-thread
// mutation of this file.
class UiSettings {
  public:
    static UiSettings load(const std::string& path = default_path());
    void save() const;

    // Keyboard-mode window opacity, 0.2–1.0. Windows defaults 0.85, Apple
    // 0.75; we match Windows (canvas over the target app should be mostly
    // opaque but let the user dial it down).
    double keyboard_opacity() const { return m_keyboard_opacity; }
    void set_keyboard_opacity(double v);

    // RFC 0017: whether the passive update check runs (opt-out toggle in
    // Preferences → Privacy). Lives here so the preferences UI and the
    // launch-time check read the same persisted value without racing the
    // background thread's timestamp writes.
    bool update_checks_enabled() const { return m_update_checks_enabled; }
    void set_update_checks_enabled(bool v);

    static std::string default_path();

  private:
    std::string m_path;
    double m_keyboard_opacity = 0.85;
    bool m_update_checks_enabled = true;
};

} // namespace ui
