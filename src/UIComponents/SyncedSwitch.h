#pragma once

#include "Parameters.h"
#include "gtkmm/enums.h"
#include "gtkmm/switch.h"
#include "SettingsStore.h"
#include <memory>

class SyncedSwitch : public Gtk::Switch {
public:
    SyncedSwitch(Dasher::Parameter parameter, std::shared_ptr<Dasher::CSettingsStore> settings) : m_settings(settings), m_synced_parameter(parameter) {
        set_halign(Gtk::Align::CENTER);

        // Switch state on settings change
        settings->OnParameterChanged.Subscribe(this, [this](Dasher::Parameter param){
            //Does not need exception for loop, as Dasher::SettingsStore capture a "non-state change"
            if(param == m_synced_parameter) set_active(m_settings->GetBoolParameter(m_synced_parameter));
        });

        // Set inital state
        set_active(m_settings->GetBoolParameter(m_synced_parameter));

        // Switch on 
        property_active().signal_changed().connect([this](){
            m_settings->SetBoolParameter(m_synced_parameter, get_active());
        });
    }

protected:
    std::shared_ptr<Dasher::CSettingsStore> m_settings;
    const Dasher::Parameter m_synced_parameter;
};