#include "SettingsMode.h"
#include "ModuleSettings.h"
#include "SettingsPageBase.h"
#include "gtkmm/centerbox.h"
#include "gtkmm/object.h"
#include "gtkmm/label.h"
#include "gtkmm/stringobject.h"
#include "gtkmm/switch.h"
#include <memory>


#pragma clang optimize off
SettingsMode::SettingsMode(std::shared_ptr<DasherController> c) : SettingsPageBase("Mode", "Input Mode", c){
    append(m_device_label);
    m_device_label.set_child(m_device_switch);
    FillDropDown(c, m_device_switch, Dasher::Parameter::SP_INPUT_DEVICE);
    m_device_switch.property_selected_item().signal_changed().connect([&](){
        Glib::ustring s = std::dynamic_pointer_cast<Gtk::StringObject>(m_device_switch.get_selected_item())->get_string();
        controller->SetStringParameter(Dasher::Parameter::SP_INPUT_DEVICE, s);
    });

    append(m_method_label);
    m_method_label.set_child(m_method_switch);
    FillDropDown(c, m_method_switch, Dasher::Parameter::SP_INPUT_FILTER);
    m_method_switch.property_selected_item().signal_changed().connect([this](){
        Glib::ustring s = std::dynamic_pointer_cast<Gtk::StringObject>(m_method_switch.get_selected_item())->get_string();
        controller->SetStringParameter(Dasher::Parameter::SP_INPUT_FILTER, s);

        const std::vector<Dasher::Settings::ModuleSetting>& settings = controller->GetModuleManager()->GetInputMethodByName(s)->getUISettings();
        for(auto& s : settings){
            switch(s.Type){
                case Switch:
                {
                    // const Dasher::Settings::SwitchSetting set = static_cast<const Dasher::Settings::SwitchSetting&>(s);
                    Gtk::CenterBox* row = Gtk::make_managed<Gtk::CenterBox>();
                    Gtk::Label* label = Gtk::make_managed<Gtk::Label>(s.Description);
                    Gtk::Switch* sw = Gtk::make_managed<Gtk::Switch>();
                    sw->set_active(controller->GetBoolParameter(s.Param));
                    sw->property_active().signal_changed().connect([s, sw, this](){
                        controller->SetBoolParameter(s.Param, sw->get_active());
                    });
                    row->set_start_widget(*label);
                    row->set_end_widget(*sw);
                    m_method_specific_box.append(*row);
                    break;
                }
                case TextField: break;
                case Slider: break;
                case Enum: break;
                case Step: break;
            }
        }
    });

    append(m_method_specific);
    m_method_specific.set_child(m_method_specific_box);

}
#pragma clang optimize on