#pragma once

#include "SettingsPageBase.h"
#include "SettingsStore.h"
#include "UIComponents/SyncedStringDropdown.h"
#include "gtkmm/frame.h"
#include "gtkmm/grid.h"

class SettingsMode : public SettingsPageBase
{
public:
    SettingsMode(std::shared_ptr<Dasher::CSettingsStore> settings, std::shared_ptr<DasherController> controller);

    Gtk::Frame m_device_label = Gtk::Frame("Input Device");
    SyncedStringDropdown m_device_switch;

    Gtk::Frame m_device_specific = Gtk::Frame("Control Method Settings");
    Gtk::Grid m_device_specific_settings_grid;
    
    Gtk::Frame m_method_label = Gtk::Frame("Control Method");
    SyncedStringDropdown m_method_switch;

    Gtk::Frame m_method_specific = Gtk::Frame("Control Method Settings");
    Gtk::Grid m_method_specific_settings_grid;

protected:
    std::shared_ptr<Dasher::CSettingsStore> m_settings;

    void DeviceSwitched(Glib::ustring selectedString);
};