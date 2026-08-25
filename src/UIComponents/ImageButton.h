#pragma once

#include "gtkmm/label.h"
#include "gtkmm/box.h"
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/iconpaintable.h>
#include <giomm/file.h>

// Toolbar button: Lucide icon (bundled in the gresource, RFC 0002) beside the
// label. Bundled SVGs are theme-independent — the previous theme-icon lookup
// produced broken-image glyphs wherever a name was missing (e.g. XFCE themes
// lack applications-system-symbolic) — and give GTK the same icon set
// Dasher-Windows and Dasher-Apple already ship.
class ImageButton : public Gtk::Button {

  public:
    // icon_name: a stem under /org/alternativeinterface/dasher/icons/
    ImageButton(const Glib::ustring label, const Glib::ustring icon_name) : m_box(Gtk::Orientation::HORIZONTAL) {
        set_child(m_box);
        m_box.append(m_icon);
        m_box.append(m_label);

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
    }

  protected:
    Gtk::Box m_box;
    Gtk::Label m_label;
    Gtk::Image m_icon;
};
