#pragma once

#include "Engine/DasherBridge.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/stringlist.h"
#include <vector>
#include <memory>

class SyncedStringDropdown : public Gtk::DropDown {
public:
    SyncedStringDropdown(int parameter_key, std::shared_ptr<DasherBridge> bridge, const std::vector<std::string>& values)
        : m_bridge(bridge), m_key(parameter_key)
    {
        possible_values = std::vector<Glib::ustring>(values.begin(), values.end());
        set_model(Gtk::StringList::create(possible_values));
        set_selected(find_index(m_bridge->get_string_parameter(m_key)));

        property_selected_item().signal_changed().connect([this]() {
            Glib::ustring sel = get_selected_string();
            m_bridge->set_string_parameter(m_key, sel);
            OnSelectionChanged.emit(sel);
        });
    }

    Glib::ustring get_selected_string() {
        return std::dynamic_pointer_cast<Gtk::StringObject>(get_selected_item())->get_string();
    }

    void update_from_bridge() {
        set_selected(find_index(m_bridge->get_string_parameter(m_key)));
    }

    sigc::signal<void(Glib::ustring)> OnSelectionChanged;

protected:
    std::shared_ptr<DasherBridge> m_bridge;
    int m_key;
    std::vector<Glib::ustring> possible_values;

    guint find_index(const std::string& value) {
        auto it = std::find(possible_values.begin(), possible_values.end(), Glib::ustring(value));
        return static_cast<guint>(it - possible_values.begin());
    }
};
