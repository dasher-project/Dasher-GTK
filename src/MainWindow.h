#pragma once

#include "ImageButton.h"
#include "RenderingCanvas.h"
#include "glibmm/refptr.h"
#include "gtkmm.h"
#include "MessageOverlay.h"
#include "ColorDisplayWidget.h"
#include "PaletteProxy.h"
#include <memory>

class MainWindow : public Gtk::Window
{

public:
    MainWindow();

protected:
    Gtk::Box m_main_vertical_box;
    Gtk::Paned m_main_pane;
    Glib::RefPtr<Gio::ListStore<PaletteProxy>> colorPaletteList = Gio::ListStore<PaletteProxy>::create();

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
    MessageOverlay m_message_overlay;
    
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