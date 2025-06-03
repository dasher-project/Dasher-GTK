#pragma once

#include "../DasherController.h"
#include "glibmm/ustring.h"
#include "gtkmm/box.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/stringlist.h"
#include "gtkmm/enums.h"
#include "gtkmm/grid.h"
#include "gtkmm/label.h"
#include <memory>
#include "UIComponents/SyncedEnum.h"
#include "UIComponents/SyncedSlider.h"
#include "UIComponents/SyncedSpinButton.h"
#include "UIComponents/SyncedTextbox.h"
#include "UIComponents/SyncedSwitch.h"

class SettingsPageBase : public Gtk::Box
{

protected:
    SettingsPageBase(Glib::ustring name, Glib::ustring title, std::shared_ptr<DasherController> controller) :
        Gtk::Box(Gtk::Orientation::VERTICAL),
        m_settings_page_name(name), m_settings_page_title(title), m_controller(controller)
    {}

    Glib::ustring m_settings_page_name;
    Glib::ustring m_settings_page_title;
    std::shared_ptr<DasherController> m_controller;

public:
    Glib::ustring getSettingsName() {return m_settings_page_name;}
    Glib::ustring getSettingsTitle() {return m_settings_page_title;}
    std::shared_ptr<DasherController> Dasher() {return m_controller;}

    static void FillDropDown(std::shared_ptr<DasherController> controller, Gtk::DropDown& dropdown, Dasher::Parameter param){
        //Get list from Dasher
        std::vector<std::string> input_method;
        controller->GetPermittedValues(param, input_method);

        //Copy list as elements
        std::vector<Glib::ustring> method_list(input_method.begin(), input_method.end());
        dropdown.set_model(Gtk::StringList::create(method_list));
        
        //Set selected element
        std::string selected = controller->GetStringParameter(Dasher::Parameter::SP_INPUT_FILTER);
        dropdown.set_selected(std::find(method_list.begin(), method_list.end(), Glib::ustring(selected)) - method_list.begin());
    }

    static void FillModuleSettingsGrid(Gtk::Grid& gridWidget, CDasherModule* Module, std::shared_ptr<Dasher::CSettingsStore>& DasherSettings){       
        //Clear the grid
        Widget* iter = gridWidget.get_first_child();
        while(iter) {
            Widget* next = iter->get_next_sibling();
            gridWidget.remove(*iter);
            iter = next;
        }
        
        CDasherModule::UISettingList UISettings;
        Module->GetUISettings(UISettings);
        for(unsigned int i = 0; i < UISettings.size(); i++){
            // Attach Name Label
            Gtk::Label* nameLabel = Gtk::make_managed<Gtk::Label>(UISettings[i]->Name);
            nameLabel->set_halign(Gtk::Align::START);
            gridWidget.attach(*nameLabel, 0, i);

            // Attach Interactable Widget
            switch(UISettings[i]->Type){
                case Switch:
                {
                    gridWidget.attach(*Gtk::make_managed<SyncedSwitch>(UISettings[i]->Param, DasherSettings), 1, i);
                    break;
                }
                case TextField: {
                    gridWidget.attach(*Gtk::make_managed<SyncedTextbox>(UISettings[i]->Param, DasherSettings), 1, i);
                    break;
                }
                case Slider: {
                    Dasher::Settings::SliderSetting* setting = static_cast<Dasher::Settings::SliderSetting*>(UISettings[i].get());
                    gridWidget.attach(*Gtk::make_managed<SyncedSlider>(setting->Param, DasherSettings, setting->min, setting->max, setting->step), 1, i);
                    break;
                }
                case Step: {
                    Dasher::Settings::SpinSetting* setting = static_cast<Dasher::Settings::SpinSetting*>(UISettings[i].get());
                    gridWidget.attach(*Gtk::make_managed<SyncedSpinButton>(setting->Param, DasherSettings, setting->min, setting->max, setting->step), 1, i);
                    break;
                }
                case Enum: {
                    Dasher::Settings::EnumSetting* setting = static_cast<Dasher::Settings::EnumSetting*>(UISettings[i].get());
                    gridWidget.attach(*Gtk::make_managed<SyncedEnum>(setting->Param, DasherSettings, setting->Enums), 1, i);
                    break;
                }
            }
        }
    }
};