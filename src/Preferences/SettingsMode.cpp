
#include "SettingsMode.h"
#include "SettingsPageBase.h"
#include <memory>

SettingsMode::SettingsMode(std::shared_ptr<DasherController> controller) : SettingsPageBase("Mode", "Input Mode", controller){
    append(m_device_label);
    m_device_label.set_child(m_device_switch);
    FillDropDown(controller, m_device_switch, Dasher::Parameter::SP_INPUT_DEVICE);

    append(m_method_label);
    m_method_label.set_child(m_method_switch);
    FillDropDown(controller, m_method_switch, Dasher::Parameter::SP_INPUT_FILTER);
}
