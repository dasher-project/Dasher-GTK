#pragma once

#include "giomm/liststore.h"
#include "glibmm/ustring.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/enums.h"
#include "gtkmm/label.h"
#include "gtkmm/signallistitemfactory.h"
#include <memory>

class EnumProxy : public Glib::Object
{
    public:
        EnumProxy(const Glib::ustring name, const int value): name(name), value(value){}
        const Glib::ustring name;
        const int value;
        static Glib::RefPtr<EnumProxy> create(const Glib::ustring name, const int value)
        {
            return Glib::make_refptr_for_instance<EnumProxy>(new EnumProxy(name, value));
        }
};

class EnumDropdown : public Gtk::DropDown {
public:
    Glib::RefPtr<Gio::ListStore<EnumProxy>> valueList = Gio::ListStore<EnumProxy>::create();
    Glib::RefPtr<Gtk::SignalListItemFactory> itemFactory = Gtk::SignalListItemFactory::create();
    
    EnumDropdown(const std::map<std::string, int>& Enums, int currentlySelected){
        //Init values
        for(auto& [key, value] : Enums){
            valueList->append(EnumProxy::create(key, value));
        }

        set_model(valueList);
        set_factory(itemFactory);

        itemFactory->signal_setup().connect(&EnumDropdown::SetupItemList);
        itemFactory->signal_bind().connect(&EnumDropdown::BindItemList);

        // Set inital state
        SetSelected(currentlySelected);
    }

    bool SetSelected(const int enum_value){
        for(guint i = 0; i < valueList->get_n_items(); i++){
            if(valueList->get_item(i)->value == enum_value){
                set_selected(i);
                return true;
            }
        }
        return false;
    }

    int GetSelected(){
        return std::dynamic_pointer_cast<EnumProxy>(get_selected_item())->value;
    }

protected:
    // Functions for List Items
    static void SetupItemList(const std::shared_ptr<Gtk::ListItem> item){
        Gtk::Label* label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        item->set_child(*label);
    }

    static void BindItemList(const std::shared_ptr<Gtk::ListItem> item){
        std::shared_ptr<EnumProxy> enumProxy = std::dynamic_pointer_cast<EnumProxy>(item->get_item());

        Gtk::Label* label = dynamic_cast<Gtk::Label*>(item->get_child());
        label->set_label(enumProxy->name);
    }
};