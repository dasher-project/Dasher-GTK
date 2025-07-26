#pragma once

#include "gtkmm/grid.h"

class DeviceSettingsProvider {
public:
    virtual bool FillInputDeviceSettings(Gtk::Grid* grid){return false;}
};