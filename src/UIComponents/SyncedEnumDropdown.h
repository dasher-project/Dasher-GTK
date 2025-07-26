#pragma once

#include "EnumDropdown.h"
#include "Parameters.h"
#include "giomm/liststore.h"
#include "gtkmm/signallistitemfactory.h"
#include "SettingsStore.h"
#include <memory>

class SyncedEnumDropdown : public EnumDropdown {
public:
    Glib::RefPtr<Gio::ListStore<EnumProxy>> valueList = Gio::ListStore<EnumProxy>::create();
    Glib::RefPtr<Gtk::SignalListItemFactory> itemFactory = Gtk::SignalListItemFactory::create();
    
    SyncedEnumDropdown(Dasher::Parameter parameter, std::shared_ptr<Dasher::CSettingsStore> settings, const std::map<std::string, int>& Enums) :
        m_settings(settings), m_synced_parameter(parameter), EnumDropdown(Enums, settings->GetLongParameter(parameter)) {
        
        // Switch selected value on settings change
        settings->OnParameterChanged.Subscribe(this, [this](Dasher::Parameter param){
            //Does not need exception for loop, as Dasher::SettingsStore capture a "non-state change"
            if(param == m_synced_parameter) SetSelected(m_settings->GetLongParameter(m_synced_parameter));
        });

        // Switch value on user interaction 
        property_selected().signal_changed().connect([this](){
            m_settings->SetLongParameter(m_synced_parameter, GetSelected());
        });
    }

protected:
    std::shared_ptr<Dasher::CSettingsStore> m_settings;
    const Dasher::Parameter m_synced_parameter;
};