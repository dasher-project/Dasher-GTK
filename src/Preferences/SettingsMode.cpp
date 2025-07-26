#include "SettingsMode.h"
#include "SettingsPageBase.h"
#include "DeviceSettingsProvider.h"
#include <memory>


#pragma clang optimize off
SettingsMode::SettingsMode(std::shared_ptr<Dasher::CSettingsStore> settings, std::shared_ptr<DasherController> controller) :
    m_settings(settings),
    SettingsPageBase("Mode", "Input Mode", controller),
    m_device_switch(Dasher::Parameter::SP_INPUT_DEVICE, settings, controller->GetPermittedValues(Dasher::Parameter::SP_INPUT_DEVICE)),
    m_method_switch(Dasher::Parameter::SP_INPUT_FILTER, settings, controller->GetPermittedValues(Dasher::Parameter::SP_INPUT_FILTER))
{
    append(m_device_label);
    m_device_label.set_child(m_device_switch);

    append(m_device_specific);
    m_device_specific.set_child(m_device_specific_settings_grid);

    m_device_switch.OnSelectionChanged.connect([this](Glib::ustring selectedString){DeviceSwitched(selectedString);});
    DeviceSwitched(m_device_switch.GetSelected());

    append(m_method_label);
    m_method_label.set_child(m_method_switch);

    m_method_switch.OnSelectionChanged.connect([this](Glib::ustring selectedString){
        FillModuleSettingsGrid(m_method_specific_settings_grid, m_controller->GetModuleManager()->GetInputMethodByName(selectedString), m_settings, true);
    });

    //Initial Filling of Options-Pane
    FillModuleSettingsGrid(m_method_specific_settings_grid, m_controller->GetModuleManager()->GetInputMethodByName(m_controller->GetStringParameter(Dasher::Parameter::SP_INPUT_FILTER)), m_settings, true);

    append(m_method_specific);
    m_method_specific.set_child(m_method_specific_settings_grid);
}

void SettingsMode::DeviceSwitched(Glib::ustring selectedString){
    DeviceSettingsProvider* input = dynamic_cast<DeviceSettingsProvider*>(m_controller->GetModuleManager()->GetInputDeviceByName(selectedString));
    bool settingsVisible = false;
    if(input){
        ClearGrid(m_device_specific_settings_grid);
        settingsVisible = input->FillInputDeviceSettings(&m_device_specific_settings_grid);
    }
    m_device_specific.set_visible(settingsVisible);
}

#pragma clang optimize on