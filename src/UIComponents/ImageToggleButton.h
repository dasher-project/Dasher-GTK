#pragma once

#include "gtkmm/label.h"
#include "gtkmm/box.h"
#include <gtkmm/togglebutton.h>
#include <gtkmm/image.h>
#include <gtkmm/iconpaintable.h>
#include <giomm/file.h>

// A toolbar-style toggle: same Lucide icon + label layout as ImageButton, but
// it stays pressed to reflect an on/off mode (the header-bar Keyboard mode).
class ImageToggleButton : public Gtk::ToggleButton {

  public:
    ImageToggleButton(const Glib::ustring label, const Glib::ustring icon_name) : m_box(Gtk::Orientation::HORIZONTAL) {
        m_label.set_label(label);
        m_label.set_margin(2);

        auto file =
            Gio::File::create_for_uri("resource:///org/alternativeinterface/dasher/icons/" + icon_name + ".svg");
        if (file->query_exists()) {
            auto paintable = Gtk::IconPaintable::create(file, 24, get_scale_factor());
            m_icon.property_paintable() = paintable;
            m_icon.set_pixel_size(18);
            m_icon.set_margin_end(4);
        } else {
            m_icon.set_visible(false);
        }

        m_box.append(m_icon);
        m_box.append(m_label);
        set_child(m_box);
    }

  protected:
    Gtk::Box m_box;
    Gtk::Label m_label;
    Gtk::Image m_icon;
};
