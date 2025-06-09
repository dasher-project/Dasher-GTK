#include "SettingsMode.h"
#include "SettingsPageBase.h"
#include "gtkmm/stringobject.h"
#include <memory>


#pragma clang optimize off
SettingsMode::SettingsMode(std::shared_ptr<Dasher::CSettingsStore> settings, std::shared_ptr<DasherController> controller) : m_settings(settings), SettingsPageBase("Mode", "Input Mode", controller){
    append(m_device_label);
    m_device_label.set_child(m_device_switch);
    FillDropDown(m_controller, m_device_switch, Dasher::Parameter::SP_INPUT_DEVICE);
    m_device_switch.property_selected_item().signal_changed().connect([&](){
        Glib::ustring s = std::dynamic_pointer_cast<Gtk::StringObject>(m_device_switch.get_selected_item())->get_string();
        m_controller->SetStringParameter(Dasher::Parameter::SP_INPUT_DEVICE, s);
    });

    append(m_method_label);
    m_method_label.set_child(m_method_switch);
    FillDropDown(m_controller, m_method_switch, Dasher::Parameter::SP_INPUT_FILTER);
    m_method_switch.property_selected_item().signal_changed().connect([this](){
        Glib::ustring s = std::dynamic_pointer_cast<Gtk::StringObject>(m_method_switch.get_selected_item())->get_string();
        m_controller->SetStringParameter(Dasher::Parameter::SP_INPUT_FILTER, s);
        FillModuleSettingsGrid(m_method_specific_settings_grid, m_controller->GetModuleManager()->GetInputMethodByName(s), m_settings, true);
    });

    //Initial Filling of Optionspane
    FillModuleSettingsGrid(m_method_specific_settings_grid, m_controller->GetModuleManager()->GetInputMethodByName(m_controller->GetStringParameter(Dasher::Parameter::SP_INPUT_FILTER)), m_settings, true);

    append(m_method_specific);
    m_method_specific.set_child(m_method_specific_settings_grid);
}

#pragma clang optimize on