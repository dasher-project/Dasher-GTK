#pragma once

#include "Analytics/AnalyticsSettings.h"
#include "Engine/DasherBridge.h"
#include "SettingsSection.h"
#include "gtkmm/alertdialog.h"
#include "gtkmm/box.h"
#include "UIComponents/SyncedColorDropdown.h"
#include "gtkmm/fontdialogbutton.h"
#include "gtkmm/stack.h"
#include "gtkmm/stacksidebar.h"
#include "gtkmm/window.h"
#include <functional>
#include <memory>
#include <sigc++/signal.h>

class DwellClickHandler;

// tests/test_prefs_rebuild_selftest.cpp — drives the private rebuild path to
// reproduce issue #42 (rebuild_sections() use-after-free) headlessly.
class PrefsRebuildSelftest;

class PreferencesWindow : public Gtk::Window {
public:
  // dwell_handler is owned by the canvas (may be null); the Input section hosts
  // its on/off toggle, which used to live in the footer bar (issue #35).
  PreferencesWindow(std::shared_ptr<DasherBridge> bridge, DwellClickHandler* dwell_handler = nullptr);

  // Keyboard-mode window opacity (RFC 0015 parity with Windows/Apple):
  // getter returns the persisted value; setter persists + applies live.
  void set_keyboard_opacity_access(std::function<double()> get, std::function<void(double)> set);

  // Build the appearance controls (colour palette + canvas font) into the
  // Customization tab, owned by this window (RFC 0006; Dasher-Windows/Apple
  // placement). on_font_changed fires when the user picks a canvas font —
  // MainWindow applies it to the renderer/engine there. The colour dropdown
  // drives the bridge directly.
  void set_appearance_handler(std::function<void(const Glib::ustring& family, bool italic, bool bold)> on_font_changed);

  // Emitted after "Reset engine settings to defaults" so footer-bar widgets
  // (speed/alphabet/learning/colour) can re-read from the engine.
  sigc::signal<void()> OnSettingsReset;

  friend class ::PrefsRebuildSelftest;

private:
    void rebuild_sections();
    // Speech/TTS page — built once, never rebuilt (issue #42 lifetime hazard).
    void add_speech_section();
    void add_locale_section();
    void add_privacy_section();
    void update_rate_readout();

    std::shared_ptr<DasherBridge> m_bridge;
    DwellClickHandler* m_dwell_handler = nullptr;
    std::function<double()> m_keyboard_opacity_get;
    std::function<void(double)> m_keyboard_opacity_set;
    std::function<void(const Glib::ustring&, bool, bool)> m_on_font_changed;
    // Appearance widgets owned here, refreshed on settings-reset.
    Gtk::Widget* m_reset_color_chooser = nullptr;
    Gtk::Widget* m_reset_font_btn = nullptr;
    // Live CPS/WPM readout hosted in Settings -> Output (moved from the footer, issue #35).
    Gtk::Label* m_rate_value = nullptr;
    analytics::AnalyticsSettings m_analytics = analytics::AnalyticsSettings::load();
    Gtk::Box m_layout = Gtk::Box(Gtk::Orientation::HORIZONTAL);
    Gtk::StackSidebar m_sidebar;
    Gtk::Stack m_stack;

    std::vector<Gtk::Widget*> m_dynamic_pages;
};
