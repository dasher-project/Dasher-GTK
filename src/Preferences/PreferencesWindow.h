#pragma once

#include "SettingsHelp.h"
#include "SettingsMode.h"
#include "SettingsStore.h"
#include "gtkmm/enums.h"
#include "gtkmm/stack.h"
#include "gtkmm/stacksidebar.h"
#include "gtkmm/window.h"
#include "gtkmm/box.h"


class PreferencesWindow : public Gtk::Window
{

public:
    PreferencesWindow(std::shared_ptr<Dasher::CSettingsStore> settings, std::shared_ptr<DasherController> controller);
    Gtk::Box m_sideways_box = Gtk::Box(Gtk::Orientation::HORIZONTAL);
    Gtk::StackSidebar m_sidebar;
    Gtk::Stack m_settings_stack;

    //Dasher
    std::shared_ptr<DasherController> controller;
    std::shared_ptr<Dasher::CSettingsStore> settings;

    //Pages
    SettingsMode m_mode_page;
    SettingsHelp m_help_page;
};