#pragma once

#include "DasherTypes.h"
#include "SettingsPageBase.h"
#include "gtkmm/button.h"
#include "gtkmm/togglebutton.h"
#include "gtkmm/columnview.h"
#include "gtkmm/singleselection.h"
#include "gtkmm/entry.h"
#include "gtkmm/scrolledwindow.h"

class InputMapping : public Glib::Object
{
public:
    Dasher::Keys::VirtualKey dasherKey;
    std::string deviceKey;

    static Glib::RefPtr<InputMapping> create(Dasher::Keys::VirtualKey dasherKey, std::string deviceKey)
    {
        return Glib::make_refptr_for_instance<InputMapping>(new InputMapping(dasherKey, deviceKey));
    }

protected:
    InputMapping(Dasher::Keys::VirtualKey dasherKey, std::string deviceKey) : dasherKey(dasherKey), deviceKey(deviceKey){}
};

class VirtualKeyProxy : public Glib::Object
{
public:
    Dasher::Keys::VirtualKey dasherKey = Dasher::Keys::Invalid_Key;
    static Glib::RefPtr<VirtualKeyProxy> create(Dasher::Keys::VirtualKey dasherKey)
    {
        return Glib::make_refptr_for_instance<VirtualKeyProxy>(new VirtualKeyProxy(dasherKey));
    }

protected:
    VirtualKeyProxy(Dasher::Keys::VirtualKey dasherKey) : dasherKey(dasherKey){}
};

class SettingsInputMapping : public SettingsPageBase
{
public:
    SettingsInputMapping(std::shared_ptr<ButtonMapper> buttonMapper);

protected:
    void InitColumView();

    std::shared_ptr<ButtonMapper> buttonMapper;
    // ColumnView Stuff
    Glib::RefPtr<Gio::ListStore<InputMapping>> valueList = Gio::ListStore<InputMapping>::create();
    Glib::RefPtr<Gtk::SingleSelection> selection_model = Gtk::SingleSelection::create(valueList);
    Glib::RefPtr<Gtk::SignalListItemFactory> itemFactoryVirtualKey = Gtk::SignalListItemFactory::create();
    Glib::RefPtr<Gtk::SignalListItemFactory> itemFactoryDeviceKey = Gtk::SignalListItemFactory::create();

    Gtk::ScrolledWindow scolledView;
    Gtk::ColumnView columnView;
    Gtk::Box lowerBar;
    Gtk::DropDown virtualKeyListDropdown;
    Gtk::Entry deviceKeyEntry;
    Gtk::ToggleButton deviceKeyListenButton = Gtk::ToggleButton("Record");
    Gtk::Button entryAddButton = Gtk::Button("Add");
    Gtk::Button entryRemoveButton = Gtk::Button("Remove");

    //EnumDropDown Stuff
    Glib::RefPtr<Gio::ListStore<VirtualKeyProxy>> virtualKeyList = Gio::ListStore<VirtualKeyProxy>::create();
    Glib::RefPtr<Gtk::SignalListItemFactory> virtualKeyListItemFactory = Gtk::SignalListItemFactory::create();
}; 