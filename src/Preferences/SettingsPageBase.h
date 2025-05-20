#pragma once

#include "../DasherController.h"
#include "glibmm/ustring.h"
#include "gtkmm/box.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/stringlist.h"
#include "gtkmm/enums.h"
#include "gtkmm/widget.h"
#include <memory>

class SettingsPageBase : public Gtk::Box
{

protected:
    SettingsPageBase(Glib::ustring name, Glib::ustring title, std::shared_ptr<DasherController> controller) :
        Gtk::Box(Gtk::Orientation::VERTICAL),
        name(name), title(title), controller(controller)
    {}

    Glib::ustring name;
    Glib::ustring title;
    std::shared_ptr<DasherController> controller;

public:
    Glib::ustring getSettingsName() {return name;}
    Glib::ustring getSettingsTitle() {return title;}
    std::shared_ptr<DasherController> Dasher() {return controller;}

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

};