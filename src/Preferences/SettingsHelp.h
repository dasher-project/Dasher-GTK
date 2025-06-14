#pragma once

#include "SettingsPageBase.h"
#include "gtkmm/enums.h"
#include "gtkmm/expander.h"
#include "gtkmm/frame.h"
#include "gtkmm/label.h"

class SettingsHelp : public SettingsPageBase
{
public:
    SettingsHelp();

    Gtk::Label m_dasher_text = Gtk::Label("");

    Gtk::Frame m_licensing_frame = Gtk::Frame("Thirdparty Licenses");
    Gtk::Box m_licenses_box = Gtk::Box(Gtk::Orientation::VERTICAL);

private:
    void AddLicense(Glib::ustring title, Glib::ustring text);
};