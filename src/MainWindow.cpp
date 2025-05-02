#include "MainWindow.h"
#include "Parameters.h"
#include "RenderingCanvas.h"
#include "cairomm/fontface.h"
#include "cairomm/fontoptions.h"
#include "giomm/listmodel.h"
#include "glibmm/ustring.h"
#include "gtk/gtk.h"
#include "gtkmm/enums.h"
#include "gtkmm/adjustment.h"
#include "gtkmm/label.h"
#include "gtkmm/stringobject.h"
#include "glibmm/refptr.h"
#include "pangomm/font.h"
#include "pangomm/fontdescription.h"
#include <gtkmm/window.h>
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
    m_main_pane(Gtk::Orientation::HORIZONTAL)
{
    g_setenv("GTK_CSD", "0", false);

    set_title("Dasher v6");

    set_child(m_main_vertical_box);
    m_main_vertical_box.append(m_header_bar);
    m_main_vertical_box.append(m_main_pane);
    m_main_vertical_box.append(m_footer_bar);

    m_message_overlay.set_child(m_canvas);

    m_message_revealer.set_valign(Gtk::Align::END);
    m_message_revealer.set_vexpand(false);
    m_message_overlay.add_overlay(m_message_revealer);
    m_message_revealer.set_child(m_test);

    m_test.set_text("<span foreground='blue' weight='ultrabold' font='40'>This is a real test test.</span>");
    m_test.set_use_markup(true);
    m_speech_enable_switch.property_state().signal_changed().connect([this](){
        m_message_revealer.set_reveal_child(m_speech_enable_switch.get_state());
    });

    // Pack Header Bar
    m_header_bar.pack_start(m_new_button);
    m_header_bar.pack_start(m_open_button);
    m_header_bar.pack_start(m_save_button);
    m_header_bar.pack_start(m_separator_1);
    m_header_bar.pack_start(m_play_button);
    m_header_bar.pack_start(m_separator_2);
    m_header_bar.pack_start(m_layout_chooser);
    m_header_bar.pack_start(m_separator_3);
    m_header_bar.pack_start(m_pref_button);

    // Pack Footer Bar
    m_footer_bar.pack_start(m_alphabet_chooser);
    m_footer_bar.pack_start(m_separator_4);
    m_footer_bar.pack_start(m_speed_adjustment);
    m_footer_bar.pack_start(m_learning_label);
    m_footer_bar.pack_start(m_learning_switch);
    m_footer_bar.pack_start(m_separator_5);
    m_footer_bar.pack_start(m_color_chooser);
    m_footer_bar.pack_start(m_font_chooser);
    m_footer_bar.pack_start(m_separator_6);
    m_footer_bar.pack_start(m_speech_enable_label);
    m_footer_bar.pack_start(m_speech_enable_switch);
    m_speech_enable_switch.set_valign(Gtk::Align::CENTER);
    m_learning_switch.set_valign(Gtk::Align::CENTER);

    std::vector<std::string> Alphabets;
    m_canvas.dasherController->GetPermittedValues(Dasher::Parameter::SP_ALPHABET_ID, Alphabets);
    std::vector<Glib::ustring> AlphaUString(Alphabets.begin(), Alphabets.end());
    m_alphabet_chooser.set_model(Gtk::StringList::create(AlphaUString));
    m_alphabet_chooser.property_selected_item().signal_changed().connect([this](){
        Glib::ustring s = std::dynamic_pointer_cast<Gtk::StringObject>(m_alphabet_chooser.get_selected_item())->get_string();
        m_canvas.dasherController->SetStringParameter(Dasher::Parameter::SP_ALPHABET_ID, s);
    });

    m_font_chooser.property_font_desc().signal_changed().connect([this](){
        Pango::FontDescription newFont = m_font_chooser.get_font_desc();
        m_canvas.renderingBackend->select_font_face(newFont.get_family(),
            getSlantFromPango(newFont.get_style()),
            (newFont.get_weight() == Pango::Weight::NORMAL) ? Cairo::ToyFontFace::Weight::NORMAL : Cairo::ToyFontFace::Weight::BOLD);
        m_canvas.dasherController->SetLongParameter(Dasher::Parameter::LP_DASHER_FONTSIZE, newFont.get_size() / Pango::SCALE);
    });

    m_speed_adjustment.set_adjustment(Gtk::Adjustment::create(m_canvas.dasherController->GetLongParameter(Dasher::Parameter::LP_MAX_BITRATE), 50, 1000));
    m_speed_adjustment.property_value().signal_changed().connect([this](){
        m_canvas.dasherController->SetLongParameter(Dasher::Parameter::LP_MAX_BITRATE, m_speed_adjustment.get_value_as_int());
    });

    m_main_pane.set_start_child(m_message_overlay);
    m_main_pane.set_resize_start_child(true);
    m_main_pane.set_end_child(m_side_panel);

    m_side_panel.set_size_request(150,-1);
    m_side_panel.set_vexpand(true);
    m_side_panel.set_valign(Gtk::Align::FILL);
}
#pragma clang optimize on