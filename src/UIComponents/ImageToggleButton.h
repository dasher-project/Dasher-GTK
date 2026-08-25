#pragma once

#include "gtkmm/label.h"
#include "gtkmm/box.h"
#include <gtkmm/togglebutton.h>
#include <gtkmm/image.h>
#include <gtkmm/icontheme.h>

// A toolbar-style toggle: same horizontal icon+label layout as ImageButton,
// but it stays pressed to reflect an on/off mode (e.g. the header-bar
// Keyboard mode). Icons fall back the same way when the theme lacks them.
class ImageToggleButton : public Gtk::ToggleButton {

  public:
    ImageToggleButton(const Glib::ustring label, const Glib::ustring icon) : m_box(Gtk::Orientation::HORIZONTAL) {
        m_box.append(m_icon);
        m_box.append(m_label);

        m_label.set_label(label);
        m_label.set_margin(2);

        auto paintable = Gtk::IconTheme::get_for_display(get_display())->lookup_icon(icon, 24, get_scale_factor());
        if (paintable) {
            m_icon.property_paintable() = paintable;
            m_icon.set_margin(2);
        } else {
            m_icon.set_visible(false);
        }

        set_child(m_box);
    }

  protected:
    Gtk::Box m_box;
    Gtk::Label m_label;
    Gtk::Image m_icon;
};
