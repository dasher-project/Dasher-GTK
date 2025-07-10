#include "PreferencesWindow.h"
#include "SettingsMode.h"
#include "SettingsStore.h"
#include "gtkmm/eventcontrollerkey.h"

PreferencesWindow::PreferencesWindow(std::shared_ptr<Dasher::CSettingsStore> settings, std::shared_ptr<DasherController> controller): m_controller(controller), m_settings(settings), m_mode_page(settings, controller), m_input_page(m_controller->GetButtonMapper()) {
    set_child(m_sideways_box);
    set_title("Dasher Preferences");

    m_sideways_box.append(m_sidebar);
    m_sideways_box.append(m_settings_stack);
    m_sidebar.set_stack(m_settings_stack);

    //Add Pages
    m_settings_stack.add(m_mode_page, m_mode_page.getSettingsName(), m_mode_page.getSettingsTitle());
    m_settings_stack.add(m_help_page, m_help_page.getSettingsName(), m_help_page.getSettingsTitle());
    m_settings_stack.add(m_input_page, m_input_page.getSettingsName(), m_input_page.getSettingsTitle());

    // Send All Keys to the Button Mapper
    auto event_controller = Gtk::EventControllerKey::create();
    event_controller->signal_key_pressed().connect([this](guint keyval, guint keycode, Gdk::ModifierType state){
        return m_controller->GetButtonMapper()->MappedKeyDown(gdk_keyval_name(keyval));
    }, false);
    event_controller->signal_key_released().connect([this](guint keyval, guint keycode, Gdk::ModifierType state){
        m_controller->GetButtonMapper()->MappedKeyUp(gdk_keyval_name(keyval));
    }, false);
    add_controller(event_controller);
}