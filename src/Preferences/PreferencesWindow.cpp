#include "PreferencesWindow.h"
#include "SettingsMode.h"

PreferencesWindow::PreferencesWindow(std::shared_ptr<DasherController> controller): controller(controller), m_mode_page(controller){
    set_child(m_sideways_box);
    set_title("Dasher Preferences");

    m_sideways_box.append(m_sidebar);
    m_sideways_box.append(m_settings_stack);
    m_sidebar.set_stack(m_settings_stack);

    //Add Pages
    m_settings_stack.add(m_mode_page, m_mode_page.getSettingsName(), m_mode_page.getSettingsTitle());
}