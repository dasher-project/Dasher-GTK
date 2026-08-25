#pragma once

#include "gtkmm/label.h"
#include "gtkmm/box.h"
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/icontheme.h>

// Toolbar button: icon beside label (horizontal). The previous vertical
// icon-over-label stack needed ~72px of height that action bars don't have,
// squashing every button into a thin strip; horizontal keeps one row.
// If the icon name is missing from the current theme (e.g. XFCE themes lack
// applications-system-symbolic) the icon is dropped and the label still
// shows, instead of rendering a broken-image glyph.
class ImageButton : public Gtk::Button {

  public:
    ImageButton(const Glib::ustring label, const Glib::ustring icon) : m_box(Gtk::Orientation::HORIZONTAL) {
        set_child(m_box);
        m_box.append(m_icon);
        m_box.append(m_label);

        m_label.set_label(label);
        m_label.set_margin(2);

        // Fallback chain: theme name -> generic preferences icon -> text-only.
        std::vector<Glib::ustring> fallbacks;
        if (icon.find("applications-system") != Glib::ustring::npos || icon.find("settings") != Glib::ustring::npos) {
            fallbacks.push_back("preferences-system-symbolic");
            fallbacks.push_back("emblem-system-symbolic");
        }
        auto paintable =
            Gtk::IconTheme::get_for_display(get_display())->lookup_icon(icon, fallbacks, 24, get_scale_factor());
        if (paintable) {
            m_icon.property_paintable() = paintable;
            m_icon.set_margin(2);
        } else {
            m_icon.set_visible(false);
        }
    }

  protected:
    Gtk::Box m_box;
    Gtk::Label m_label;
    Gtk::Image m_icon;
};
