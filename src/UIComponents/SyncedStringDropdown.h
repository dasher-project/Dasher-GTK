#pragma once

#include "Engine/DasherBridge.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/stringlist.h"
#include "gtkmm/stringobject.h"
#include <vector>
#include <memory>

class SyncedStringDropdown : public Gtk::DropDown {
public:
    SyncedStringDropdown(int parameter_key, std::shared_ptr<DasherBridge> bridge, const std::vector<std::string>& values)
        : m_bridge(bridge), m_key(parameter_key)
    {
        set_possible_values(values);
        set_selected(find_index(m_bridge->get_string_parameter(m_key)));

        property_selected_item().signal_changed().connect([this]() {
            auto item = std::dynamic_pointer_cast<Gtk::StringObject>(get_selected_item());
            if (!item) return; // model just replaced; nothing selected yet
            // During update_from_bridge() the model swap transiently selects
            // index 0; writing that to the engine would clobber the current
            // (or reset-default) value with the first list entry.
            if (m_refreshing) return;
            Glib::ustring sel = item->get_string();
            m_bridge->set_string_parameter(m_key, sel);
            OnSelectionChanged.emit(sel);
        });
    }

    Glib::ustring get_selected_string() {
        return std::dynamic_pointer_cast<Gtk::StringObject>(get_selected_item())->get_string();
    }

    // Rebuild the model (e.g. once the engine has realised and the permitted
    // values exist — the constructor may run before that, when the engine
    // returns an empty list) and re-select the engine's current value.
    void update_from_bridge() {
        m_refreshing = true;
        set_possible_values(m_bridge->get_parameter_string_values(m_key));
        set_selected(find_index(m_bridge->get_string_parameter(m_key)));
        m_refreshing = false;
    }

    sigc::signal<void(Glib::ustring)> OnSelectionChanged;

protected:
  void set_possible_values(const std::vector<std::string>& values) {
      possible_values = std::vector<Glib::ustring>(values.begin(), values.end());
      m_model = Gtk::StringList::create(possible_values);
      set_model(m_model);
  }

    std::shared_ptr<DasherBridge> m_bridge;
    int m_key;
    std::vector<Glib::ustring> possible_values;
    Glib::RefPtr<Gtk::StringList> m_model;
    bool m_refreshing = false; // suppress the change handler during model swaps

    guint find_index(const std::string& value) {
        auto it = std::find(possible_values.begin(), possible_values.end(), Glib::ustring(value));
        if (it == possible_values.end()) return 0; // not in list: show the first rather than an invalid index
        return static_cast<guint>(it - possible_values.begin());
    }
};
