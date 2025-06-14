#pragma once

#include "../DasherController.h"
#include "Parameters.h"
#include "glibmm/ustring.h"
#include "gtkmm/box.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/stringlist.h"
#include "gtkmm/enums.h"
#include "gtkmm/grid.h"
#include "gtkmm/label.h"
#include <algorithm>
#include <memory>
#include "UIComponents/SyncedEnumDropdown.h"
#include "UIComponents/SyncedSlider.h"
#include "UIComponents/SyncedSpinButton.h"
#include "UIComponents/SyncedTextbox.h"
#include "UIComponents/SyncedSwitch.h"
#include "UIComponents/PopoverMenuButtonInfo.h"

class SettingsPageBase : public Gtk::Box
{

protected:
    SettingsPageBase(Glib::ustring name, Glib::ustring title, std::shared_ptr<DasherController> controller = nullptr) :
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
        std::vector<std::string> input_method = controller->GetPermittedValues(param);

        //Copy list as elements
        std::vector<Glib::ustring> method_list(input_method.begin(), input_method.end());
        dropdown.set_model(Gtk::StringList::create(method_list));
        
        //Set selected element
        std::string selected = controller->GetStringParameter(Dasher::Parameter::SP_INPUT_FILTER);
        dropdown.set_selected(std::find(method_list.begin(), method_list.end(), Glib::ustring(selected)) - method_list.begin());
    }

    static void FillModuleSettingsGrid(Gtk::Grid& gridWidget, CDasherModule* Module, std::shared_ptr<Dasher::CSettingsStore>& DasherSettings, bool includeAdvancedSettings){       
        //Clear the grid
        Widget* iter = gridWidget.get_first_child();
        while(iter) {
            Widget* next = iter->get_next_sibling();
            gridWidget.remove(*iter);
            iter = next;
        }
        
        std::vector<Dasher::Parameter> UISettings;
        Module->GetUISettings(UISettings);

        //Sort Settingslist, comparator needed to reference default parameter list
        std::sort(UISettings.begin(), UISettings.end(), [](Dasher::Parameter& a, Dasher::Parameter&b) {return Dasher::Settings::parameter_defaults.at(a) < Dasher::Settings::parameter_defaults.at(b);});
        
        for(unsigned int i = 0; i < UISettings.size(); i++){
            const Dasher::Settings::Parameter_Value& setting = Dasher::Settings::parameter_defaults.at(UISettings[i]);
            if(!includeAdvancedSettings && setting.advancedSetting) continue; //Potentially skip advanced settings

            // Attach Name Label
            Gtk::Label* nameLabel = Gtk::make_managed<Gtk::Label>(setting.humanName != "" ? setting.humanName : setting.storageName);
            nameLabel->set_halign(Gtk::Align::START);
            gridWidget.attach(*nameLabel, 0, i);

            // Attach Interactable Widget
            switch(setting.suggestedUI){
                case Dasher::Settings::UIControlType::Switch:
                {
                    gridWidget.attach(*Gtk::make_managed<SyncedSwitch>(UISettings[i], DasherSettings), 1, i);
                    break;
                }
                case Dasher::Settings::UIControlType::TextField: {
                    gridWidget.attach(*Gtk::make_managed<SyncedTextbox>(UISettings[i], DasherSettings), 1, i);
                    break;
                }
                case Dasher::Settings::UIControlType::Slider: {
                    gridWidget.attach(*Gtk::make_managed<SyncedSlider>(UISettings[i], DasherSettings, setting.min, setting.max, setting.step), 1, i);
                    break;
                }
                case Dasher::Settings::UIControlType::Step: {
                    gridWidget.attach(*Gtk::make_managed<SyncedSpinButton>(UISettings[i], DasherSettings, setting.min, setting.max, setting.step), 1, i);
                    break;
                }
                case Dasher::Settings::UIControlType::Enum: {
                    gridWidget.attach(*Gtk::make_managed<SyncedEnumDropdown>(UISettings[i], DasherSettings, setting.possibleValues), 1, i);
                    break;
                }
                case Dasher::Settings::None: break;
                }

            gridWidget.attach(*Gtk::make_managed<PopoverMenuButtonInfo>(setting.humanDescription), 2, i);
        }
    }
};