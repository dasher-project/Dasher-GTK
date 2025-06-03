#pragma once

#include "Parameters.h"
#include "SettingsStore.h"
#include "gtkmm/spinbutton.h"
#include <memory>

//Basically a copy of the SyncedSlider
class SyncedSpinButton : public Gtk::SpinButton {
public:
    SyncedSpinButton(Dasher::Parameter parameter, std::shared_ptr<Dasher::CSettingsStore> settings, int min, int max, int step) : m_settings(settings), m_synced_parameter(parameter) {
        // Set spinbutton parameters 
        set_digits(0);
        set_range(min, max);
        set_increments(step, step*5);

        // Adjust state on settings change
        settings->OnParameterChanged.Subscribe(this, [this](Dasher::Parameter param){
            //Does not need exception for loop, as Dasher::SettingsStore capture a "non-state change"
            if(param == m_synced_parameter) set_value(m_settings->GetLongParameter(m_synced_parameter));
        });

        // Set inital state
        set_value(m_settings->GetLongParameter(m_synced_parameter));

        // Adjust based on movement
        signal_value_changed().connect([this](){
            m_settings->SetLongParameter(m_synced_parameter, static_cast<long>(get_value()));
        });
    }

protected:
    std::shared_ptr<Dasher::CSettingsStore> m_settings;
    const Dasher::Parameter m_synced_parameter;
};