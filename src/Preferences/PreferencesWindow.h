#pragma once

#include "Engine/DasherBridge.h"
#include "SettingsSection.h"
#include "gtkmm/box.h"
#include "gtkmm/stack.h"
#include "gtkmm/stacksidebar.h"
#include "gtkmm/window.h"
#include <memory>

class DwellClickHandler;

class PreferencesWindow : public Gtk::Window {
public:
  // dwell_handler is owned by the canvas (may be null); the Input section hosts
  // its on/off toggle, which used to live in the footer bar (issue #35).
  PreferencesWindow(std::shared_ptr<DasherBridge> bridge, DwellClickHandler* dwell_handler = nullptr);

private:
    void rebuild_sections();
    void add_locale_section();

    std::shared_ptr<DasherBridge> m_bridge;
    DwellClickHandler* m_dwell_handler = nullptr;
    Gtk::Box m_layout = Gtk::Box(Gtk::Orientation::HORIZONTAL);
    Gtk::StackSidebar m_sidebar;
    Gtk::Stack m_stack;

    std::vector<Gtk::Widget*> m_dynamic_pages;
};
