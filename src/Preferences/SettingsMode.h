#pragma once

#include "SettingsPageBase.h"
#include "SettingsStore.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/frame.h"
#include "gtkmm/grid.h"


class SettingsMode : public SettingsPageBase
{
public:
    SettingsMode(std::shared_ptr<Dasher::CSettingsStore> settings, std::shared_ptr<DasherController> controller);

    Gtk::Frame m_device_label = Gtk::Frame("Input Device");
    Gtk::DropDown m_device_switch;
    
    Gtk::Frame m_method_label = Gtk::Frame("Control Method");
    Gtk::DropDown m_method_switch;

    Gtk::Frame m_method_specific = Gtk::Frame("Settings");
    Gtk::Grid m_method_specific_settings_grid;

protected:
    std::shared_ptr<Dasher::CSettingsStore> m_settings;
};