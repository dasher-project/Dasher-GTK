#pragma once

#include "Parameters.h"
#include "gtkmm/entry.h"
#include "SettingsStore.h"
#include <memory>

#pragma clang optimize off
class SyncedTextbox : public Gtk::Entry {
public:
    SyncedTextbox(Dasher::Parameter parameter, std::shared_ptr<Dasher::CSettingsStore> settings) : m_settings(settings), m_synced_parameter(parameter) {

        // Change String on settings change
        settings->OnParameterChanged.Subscribe(this, [this](Dasher::Parameter param){
            //Does not need exception for loop, as Dasher::SettingsStore capture a "non-state change"
            if(param == m_synced_parameter) set_text(m_settings->GetStringParameter(m_synced_parameter));
        });

        // Set inital string
        set_text(m_settings->GetStringParameter(m_synced_parameter));

        // Change value on user input
        signal_activate().connect([this](){
            m_settings->SetStringParameter(m_synced_parameter, get_text());
        }, false);
    }

protected:
    std::shared_ptr<Dasher::CSettingsStore> m_settings;
    const Dasher::Parameter m_synced_parameter;
};
#pragma clang optimize on