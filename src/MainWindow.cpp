#include "MainWindow.h"
#include "dasher.h"
#include <cairomm/fontface.h>
#include <gdkmm/display.h>
#include <pangomm/fontdescription.h>
#include <memory>

static Cairo::ToyFontFace::Slant getSlantFromPango(Pango::Style s) {
    if (s == Pango::Style::ITALIC) return Cairo::ToyFontFace::Slant::ITALIC;
    if (s == Pango::Style::OBLIQUE) return Cairo::ToyFontFace::Slant::OBLIQUE;
    return Cairo::ToyFontFace::Slant::NORMAL;
}

MainWindow::MainWindow()
    : m_alphabet_chooser(95, m_canvas.bridge, m_canvas.bridge->get_parameter_string_values(95))
    , m_speed_adjustment(30, m_canvas.bridge, 20, 400, 5)
    , m_learning_switch(15, m_canvas.bridge)
    , m_color_chooser(m_canvas.bridge)
    , m_preferences_window(m_canvas.bridge)
{
    Glib::RefPtr<Gtk::CssProvider> css = Gtk::CssProvider::create();
    css->load_from_path("./UIStyle.css");
    get_style_context()->add_provider_for_display(Gdk::Display::get_default(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    set_title("Dasher v6");
    set_default_size(900, 600);

    set_child(m_main_box);
    m_main_box.append(m_header_bar);
    m_main_box.append(m_pane);
    m_main_box.append(m_footer_bar);

    m_message_overlay.set_child(m_canvas);
    m_message_overlay.ConnectToDasher(m_canvas.bridge);

    m_header_bar.pack_start(m_new_button);
    m_header_bar.pack_start(m_open_button);
    m_header_bar.pack_start(m_save_button);
    m_header_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_header_bar.pack_start(m_play_button);
    m_header_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_header_bar.pack_start(m_pref_button);
    m_header_bar.add_css_class("topbar");

    m_preferences_window.signal_close_request().connect([this]() {
        m_preferences_window.set_visible(false);
        return true;
    }, false);
    m_pref_button.signal_clicked().connect([this]() {
        m_preferences_window.set_visible(true);
    });

    m_new_button.signal_clicked().connect([this]() {
        m_canvas.bridge->reset_output_text();
    });

    auto event_controller = Gtk::EventControllerKey::create();
    event_controller->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType) {
        std::string key_name = gdk_keyval_name(keyval);
        if (key_name == "space") {
            m_canvas.bridge->key_event(0, 1);
            return true;
        }
        if (key_name == "Return" || key_name == "KP_Enter") {
            m_canvas.bridge->key_event(0, 1);
            return true;
        }
        if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_4) {
            m_canvas.bridge->key_event(keyval - GDK_KEY_1 + 1, 1);
            return true;
        }
        if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F4) {
            m_canvas.bridge->key_event(keyval - GDK_KEY_F1 + 1, 1);
            return true;
        }
        return false;
    }, false);
    event_controller->signal_key_released().connect([this](guint keyval, guint, Gdk::ModifierType) {
        std::string key_name = gdk_keyval_name(keyval);
        if (key_name == "space" || key_name == "Return" || key_name == "KP_Enter") {
            m_canvas.bridge->key_event(0, 0);
            return;
        }
        if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_4) {
            m_canvas.bridge->key_event(keyval - GDK_KEY_1 + 1, 0);
            return;
        }
        if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F4) {
            m_canvas.bridge->key_event(keyval - GDK_KEY_F1 + 1, 0);
            return;
        }
    }, false);
    add_controller(event_controller);

    m_footer_bar.pack_start(m_alphabet_chooser);
    m_footer_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_footer_bar.pack_start(m_speed_adjustment);
    m_footer_bar.pack_start(m_learning_label);
    m_footer_bar.pack_start(m_learning_switch);
    m_footer_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_footer_bar.pack_start(m_color_chooser);
    m_footer_bar.pack_start(m_font_chooser);
    m_footer_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_footer_bar.pack_start(m_speech_label);
    m_footer_bar.pack_start(m_speech_switch);
    m_speech_switch.set_valign(Gtk::Align::CENTER);
    m_learning_switch.set_valign(Gtk::Align::CENTER);

    m_font_chooser.property_font_desc().signal_changed().connect([this]() {
        Pango::FontDescription font = m_font_chooser.get_font_desc();
        m_canvas.renderer->set_font_family(font.get_family());
        m_canvas.renderer->set_font_slant(getSlantFromPango(font.get_style()));
        m_canvas.renderer->set_font_weight(
            font.get_weight() == Pango::Weight::NORMAL
                ? Cairo::ToyFontFace::Weight::NORMAL
                : Cairo::ToyFontFace::Weight::BOLD);
    });

    m_pane.set_start_child(m_message_overlay);
    m_pane.set_resize_start_child(true);
    m_pane.set_end_child(m_side_panel);

    m_panel_bar.pack_start(m_copy_button);
    m_panel_bar.pack_start(m_copyall_button);
    m_panel_bar.pack_start(m_paste_button);
    m_panel_bar.pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_panel_bar.pack_start(m_read_button);
    m_panel_bar.set_size_request(150, -1);
    m_panel_bar.add_css_class("topbar");

    m_canvas.OnBufferChange.connect([this](const std::string& text) {
        m_text_view.get_buffer()->set_text(text);
    });

    m_side_panel.append(m_panel_bar);
    m_side_panel.append(m_text_view);
    m_text_view.set_vexpand(true);
    m_text_view.set_valign(Gtk::Align::FILL);
    m_text_view.set_margin(5);
    m_text_view.add_css_class("dasher-output");

    m_paste_button.signal_clicked().connect([this]() {
        m_text_view.activate_action("clipboard.paste");
    });
    m_copy_button.signal_clicked().connect([this]() {
        m_text_view.activate_action("clipboard.copy");
    });
    m_copyall_button.signal_clicked().connect([this]() {
        m_text_view.activate_action("selection.select-all");
        m_text_view.activate_action("clipboard.copy");
    });
}
