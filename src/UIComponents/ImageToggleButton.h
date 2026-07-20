#pragma once

#include "gtkmm/label.h"
#include "gtkmm/box.h"
#include <gtkmm/togglebutton.h>
#include <gtkmm/image.h>

// A toolbar-style toggle: same icon-over-label layout as ImageButton, but it
// stays pressed to reflect an on/off mode (e.g. the header-bar Keyboard mode).
class ImageToggleButton : public Gtk::ToggleButton {

  public:
    ImageToggleButton(const Glib::ustring label, const Glib::ustring icon,
                      Gtk::Orientation orientation = Gtk::Orientation::VERTICAL)
        : m_box(orientation) {
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
