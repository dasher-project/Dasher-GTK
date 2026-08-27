#pragma once

#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/iconpaintable.h>
#include <giomm/file.h>

// Info icon with a hover popover explaining the setting. Uses the bundled
// Lucide info icon (RFC 0002) — theme-icon names like help-about-symbolic
// render as stars on XFCE and other themes, which reads as decoration.
class PopoverMenuButtonInfo : public Gtk::Image {
public:
    PopoverMenuButtonInfo(const Glib::ustring& text) : m_label(text) {
        auto file = Gio::File::create_for_uri("resource:///org/alternativeinterface/dasher/icons/info.svg");
        if (file->query_exists()) {
            auto paintable = Gtk::IconPaintable::create(file, 16, 1);
            property_paintable() = paintable;
            set_pixel_size(16);
        } else {
            set_from_icon_name("dialog-question-symbolic");
        }

        m_popover.set_parent(*this);
        m_popover.set_child(m_label);
        m_popover.set_has_arrow(false);
        m_popover.set_autohide(false);

        auto controller = Gtk::EventControllerMotion::create();
        controller->signal_enter().connect([this](double, double) {
            m_popover.popup();
            grab_focus();
        });
        controller->signal_leave().connect([this]() {
            m_popover.popdown();
        });
        add_controller(controller);

        signal_destroy().connect([this]() {
            m_popover.unparent();
        });
    }

private:
    Gtk::Popover m_popover;
    Gtk::Label m_label;
};
