
#include "SettingsHelp.h"
#include "SettingsPageBase.h"
#include "glibmm/fileutils.h"
#include "gtkmm/enums.h"
#include "gtkmm/expander.h"
#include "gtkmm/label.h"
#include "gtkmm/object.h"
#include "gtkmm/scrolledwindow.h"

SettingsHelp::SettingsHelp(): SettingsPageBase("Help", "About & License") {
    append(m_dasher_text);
    append(m_licensing_frame);
    m_licensing_frame.set_child(m_licenses_box);
    m_licensing_frame.set_halign(Gtk::Align::FILL);
    m_licensing_frame.set_hexpand(true);

    m_dasher_text.set_markup(Glib::ustring("<big>Dasher v6</big>\r\nBuilt Time: ") + __DATE__ + ", " + __TIME__);
    m_dasher_text.set_justify(Gtk::Justification::CENTER);
    m_dasher_text.set_use_markup(true);
    m_dasher_text.set_halign(Gtk::Align::FILL);

    //Add License Texts
    AddLicense("GTK4", "LICENSE_GTK4.txt");
    AddLicense("GTKmm", "LICENSE_GTKmm.txt");
    AddLicense("PugiXML", "LICENSE_PugiXML.txt");
    AddLicense("SDL2", "LICENSE_SDL.txt");
}

void SettingsHelp::AddLicense(Glib::ustring title, Glib::ustring filename){
    Gtk::Expander* expander = Gtk::make_managed<Gtk::Expander>(title);
    Gtk::Label* label = Gtk::make_managed<Gtk::Label>(Glib::file_get_contents("Resources/License/" + filename));
    Gtk::ScrolledWindow* scrolledWindow = Gtk::make_managed<Gtk::ScrolledWindow>();

    label->set_justify(Gtk::Justification::CENTER);
    label->set_wrap(true);
    scrolledWindow->set_has_frame(true);
    scrolledWindow->set_min_content_width(500);
    scrolledWindow->set_min_content_height(500);

    scrolledWindow->set_child(*label);
    expander->set_child(*scrolledWindow);
    m_licenses_box.append(*expander);
}