#pragma once

#include "Analytics/AnalyticsSettings.h"
#include "Engine/DasherBridge.h"
#include "SettingsSection.h"
#include "UpdateChecker.h"
#include "gtkmm/alertdialog.h"
#include "gtkmm/box.h"
#include "gtkmm/button.h"
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
<<<<<<< HEAD
=======

    friend class ::PrefsRebuildSelftest;

    // LIFETIME INVARIANT: MainWindow holds this window in a unique_ptr for
    // the whole app session (close only hides it), so `this` outlives every
    // async dialog callback launched from here. If that ever changes, the
    // captured-`this` callbacks below (training import/export/reset) must
    // switch to Glib::WeakRef validation at entry.

  private:
>>>>>>> a96be97 (polish: review-loop round 2 — honest Reset, cap-before-read, boundaries)
    // Speech/TTS page — built once, never rebuilt (issue #42 lifetime hazard).
    void add_speech_section();
    void add_locale_section();
    void add_privacy_section();
    void update_rate_readout();

    // Training-data row (Language page). The widgets live in the rebuilt
    // dynamic pages, so the pointers are nulled at the top of
    // rebuild_sections() (m_rate_value pattern) and every deferred user of
    // them — including async dialog callbacks that may still be in flight
    // across a rebuild — must null-check via refresh_training_row().
    void refresh_training_row();
    Gtk::Label* m_training_size_label = nullptr;
    Gtk::Label* m_training_status = nullptr;
    Gtk::Button* m_training_export_btn = nullptr;
    Gtk::Button* m_training_reset_btn = nullptr;

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
    // RFC 0017 update-check opt-out. The switch state comes from
    // load_update_check_pref() (UiSettings) and saves via UiSettings —
    // there is no cached member to drift out of sync with the persisted
    // value (the original Greptile finding).
    static bool load_update_check_pref();
    void save_update_check_pref(bool enabled);
    analytics::AnalyticsSettings m_analytics = analytics::AnalyticsSettings::load();
    Gtk::Box m_layout = Gtk::Box(Gtk::Orientation::HORIZONTAL);
    Gtk::StackSidebar m_sidebar;
    Gtk::Stack m_stack;

    std::vector<Gtk::Widget*> m_dynamic_pages;
};
