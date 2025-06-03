#pragma once

#include "gtkmm/label.h"
#include "gtkmm/box.h"
#include <gtkmm/button.h>
#include <gtkmm/image.h>

class ImageButton : public Gtk::Button
{

public:
    ImageButton(const Glib::ustring label, const Glib::ustring icon, Gtk::Orientation orientation = Gtk::Orientation::VERTICAL) : m_box(orientation) {
        set_child(m_box);
        m_box.append(m_icon);
        m_box.append(m_label);

        m_label.set_label(label);
        m_label.set_margin(2);

        m_icon.set_from_icon_name(icon);
        m_icon.set_margin(2);
    }

protected:
    Gtk::Box m_box;
    Gtk::Label m_label;
    Gtk::Image m_icon;
};