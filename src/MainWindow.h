#pragma once

#include "UIComponents/ImageButton.h"
#include "Preferences/PreferencesWindow.h"
#include "UIComponents/SyncedSpinButton.h"
#include "UIComponents/SyncedStringDropdown.h"
#include "UIComponents/SyncedSwitch.h"
#include "UIComponents/RenderingCanvas.h"
#include "UIComponents/MessageOverlay.h"
#include "UIComponents/SyncedColorDropdown.h"

#include "gtkmm/window.h"
#include "gtkmm/box.h"
#include "gtkmm/label.h"
#include "gtkmm/paned.h"
#include "gtkmm/actionbar.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/switch.h"
#include "gtkmm/fontdialog.h"
#include "gtkmm/fontdialogbutton.h"

class MainWindow : public Gtk::Window
{

public:
    MainWindow();

protected:
    Gtk::Box m_main_vertical_box;
    Gtk::Paned m_main_pane;

    // Top Menu:
    Gtk::ActionBar m_header_bar;
    ImageButton m_new_button = ImageButton("New", "document-new-symbolic");
    ImageButton m_open_button = ImageButton("Open", "document-open-symbolic");
    ImageButton m_save_button = ImageButton("Save", "document-save-symbolic");
    ImageButton m_play_button = ImageButton("Play", "input-gaming-symbolic");
    Gtk::DropDown m_layout_chooser;
    ImageButton m_pref_button = ImageButton("Prefs", "applications-system-symbolic");
    
    // Main Content
    Gtk::Box m_side_panel = Gtk::Box(Gtk::Orientation::VERTICAL);
    RenderingCanvas m_canvas;
    MessageOverlay m_message_overlay;

    //Side Panel
    Gtk::ActionBar m_panel_bar;
    ImageButton m_accessibility_button = ImageButton("Enlarge", "view-reveal-symbolic");
    ImageButton m_readout_button = ImageButton("Read", "audio-speakers-symbolic");
    ImageButton m_copy_button = ImageButton("Copy", "edit-copy-symbolic");
    ImageButton m_copyall_button = ImageButton("Copy all", "edit-select-all-symbolic");
    ImageButton m_paste_button = ImageButton("Paste", "edit-paste-symbolic");
    ImageButton m_undo_button = ImageButton("Undo", "edit-undo-symbolic");
    ImageButton m_redo_button = ImageButton("Redo", "edit-redo-symbolic");
    Gtk::TextView m_text_view = Gtk::TextView();
    
    // Footer Bar
    Gtk::ActionBar m_footer_bar;
    SyncedStringDropdown m_alphabet_chooser;
    SyncedSpinButton m_speed_adjustment;
    Gtk::Label m_learning_label = Gtk::Label("Learning");
    SyncedSwitch m_learning_switch;
    SyncedColorDropdown m_color_chooser;
    Glib::RefPtr<Gtk::FontDialog> m_font_dialog = Gtk::FontDialog::create();
    Gtk::FontDialogButton m_font_chooser = Gtk::FontDialogButton(m_font_dialog);
    Gtk::Label m_speech_enable_label = Gtk::Label("Speech");
    Gtk::Switch m_speech_enable_switch;

    // Preferences Window
    PreferencesWindow m_preferences_window;
};