#pragma once

#include "gtkmm/dropdown.h"
#include "gtkmm/stringlist.h"
#include "SettingsStore.h"
#include "gtkmm/stringobject.h"
#include <vector>


class SyncedStringDropdown : public Gtk::DropDown {
public:
    SyncedStringDropdown(Dasher::Parameter parameter, std::shared_ptr<Dasher::CSettingsStore> settings, std::vector<std::string> values) : m_settings(settings), m_synced_parameter(parameter) {
        possible_values = std::vector<Glib::ustring>(values.begin(), values.end());
        set_model(Gtk::StringList::create(possible_values));

        // Adjust state on settings change
        settings->OnParameterChanged.Subscribe(this, [this](Dasher::Parameter param){
            //Does not need exception for loop, as Dasher::SettingsStore capture a "non-state change"
            if(param == m_synced_parameter) SetSelected(m_settings->GetStringParameter(m_synced_parameter));
        });

        // Set inital state
        SetSelected(m_settings->GetStringParameter(m_synced_parameter));

        // Adjust based on movement
        property_selected_item().signal_changed().connect([this](){
            m_settings->SetStringParameter(m_synced_parameter, GetSelected());
        });
    }

    void SetSelected(Glib::ustring item){
        set_selected(std::find(possible_values.begin(), possible_values.end(), item) - possible_values.begin());
    }

    Glib::ustring GetSelected(){
        return std::dynamic_pointer_cast<Gtk::StringObject>(get_selected_item())->get_string();
    }

protected:
    std::shared_ptr<Dasher::CSettingsStore> m_settings;
    const Dasher::Parameter m_synced_parameter;
    std::vector<Glib::ustring> possible_values;
};






// std::vector<std::string> Alphabets;
//     ;
//     
//     
//     std::string selected_alphabet = m_canvas.dasherController->GetStringParameter(Dasher::Parameter::SP_ALPHABET_ID);
//     m_alphabet_chooser.set_selected(std::find(AlphaUString.begin(), AlphaUString.end(), Glib::ustring(selected_alphabet)) - AlphaUString.begin());
//     m_alphabet_chooser.property_selected_item().signal_changed().connect([this](){
//         Glib::ustring s = std::dynamic_pointer_cast<Gtk::StringObject>(m_alphabet_chooser.get_selected_item())->get_string();
//         m_canvas.dasherController->SetStringParameter(Dasher::Parameter::SP_ALPHABET_ID, s);
//     });