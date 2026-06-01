#pragma once

#include "Engine/DasherBridge.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/label.h"
#include "gtkmm/signallistitemfactory.h"
#include "giomm/liststore.h"
#include <memory>
#include <map>

class EnumProxy : public Glib::Object {
public:
    Glib::ustring name;
    int value;
    static Glib::RefPtr<EnumProxy> create(const Glib::ustring& n, int v) {
        return Glib::make_refptr_for_instance<EnumProxy>(new EnumProxy(n, v));
    }
protected:
    EnumProxy(const Glib::ustring& n, int v) : name(n), value(v) {}
};

class SyncedEnumDropdown : public Gtk::DropDown {
public:
    SyncedEnumDropdown(int parameter_key, std::shared_ptr<DasherBridge> bridge)
        : m_bridge(bridge), m_key(parameter_key)
    {
        int count = m_bridge->get_parameter_enum_count(m_key);
        for (int i = 0; i < count; i++) {
            std::string name = m_bridge->get_parameter_enum_name(m_key, i);
            int value = m_bridge->get_parameter_enum_value(m_key, i);
            value_list->append(EnumProxy::create(name, value));
        }

        set_model(value_list);
        set_factory(item_factory);

        item_factory->signal_setup().connect([](const std::shared_ptr<Gtk::ListItem> item) {
            auto* label = Gtk::make_managed<Gtk::Label>("");
            label->set_halign(Gtk::Align::START);
            item->set_child(*label);
        });
        item_factory->signal_bind().connect([](const std::shared_ptr<Gtk::ListItem> item) {
            auto proxy = std::dynamic_pointer_cast<EnumProxy>(item->get_item());
            auto* label = dynamic_cast<Gtk::Label*>(item->get_child());
            label->set_label(proxy->name);
        });

        set_selected(find_value(static_cast<int>(m_bridge->get_long_parameter(m_key))));

        property_selected().signal_changed().connect([this]() {
            auto proxy = std::dynamic_pointer_cast<EnumProxy>(get_selected_item());
            if (proxy) {
                m_bridge->set_long_parameter(m_key, proxy->value);
            }
        });
    }

protected:
    std::shared_ptr<DasherBridge> m_bridge;
    int m_key;
    Glib::RefPtr<Gio::ListStore<EnumProxy>> value_list = Gio::ListStore<EnumProxy>::create();
    Glib::RefPtr<Gtk::SignalListItemFactory> item_factory = Gtk::SignalListItemFactory::create();

    guint find_value(int val) {
        for (guint i = 0; i < value_list->get_n_items(); i++) {
            if (value_list->get_item(i)->value == val) return i;
        }
        return 0;
    }
};
