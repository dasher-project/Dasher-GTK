#pragma once

#include "SettingsPageBase.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/enums.h"
#include "gtkmm/frame.h"


class SettingsMode : public SettingsPageBase
{
    public:
        SettingsMode(std::shared_ptr<DasherController> controller);

    Gtk::Frame m_device_label = Gtk::Frame("Input Device");
    Gtk::DropDown m_device_switch;
    
    Gtk::Frame m_method_label = Gtk::Frame("Control Method");
    Gtk::DropDown m_method_switch;
};