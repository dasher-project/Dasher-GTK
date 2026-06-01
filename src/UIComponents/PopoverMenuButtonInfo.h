#pragma once

#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>

class PopoverMenuButtonInfo : public Gtk::Image {
public:
    PopoverMenuButtonInfo(const Glib::ustring& text) : m_label(text) {
        set_from_icon_name("help-about-symbolic");
        set_name("InfoPopup");

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
