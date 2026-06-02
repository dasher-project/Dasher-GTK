#pragma once

#include "Engine/DasherBridge.h"
#include "Output/DirectModeService.h"
#include "Output/TtsService.h"
#include "UIComponents/ImageButton.h"
#include "UIComponents/RenderingCanvas.h"
#include "UIComponents/MessageOverlay.h"
#include "UIComponents/SyncedSpinButton.h"
#include "UIComponents/SyncedStringDropdown.h"
#include "UIComponents/SyncedSwitch.h"
#include "UIComponents/SyncedColorDropdown.h"
#include "Preferences/PreferencesWindow.h"
#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/paned.h>
#include <gtkmm/actionbar.h>
#include <gtkmm/label.h>
#include <gtkmm/textview.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/separator.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/fontdialog.h>
#include <gtkmm/fontdialogbutton.h>
#include <gtkmm/switch.h>
#include <memory>

class MainWindow : public Gtk::Window {
public:
    MainWindow();

protected:
    Gtk::Box m_main_box = Gtk::Box(Gtk::Orientation::VERTICAL);
    Gtk::Paned m_pane = Gtk::Paned(Gtk::Orientation::HORIZONTAL);

    Gtk::ActionBar m_header_bar;
    ImageButton m_new_button = ImageButton("New", "document-new-symbolic");
    ImageButton m_open_button = ImageButton("Open", "document-open-symbolic");
    ImageButton m_save_button = ImageButton("Save", "document-save-symbolic");
    ImageButton m_play_button = ImageButton("Play", "input-gaming-symbolic");
    ImageButton m_pref_button = ImageButton("Prefs", "applications-system-symbolic");

    RenderingCanvas m_canvas;
    MessageOverlay m_message_overlay;

    Gtk::Box m_side_panel = Gtk::Box(Gtk::Orientation::VERTICAL);
    Gtk::ActionBar m_panel_bar;
    ImageButton m_copy_button = ImageButton("Copy", "edit-copy-symbolic");
    ImageButton m_copyall_button = ImageButton("Copy all", "edit-select-all-symbolic");
    ImageButton m_paste_button = ImageButton("Paste", "edit-paste-symbolic");
    ImageButton m_read_button = ImageButton("Read", "audio-speakers-symbolic");
    Gtk::TextView m_text_view;

    Gtk::ActionBar m_footer_bar;
    SyncedStringDropdown m_alphabet_chooser;
    SyncedSpinButton m_speed_adjustment;
    Gtk::Label m_learning_label = Gtk::Label("Learning");
    SyncedSwitch m_learning_switch;
    SyncedColorDropdown m_color_chooser;
    Glib::RefPtr<Gtk::FontDialog> m_font_dialog = Gtk::FontDialog::create();
    Gtk::FontDialogButton m_font_chooser = Gtk::FontDialogButton(m_font_dialog);
    Gtk::Label m_speech_label = Gtk::Label("Speech");
    Gtk::Switch m_speech_switch;
    Gtk::Label m_keyboard_label = Gtk::Label("Keyboard");
    Gtk::Switch m_keyboard_switch;

    std::unique_ptr<DirectModeService> m_direct_mode;
    std::unique_ptr<TtsService> m_tts;
    bool m_direct_mode_active = false;

    PreferencesWindow m_preferences_window;
};
