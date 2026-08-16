#pragma once

#include "Analytics/AnalyticsSettings.h"
#include "Engine/DasherBridge.h"
#include "SettingsSection.h"
#include "gtkmm/alertdialog.h"
#include "gtkmm/box.h"
#include "gtkmm/stack.h"
#include "gtkmm/stacksidebar.h"
#include "gtkmm/window.h"
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
    // Live CPS/WPM readout hosted in Settings -> Output (moved from the footer, issue #35).
    Gtk::Label* m_rate_value = nullptr;
    analytics::AnalyticsSettings m_analytics = analytics::AnalyticsSettings::load();
    Gtk::Box m_layout = Gtk::Box(Gtk::Orientation::HORIZONTAL);
    Gtk::StackSidebar m_sidebar;
    Gtk::Stack m_stack;

    std::vector<Gtk::Widget*> m_dynamic_pages;
};
