#include "SettingsInputMapping.h"
#include "DasherTypes.h"
#include "gtkmm/enums.h"
#include "gtkmm/singleselection.h"

#pragma clang optimize off
SettingsInputMapping::SettingsInputMapping(std::shared_ptr<ButtonMapper> buttonMapperInput):
    buttonMapper(buttonMapperInput),
    SettingsPageBase("InputMapping", "Input Mapping")
{
    append(scolledView);
    scolledView.set_child(columnView);
    scolledView.set_hexpand(true);
    scolledView.set_vexpand(true);
    scolledView.set_halign(Gtk::Align::FILL);
    scolledView.set_valign(Gtk::Align::FILL);

    InitColumView();
   
    append(lowerBar);
    lowerBar.append(virtualKeyListDropdown);
    lowerBar.append(deviceKeyEntry);
    lowerBar.append(deviceKeyListenButton);
    lowerBar.append(entryAddButton);
    lowerBar.append(entryRemoveButton);

    // Setup Dropdown
    virtualKeyListDropdown.set_factory(virtualKeyListItemFactory);
    virtualKeyListItemFactory->signal_setup().connect([](const std::shared_ptr<Gtk::ListItem> item){
        Gtk::Label* label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        item->set_child(*label);
    });
    virtualKeyListItemFactory->signal_bind().connect([](const std::shared_ptr<Gtk::ListItem> item){
        std::shared_ptr<VirtualKeyProxy> mapping = std::dynamic_pointer_cast<VirtualKeyProxy>(item->get_item());
        Gtk::Label* label = dynamic_cast<Gtk::Label*>(item->get_child());
        label->set_label(Dasher::Keys::VirtualKeyToString(mapping->dasherKey));
    });
    virtualKeyListDropdown.set_model(virtualKeyList);

    //Adding virtual keys in specific order
    virtualKeyList->append(VirtualKeyProxy::create(Dasher::Keys::Big_Start_Stop_Key));
    virtualKeyList->append(VirtualKeyProxy::create(Dasher::Keys::Primary_Input));
    virtualKeyList->append(VirtualKeyProxy::create(Dasher::Keys::Secondary_Input));
    virtualKeyList->append(VirtualKeyProxy::create(Dasher::Keys::Tertiary_Input));
    for(int k = Dasher::Keys::Button_1; k < Dasher::Keys::Primary_Input; k++){
        virtualKeyList->append(VirtualKeyProxy::create(static_cast<Dasher::Keys::VirtualKey>(k)));
    }

    // Button functionality
    entryAddButton.signal_clicked().connect([this]() {
        std::shared_ptr<VirtualKeyProxy> selectedVirtualKey = std::dynamic_pointer_cast<VirtualKeyProxy>(virtualKeyListDropdown.get_selected_item());
        Glib::ustring deviceKey = deviceKeyEntry.get_buffer()->get_text();
        if(selectedVirtualKey->dasherKey == Dasher::Keys::Invalid_Key || deviceKey.empty()) return; // Do not add invalid mappings
        buttonMapper->AddKeyToButtonMap(selectedVirtualKey->dasherKey, deviceKey);
    });
    entryRemoveButton.signal_clicked().connect([this]() {
        std::shared_ptr<InputMapping> mapping = std::dynamic_pointer_cast<InputMapping>(selection_model->get_selected_item());
        if(mapping.get() != nullptr) buttonMapper->RemoveKeyFromButtonMap(mapping->dasherKey, mapping->deviceKey);
    });
    deviceKeyListenButton.signal_clicked().connect([this]() {
        buttonMapper->ListenForAnyKey(deviceKeyListenButton.get_active());
    });
    buttonMapper->AnyKeyPressed.Subscribe(this, [this](std::string deviceKey){
        deviceKeyEntry.get_buffer()->set_text(deviceKey);
        deviceKeyListenButton.set_active(false);
    });
}

void SettingsInputMapping::InitColumView(){
    columnView.set_hexpand(true);
    columnView.set_vexpand(true);
    columnView.set_halign(Gtk::Align::FILL);
    columnView.set_valign(Gtk::Align::FILL);

    //Init values
    for(auto& [virtualKey, deviceKey] : buttonMapper->GetKeyMap()){
        valueList->append(InputMapping::create(virtualKey, deviceKey));
    }

    buttonMapper->ButtonMappingsChanged.Subscribe(this, [this](){
        valueList->remove_all();
        for(auto& [virtualKey, deviceKey] : buttonMapper->GetKeyMap()){
            valueList->append(InputMapping::create(virtualKey, deviceKey));
        }
    });

    selection_model->set_autoselect(false);
    selection_model->set_can_unselect(true);
    columnView.set_model(selection_model);
    
    itemFactoryVirtualKey->signal_setup().connect([](const std::shared_ptr<Gtk::ListItem> item){
        Gtk::Label* label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        item->set_child(*label);
    });
    itemFactoryVirtualKey->signal_bind().connect([](const std::shared_ptr<Gtk::ListItem> item){
        std::shared_ptr<InputMapping> mapping = std::dynamic_pointer_cast<InputMapping>(item->get_item());
        Gtk::Label* label = dynamic_cast<Gtk::Label*>(item->get_child());
        label->set_label(Dasher::Keys::VirtualKeyToString(mapping->dasherKey));
    });
    columnView.append_column(Gtk::ColumnViewColumn::create("Dasher Key", itemFactoryVirtualKey));

    itemFactoryDeviceKey->signal_setup().connect([](const std::shared_ptr<Gtk::ListItem> item){
        Gtk::Label* label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        item->set_child(*label);
    });
    itemFactoryDeviceKey->signal_bind().connect([](const std::shared_ptr<Gtk::ListItem> item){
        std::shared_ptr<InputMapping> mapping = std::dynamic_pointer_cast<InputMapping>(item->get_item());
        Gtk::Label* label = dynamic_cast<Gtk::Label*>(item->get_child());
        label->set_label(mapping->deviceKey);
    });
    columnView.append_column(Gtk::ColumnViewColumn::create("Device Key", itemFactoryDeviceKey));
}
#pragma clang optimize on