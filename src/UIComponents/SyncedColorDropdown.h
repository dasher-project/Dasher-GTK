#pragma once

#include "ColorPalette.h"
#include "ColorDisplayWidget.h"
#include "Parameters.h"
#include "SettingsStore.h"
#include "giomm/liststore.h"
#include "gtkmm/box.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/label.h"
#include "gtkmm/signallistitemfactory.h"
#include <string>

class PaletteProxy : public Glib::Object
{
    public:
        PaletteProxy(const Dasher::ColorPalette* p): p(p){}
        const Dasher::ColorPalette* p;
        static Glib::RefPtr<PaletteProxy> create(const Dasher::ColorPalette* p)
        {
            return Glib::make_refptr_for_instance<PaletteProxy>(new PaletteProxy(p));
        }
};

class SyncedColorDropdown : public Gtk::DropDown
{
public:
    Glib::RefPtr<Gio::ListStore<PaletteProxy>> colorPaletteList = Gio::ListStore<PaletteProxy>::create();
    Glib::RefPtr<Gtk::SignalListItemFactory> color_factory_header = Gtk::SignalListItemFactory::create();
    Glib::RefPtr<Gtk::SignalListItemFactory> color_factory = Gtk::SignalListItemFactory::create();

    SyncedColorDropdown(std::shared_ptr<Dasher::CSettingsStore> settings, const std::map<std::string, Dasher::ColorPalette*>* palettes){
        // Add Values
        for (const auto &[key, value] : *palettes) {
            colorPaletteList->append(PaletteProxy::create(value));
        }
        
        set_list_factory(color_factory);
        set_factory(color_factory_header);
        set_model(colorPaletteList);
        
        color_factory_header->signal_setup().connect(&SyncedColorDropdown::SetupItemHeader);
        color_factory_header->signal_bind().connect(&SyncedColorDropdown::BindItemHeader);

        color_factory->signal_setup().connect(&SyncedColorDropdown::SetupItemList);
        color_factory->signal_bind().connect(&SyncedColorDropdown::BindItemList);

        // Switch selected value on settings change
        settings->OnParameterChanged.Subscribe(this, [this, settings](Dasher::Parameter param){
            //Does not need exception for loop, as Dasher::SettingsStore capture a "non-state change"
            if(param == Dasher::Parameter::SP_COLOUR_ID) SelectPalette(settings->GetStringParameter(Dasher::Parameter::SP_COLOUR_ID));
        });

        // Set inital state
        SelectPalette(settings->GetStringParameter(Dasher::Parameter::SP_COLOUR_ID));

        // Switch value on user interaction 
        property_selected().signal_changed().connect([this, settings](){
            settings->SetStringParameter(Dasher::Parameter::SP_COLOUR_ID, GetSelectedPalette());
        });
    }

    std::string GetSelectedPalette(){
        return std::dynamic_pointer_cast<PaletteProxy>(get_selected_item())->p->PaletteName;
    }

    bool SelectPalette(const std::string& selected_color){
        for(guint i = 0; i < colorPaletteList->get_n_items(); i++){
            if(colorPaletteList->get_item(i)->p->PaletteName.compare(selected_color) == 0){
                set_selected(i);
                return true;
            }
        }
        return false;
    }

protected:
    // Functions for Header Display (without Palettename)
    static void SetupItemHeader(const std::shared_ptr<Gtk::ListItem> item){
        item->set_child(*Gtk::make_managed<ColorDisplayWidget>());
    }
    static void BindItemHeader(const std::shared_ptr<Gtk::ListItem> item){
        std::shared_ptr<PaletteProxy> palette = std::dynamic_pointer_cast<PaletteProxy>(item->get_item());
        ColorDisplayWidget* widget = dynamic_cast<ColorDisplayWidget*>(item->get_child());
        widget->ReadColorsFromPalette(palette->p);
    }

    // Functions for List Items (with Palettename)
    static void SetupItemList(const std::shared_ptr<Gtk::ListItem> item){
        ColorDisplayWidget* widget = Gtk::make_managed<ColorDisplayWidget>();
        Gtk::Box* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
        Gtk::Label* label = Gtk::make_managed<Gtk::Label>("");
        box->append(*widget);
        box->append(*label);
        item->set_child(*box);
    }
    static void BindItemList(const std::shared_ptr<Gtk::ListItem> item){
        std::shared_ptr<PaletteProxy> palette = std::dynamic_pointer_cast<PaletteProxy>(item->get_item());
        Gtk::Box* box = dynamic_cast<Gtk::Box*>(item->get_child());
        ColorDisplayWidget* widget = dynamic_cast<ColorDisplayWidget*>(box->get_first_child());
        Gtk::Label* label = dynamic_cast<Gtk::Label*>(box->get_last_child());
        widget->ReadColorsFromPalette(palette->p);
        label->set_label(palette->p->PaletteName);
    }
};