#include "MainWindow.h"
#include "Parameters.h"
#include "Preferences/PreferencesWindow.h"
#include "UIComponents/RenderingCanvas.h"
#include "cairomm/fontface.h"
#include "gdkmm/display.h"
#include "gdkmm/event.h"
#include "glibmm/refptr.h"
#include "gtkmm/enums.h"
#include "pangomm/fontdescription.h"
#include "gdk/gdkkeys.h"
#include <memory>

Cairo::ToyFontFace::Slant getSlantFromPango(Pango::Style s){
    if(s == Pango::Style::NORMAL) return Cairo::ToyFontFace::Slant::NORMAL;
    if(s == Pango::Style::ITALIC) return Cairo::ToyFontFace::Slant::ITALIC;
    if(s == Pango::Style::OBLIQUE) return Cairo::ToyFontFace::Slant::OBLIQUE;
    return Cairo::ToyFontFace::Slant::NORMAL;
}

#pragma clang optimize off
MainWindow::MainWindow() :
    m_main_vertical_box(Gtk::Orientation::VERTICAL),
    m_side_panel(Gtk::Orientation::VERTICAL),
    m_main_pane(Gtk::Orientation::HORIZONTAL),
    m_canvas(),
    m_preferences_window(m_canvas.Settings, m_canvas.dasherController),
    m_learning_switch(Dasher::Parameter::BP_LM_ADAPTIVE, m_canvas.Settings),
    m_speed_adjustment(Dasher::Parameter::LP_MAX_BITRATE, m_canvas.Settings, 50, 1000, 1),
    m_color_chooser(m_canvas.Settings, m_canvas.dasherController->GetColorIO()->GetKnownPalettes()),
    m_alphabet_chooser(Dasher::Parameter::SP_ALPHABET_ID, m_canvas.Settings, m_canvas.dasherController->GetPermittedValues(Dasher::Parameter::SP_ALPHABET_ID))
{
    g_setenv("GTK_CSD", "0", false);

    Glib::RefPtr<Gtk::CssProvider> css = Gtk::CssProvider::create();
    css->load_from_path("./UIStyle.css");
    get_style_context()->add_provider_for_display(Gdk::Display::get_default(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    set_title("Dasher v6");

    set_child(m_main_vertical_box);
    m_main_vertical_box.append(m_header_bar);
    m_main_vertical_box.append(m_main_pane);
    m_main_vertical_box.append(m_footer_bar);

    m_message_overlay.set_child(m_canvas);
    m_message_overlay.ConnectToDasher(m_canvas.dasherController);

    // Pack Header Bar
    m_header_bar.pack_start(m_new_button);
    m_header_bar.pack_start(m_open_button);
    m_header_bar.pack_start(m_save_button);
    m_header_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_header_bar.pack_start(m_play_button);
    m_header_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_header_bar.pack_start(m_layout_chooser);
    m_header_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_header_bar.pack_start(m_pref_button);
    m_header_bar.add_css_class("topbar");

    m_preferences_window.signal_close_request().connect([this](){
        m_preferences_window.set_visible(false);
        return true;
    }, false);
    m_pref_button.signal_clicked().connect([this](){
        m_preferences_window.set_visible(true);
    });

    m_new_button.signal_clicked().connect([this](){
        m_canvas.dasherController->Message("Interrupting Message", true);
    });
    m_open_button.signal_clicked().connect([this](){
        m_canvas.dasherController->Message("Timed Message", false);
    });

    // Send All Keys to the Button Mapper
    auto event_controller = Gtk::EventControllerKey::create();
    event_controller->signal_key_pressed().connect([this](guint keyval, guint keycode, Gdk::ModifierType state){
        return m_canvas.dasherController->GetButtonMapper()->MappedKeyDown(gdk_keyval_name(keyval));
    }, false);
    event_controller->signal_key_released().connect([this](guint keyval, guint keycode, Gdk::ModifierType state){
        m_canvas.dasherController->GetButtonMapper()->MappedKeyUp(gdk_keyval_name(keyval));
    }, false);
    add_controller(event_controller);

    // Pack Footer Bar
    m_footer_bar.pack_start(m_alphabet_chooser);
    m_footer_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_footer_bar.pack_start(m_speed_adjustment);
    m_footer_bar.pack_start(m_learning_label);
    m_footer_bar.pack_start(m_learning_switch);
    m_footer_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_footer_bar.pack_start(m_color_chooser);
    m_footer_bar.pack_start(m_font_chooser);
    m_footer_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_footer_bar.pack_start(m_speech_enable_label);
    m_footer_bar.pack_start(m_speech_enable_switch);
    m_speech_enable_switch.set_valign(Gtk::Align::CENTER);
    m_learning_switch.set_valign(Gtk::Align::CENTER);

    m_font_chooser.property_font_desc().signal_changed().connect([this](){
        Pango::FontDescription newFont = m_font_chooser.get_font_desc();
        m_canvas.renderingBackend->select_font_face(newFont.get_family(),
            getSlantFromPango(newFont.get_style()),
            (newFont.get_weight() == Pango::Weight::NORMAL) ? Cairo::ToyFontFace::Weight::NORMAL : Cairo::ToyFontFace::Weight::BOLD);
        m_canvas.dasherController->SetLongParameter(Dasher::Parameter::LP_DASHER_FONTSIZE, newFont.get_size() / Pango::SCALE);
    });

    m_main_pane.set_start_child(m_message_overlay);
    m_main_pane.set_resize_start_child(true);
    m_main_pane.set_end_child(m_side_panel);

    m_panel_bar.pack_start(m_accessibility_button);
    m_panel_bar.pack_start(m_readout_button);
    m_panel_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_panel_bar.pack_start(m_copy_button);
    m_panel_bar.pack_start(m_copyall_button);
    m_panel_bar.pack_start(m_paste_button);
    m_panel_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_panel_bar.pack_start(m_undo_button);
    m_panel_bar.pack_start(m_redo_button);
    m_panel_bar.set_size_request(150,-1);
    m_panel_bar.add_css_class("topbar");
    m_canvas.dasherController->OnBufferChange.Subscribe(&m_text_view, [this](const std::string& newBuffer){
        m_text_view.get_buffer()->set_text(newBuffer);
    });
    
    m_side_panel.append(m_panel_bar);
    m_side_panel.append(m_text_view);

    m_text_view.set_vexpand(true);
    m_text_view.set_valign(Gtk::Align::FILL);
    m_text_view.set_margin(5);

    m_paste_button.signal_clicked().connect([this](){
        m_text_view.activate_action("clipboard.paste");
    });

    m_copy_button.signal_clicked().connect([this](){
        m_text_view.activate_action("clipboard.copy");
    });

    m_copyall_button.signal_clicked().connect([this](){
        m_text_view.activate_action("selection.select-all");
        m_text_view.activate_action("clipboard.copy");
    });

    m_undo_button.signal_clicked().connect([this](){
        m_text_view.activate_action("text.undo");
    });

    m_redo_button.signal_clicked().connect([this](){
        m_text_view.activate_action("text.redo");
    });
}
#pragma clang optimize on