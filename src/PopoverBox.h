#include "gtkmm/button.h"
#include "gtkmm/enums.h"
#include "gtkmm/revealer.h"
#include <gtkmm.h>

class PopoverBox : public Gtk::Revealer
{

public:
    PopoverBox() {
        set_name("PopoverBox");
        
        set_valign(Gtk::Align::END);
        set_vexpand(false);
        set_halign(Gtk::Align::CENTER);
        set_hexpand(false);

        set_child(m_box);
        m_box.set_halign(Gtk::Align::CENTER);
        m_box.set_hexpand(false);
        m_box.append(m_icon);
        m_box.append(m_label);
        m_box.append(m_close);

        m_icon.set_from_icon_name("dialog-warning-symbolic");
        m_label.set_text("");

        m_close.set_image_from_icon_name("process-stop-symbolic");
        m_close.signal_clicked().connect([this](){
            set_reveal_child(false);
        });
        set_reveal_child(false);
    }

    void reveal(std::string text){
        m_label.set_text(text);
        set_reveal_child(true);
    }

protected:
    Gtk::Box m_box;
    Gtk::Label m_label;
    Gtk::Image m_icon;
    Gtk::Button m_close;
};