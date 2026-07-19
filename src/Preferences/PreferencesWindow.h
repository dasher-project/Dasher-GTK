#pragma once

#include "Analytics/AnalyticsSettings.h"
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
    void rebuild_sections();
    void add_locale_section();
    void add_privacy_section();

    std::shared_ptr<DasherBridge> m_bridge;
    analytics::AnalyticsSettings m_analytics = analytics::AnalyticsSettings::load();
    Gtk::Box m_layout = Gtk::Box(Gtk::Orientation::HORIZONTAL);
    Gtk::StackSidebar m_sidebar;
    Gtk::Stack m_stack;

    std::vector<Gtk::Widget*> m_dynamic_pages;
};
