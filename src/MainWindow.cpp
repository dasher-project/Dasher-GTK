#include "MainWindow.h"
#include "dasher.h"
#include <cairomm/fontface.h>
#include <gdkmm/display.h>
#include <pangomm/fontdescription.h>
#include <glibmm/main.h>
#include <cmath>
#include <cstdio>
#include <memory>

static Cairo::ToyFontFace::Slant getSlantFromPango(Pango::Style s) {
    if (s == Pango::Style::ITALIC) return Cairo::ToyFontFace::Slant::ITALIC;
    if (s == Pango::Style::OBLIQUE) return Cairo::ToyFontFace::Slant::OBLIQUE;
    return Cairo::ToyFontFace::Slant::NORMAL;
}

MainWindow::MainWindow()
    // Resolve parameter keys by their stable enum names rather than hardcoding
    // numeric indices: Dasher::Parameter values are an internal detail of
    // DasherCore and are renumbered between releases.
    : m_alphabet_chooser(
          m_canvas.bridge->find_parameter_key("SP_ALPHABET_ID"), m_canvas.bridge,
          m_canvas.bridge->get_parameter_string_values(m_canvas.bridge->find_parameter_key("SP_ALPHABET_ID"))),
      m_speed_adjustment(m_canvas.bridge->find_parameter_key("LP_MAX_BITRATE"), m_canvas.bridge, 20, 400, 5),
      m_learning_switch(m_canvas.bridge->find_parameter_key("BP_LM_ADAPTIVE"), m_canvas.bridge),
      m_color_chooser(m_canvas.bridge), m_preferences_window(m_canvas.bridge) {
    Glib::RefPtr<Gtk::CssProvider> css = Gtk::CssProvider::create();
    // Load the stylesheet from the GResource bundle compiled into the binary
    // (see src/dasher.gresource.xml). Loading from a file path meant the CSS
    // was resolved against the current working directory, so packaged builds
    // (Flatpak/AppImage), whose CWD isn't the source tree, silently fell back
    // to unstyled default GTK. Embedding it removes any CWD/layout dependency.
    css->load_from_resource("/org/alternativeinterface/dasher/UIStyle.css");
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

    m_open_button.signal_clicked().connect([this]() {
        auto dialog = Gtk::FileDialog::create();
        dialog->set_title("Open Text File");
        auto filter = Gtk::FileFilter::create();
        filter->set_name("Text files");
        filter->add_mime_type("text/plain");
        dialog->set_default_filter(filter);
        dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto file = dialog->open_finish(result);
                if (!file) return;
                auto stream = file->read();
                gsize size = 0;
                auto bytes = stream->read_bytes(1024 * 1024, Glib::RefPtr<Gio::Cancellable>());
                auto data = bytes->get_data(size);
                std::string contents(static_cast<const char*>(data), size);
                stream->close();
                m_canvas.bridge->reset_output_text();
                m_text_view.get_buffer()->set_text(contents);
            } catch (const Glib::Error& e) {
                m_message_overlay.show_message("Failed to open: " + std::string(e.what()));
            }
        });
    });

    m_save_button.signal_clicked().connect([this]() {
        auto dialog = Gtk::FileDialog::create();
        dialog->set_title("Save Text File");
        auto filter = Gtk::FileFilter::create();
        filter->set_name("Text files");
        filter->add_mime_type("text/plain");
        dialog->set_default_filter(filter);
        dialog->save(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto file = dialog->save_finish(result);
                if (!file) return;
                auto buffer = m_text_view.get_buffer();
                std::string text = buffer->get_text();
                auto stream = file->replace();
                stream->write(text);
                stream->close();
                m_message_overlay.show_message("Saved: " + file->get_basename(), true);
            } catch (const Glib::Error& e) {
                m_message_overlay.show_message("Failed to save: " + std::string(e.what()));
            }
        });
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
    m_footer_bar.pack_start(m_dwell_label);
    m_footer_bar.pack_start(m_dwell_switch);
    m_footer_bar.pack_start(m_keyboard_label);
    m_footer_bar.pack_start(m_keyboard_switch);
    m_footer_bar.pack_start(m_rate_toggle_label);
    m_footer_bar.pack_start(m_rate_switch);
    m_footer_bar.pack_start(m_rate_value);
    m_speech_switch.set_valign(Gtk::Align::CENTER);
    m_dwell_switch.set_valign(Gtk::Align::CENTER);
    m_learning_switch.set_valign(Gtk::Align::CENTER);
    m_keyboard_switch.set_valign(Gtk::Align::CENTER);
    m_rate_switch.set_valign(Gtk::Align::CENTER);

    // Typing-rate readout (RFC 0012). Off by default; when shown, refresh at
    // ~2 Hz so the number doesn't churn (the engine keeps a 5s rolling window).
    m_rate_value.set_visible(false);
    m_rate_switch.property_active().signal_changed().connect([this]() {
        bool on = m_rate_switch.get_active();
        m_rate_value.set_visible(on);
        if (on) update_typing_rate();
    });
    Glib::signal_timeout().connect(
        [this]() -> bool {
            if (m_rate_switch.get_active()) update_typing_rate();
            return true;
        },
        500);

    m_direct_mode = std::make_unique<DirectModeService>();
    m_tts = std::make_unique<TtsService>();
    m_keyboard_switch.set_sensitive(m_direct_mode->is_available());
    m_speech_switch.set_sensitive(m_tts->is_available());
    if (!m_direct_mode->is_available()) {
        m_keyboard_label.set_tooltip_text("Install ydotool for on-screen keyboard mode");
    }

    m_keyboard_switch.property_active().signal_changed().connect([this]() {
        m_direct_mode_active = m_keyboard_switch.get_active();
        if (m_direct_mode_active) {
            m_pane.set_shrink_end_child(false);
            m_pane.set_position(get_width() - 50);
        } else {
            m_pane.set_shrink_end_child(true);
            m_pane.set_position(get_width() * 2 / 3);
        }
    });

    m_dwell_switch.property_active().signal_changed().connect([this]() {
        m_canvas.dwell_handler->set_enabled(m_dwell_switch.get_active());
    });

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

    m_read_button.signal_clicked().connect([this]() {
        if (!m_tts || !m_tts->is_available()) return;
        auto buffer = m_text_view.get_buffer();
        std::string text = buffer->get_text();
        if (!text.empty()) {
            m_tts->speak(text);
        }
    });

    m_canvas.OnBufferChange.connect([this](const std::string& text) {
        m_text_view.get_buffer()->set_text(text);
        if (m_speech_switch.get_active() && m_tts && m_tts->is_available()) {
            if (!text.empty() && text.back() == ' ') {
                std::string last_word;
                auto pos = text.rfind(' ', text.size() - 2);
                if (pos == std::string::npos) {
                    last_word = text.substr(0, text.size() - 1);
                } else {
                    last_word = text.substr(pos + 1, text.size() - pos - 2);
                }
                if (!last_word.empty()) {
                    m_tts->stop();
                    m_tts->speak(last_word);
                }
            }
        }
    });

    m_canvas.OnOutputEvent.connect([this](int event_type, const std::string& text) {
        if (!m_direct_mode_active || text.empty()) return;
        if (event_type == 0) {
            m_direct_mode->inject_text(text);
        } else if (event_type == 1) {
            int count = 0;
            try {
                count = static_cast<int>(text.size());
            } catch (...) {
                count = 1;
            }
            m_direct_mode->inject_delete(count);
        }
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

void MainWindow::update_typing_rate() {
    // Engine-reported smoothed rate (WPM = CPS * 12, RFC 0012). Formatted as
    // e.g. "4.2 cps · 50 wpm"; the separator is a UTF-8 middle dot (\xc2\xb7).
    double cps = m_canvas.bridge->get_cps();
    double wpm = m_canvas.bridge->get_wpm();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f cps \xc2\xb7 %d wpm", cps, static_cast<int>(std::llround(wpm)));
    m_rate_value.set_text(buf);
}
