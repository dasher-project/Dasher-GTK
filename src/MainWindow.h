#pragma once

#include "ImageButton.h"
#include "RenderingCanvas.h"
#include "glibmm/refptr.h"
#include "gtkmm/enums.h"
#include "gtkmm/fontdialog.h"
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>
#include <gtkmm/paned.h>
#include <gtkmm/actionbar.h>
#include <gtkmm/box.h>
#include <gtkmm/separator.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/switch.h>
#include <gtkmm/fontdialogbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/overlay.h>
#include <gtkmm/revealer.h>

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
    Gtk::Separator m_separator_1 = Gtk::Separator(Gtk::Orientation::VERTICAL);
    ImageButton m_play_button = ImageButton("Play", "input-gaming-symbolic");
    Gtk::Separator m_separator_2 = Gtk::Separator(Gtk::Orientation::VERTICAL);
    Gtk::DropDown m_layout_chooser;
    Gtk::Separator m_separator_3 = Gtk::Separator(Gtk::Orientation::VERTICAL);
    ImageButton m_pref_button = ImageButton("Prefs", "applications-system-symbolic");
    
    // Main Content
    Gtk::Box m_side_panel = Gtk::Box(Gtk::Orientation::VERTICAL);
    RenderingCanvas m_canvas;
    Gtk::Box m_canvas_box = Gtk::Box(Gtk::Orientation::VERTICAL);
    Gtk::Overlay m_message_overlay;
    Gtk::Revealer m_message_revealer;
    Gtk::Label m_test = Gtk::Label("This is a real test test.");
    
    // Footer Bar
    Gtk::ActionBar m_footer_bar;
    Gtk::DropDown m_alphabet_chooser;
    Gtk::Separator m_separator_4 = Gtk::Separator(Gtk::Orientation::VERTICAL);
    Gtk::SpinButton m_speed_adjustment = Gtk::SpinButton();
    Gtk::Label m_learning_label = Gtk::Label("Learning");
    Gtk::Switch m_learning_switch;
    Gtk::Separator m_separator_5 = Gtk::Separator(Gtk::Orientation::VERTICAL);
    Gtk::DropDown m_color_chooser;
    Glib::RefPtr<Gtk::FontDialog> m_font_dialog = Gtk::FontDialog::create();
    Gtk::FontDialogButton m_font_chooser = Gtk::FontDialogButton(m_font_dialog);
    Gtk::Separator m_separator_6 = Gtk::Separator(Gtk::Orientation::VERTICAL);
    Gtk::Label m_speech_enable_label = Gtk::Label("Speech");
    Gtk::Switch m_speech_enable_switch;
};