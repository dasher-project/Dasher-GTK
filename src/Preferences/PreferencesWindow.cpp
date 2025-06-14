#include "PreferencesWindow.h"
#include "SettingsMode.h"
#include "SettingsStore.h"

PreferencesWindow::PreferencesWindow(std::shared_ptr<Dasher::CSettingsStore> settings, std::shared_ptr<DasherController> controller): controller(controller), settings(settings), m_mode_page(settings, controller){
    set_child(m_sideways_box);
    set_title("Dasher Preferences");

    m_sideways_box.append(m_sidebar);
    m_sideways_box.append(m_settings_stack);
    m_sidebar.set_stack(m_settings_stack);

    //Add Pages
    m_settings_stack.add(m_mode_page, m_mode_page.getSettingsName(), m_mode_page.getSettingsTitle());
    m_settings_stack.add(m_help_page, m_help_page.getSettingsName(), m_help_page.getSettingsTitle());
}