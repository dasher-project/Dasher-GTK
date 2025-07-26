#pragma once

#include "glibmm/ustring.h"
#include "gtkmm/label.h"
#include "gtkmm/image.h"
#include "gtkmm/popover.h"
#include "gtkmm/eventcontrollermotion.h"

#pragma clang optimize off
class PopoverMenuButtonInfo : public Gtk::Image
{

public:
    PopoverMenuButtonInfo(Glib::ustring text) : m_label(text) {
        set_from_icon_name("help-about-symbolic");
        set_name("InfoPopup");

        m_popover.set_parent(*this);
        m_popover.set_child(m_label);
        m_popover.set_has_arrow(false);
        m_popover.set_autohide(false);
        
        // Open on mouse-over
        m_mouseController = Gtk::EventControllerMotion::create();
        m_mouseController->signal_enter().connect([this](double, double){
            m_popover.popup();
            grab_focus(); //As focus is shifted to the popup, signal_leave is immediately executed. Grab focus and disable autohide to prevent this.
        });
        m_mouseController->signal_leave().connect([this](){
            m_popover.popdown();
        });
        add_controller(m_mouseController);
    
        signal_destroy().connect([this](){
            m_popover.unparent();
        });
    }

    Glib::ustring GetText(){return m_label.get_text();}
    void SetText(Glib::ustring text){return m_label.set_text(text);}

protected:
    Gtk::Popover m_popover;
    Gtk::Label m_label;
    Glib::RefPtr<Gtk::EventControllerMotion> m_mouseController;
};
#pragma clang optimize on