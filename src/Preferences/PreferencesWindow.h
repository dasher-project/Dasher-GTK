#pragma once

#include "Engine/DasherBridge.h"
#include "SettingsSection.h"
#include "gtkmm/box.h"
#include "gtkmm/stack.h"
#include "gtkmm/stacksidebar.h"
#include "gtkmm/window.h"
#include <memory>

class PreferencesWindow : public Gtk::Window {
public:
    PreferencesWindow(std::shared_ptr<DasherBridge> bridge);

private:
    std::shared_ptr<DasherBridge> m_bridge;
    Gtk::Box m_layout = Gtk::Box(Gtk::Orientation::HORIZONTAL);
    Gtk::StackSidebar m_sidebar;
    Gtk::Stack m_stack;
};
