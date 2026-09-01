#include "MainWindow.h"
#include "dasher.h"
#include "i18n.h"
#include <cairomm/fontface.h>
#include <gdkmm/display.h>
#include <gdkmm/monitor.h>
#include <giomm/listmodel.h>
#include <pangomm/fontdescription.h>
#include <glibmm/main.h>
#include <cmath>
#include <glibmm/markup.h>
#include <cstdio>
#include <memory>

MainWindow::MainWindow()
    // Resolve parameter keys by their stable enum names rather than hardcoding
    // numeric indices: Dasher::Parameter values are an internal detail of
    // DasherCore and are renumbered between releases.
    : m_alphabet_chooser(
          m_canvas.bridge->find_parameter_key("SP_ALPHABET_ID"), m_canvas.bridge,
          m_canvas.bridge->get_parameter_string_values(m_canvas.bridge->find_parameter_key("SP_ALPHABET_ID"))),
      m_learning_switch(m_canvas.bridge->find_parameter_key("BP_LM_ADAPTIVE"), m_canvas.bridge),
      m_preferences_window(m_canvas.bridge, m_canvas.dwell_handler.get()) {
    Glib::RefPtr<Gtk::CssProvider> css = Gtk::CssProvider::create();
    // Load the stylesheet from the GResource bundle compiled into the binary
    // (see src/dasher.gresource.xml). Loading from a file path meant the CSS
    // was resolved against the current working directory, so packaged builds
    // (Flatpak/AppImage), whose CWD isn't the source tree, silently fell back
    // to unstyled default GTK. Embedding it removes any CWD/layout dependency.
    css->load_from_resource("/org/alternativeinterface/dasher/UIStyle.css");
    get_style_context()->add_provider_for_display(Gdk::Display::get_default(), css,
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    set_title(_("Dasher v6"));
    set_default_size(900, 600);

    // Restore the saved normal-mode geometry (issue #74): size + maximized
    // pre-map, position after map (X11 only — under Wayland the compositor
    // places the window and we must not fight it).
    {
        const auto saved = m_ui_settings.window_geometry();
        if (saved.valid) {
            set_default_size(saved.w, saved.h);
            if (saved.maximized) maximize();
            if (saved.has_position) {
                signal_map().connect([this, saved]() {
                    // After map the surface exists; defer one idle so the WM
                    // has finished its own initial placement.
                    Glib::signal_idle().connect_once([this, saved]() {
                        if (geometry_on_any_monitor(saved)) WindowPlacementX11::move_to(*this, saved.x, saved.y);
                    });
                });
            }
        }
    }

    set_child(m_main_box);
    m_main_box.append(m_header_bar);
    m_main_box.append(m_pane);
    m_main_box.append(m_footer_bar);

    m_message_overlay.set_child(m_canvas);
    m_message_overlay.ConnectToDasher(m_canvas.bridge);

    pack_bar_start(m_header_bar, m_new_button);
    pack_bar_start(m_header_bar, m_open_button);
    pack_bar_start(m_header_bar, m_save_button);
    pack_bar_start(m_header_bar, *Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    pack_bar_start(m_header_bar, m_play_button);
    m_play_button.signal_clicked().connect([this]() {
        // Game mode toggle (Windows/Apple parity): enter starts the training
        // target from the engine's game-mode data; leave returns to normal.
        if (m_canvas.bridge->game_mode_active()) {
            m_canvas.bridge->leave_game_mode();
            m_play_button.set_label(_("Game"));
            m_game_target_label.set_visible(false);
        } else {
            const int rc = m_canvas.bridge->enter_game_mode();
            if (rc == 0) {
                m_play_button.set_label(_("Leave game"));
                update_game_target();
            } else {
                m_message_overlay.show_message(
                    "No game text available. Add a training/gamemode file to your Dasher data directory.");
            }
        }
    });
    // Layout menu replaces the bare Keyboard toggle (Apple layoutPickerMenu /
    // Windows BtnMode): Right / Left / Bottom / Top / Keyboard.
    m_layout_menu->append(_("Right side"), "layout.right");
    m_layout_menu->append(_("Left side"), "layout.left");
    m_layout_menu->append(_("Bottom"), "layout.bottom");
    m_layout_menu->append(_("Top"), "layout.top");
    m_layout_menu->append(_("_Keyboard"), "layout.keyboard");
    m_layout_button.set_menu_model(m_layout_menu);
    {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        auto icon = Gtk::IconPaintable::create(
            Gio::File::create_for_uri("resource:///org/alternativeinterface/dasher/icons/panel-right.svg"), 24,
            get_scale_factor());
        auto* image = Gtk::make_managed<Gtk::Image>();
        image->property_paintable() = icon;
        image->set_pixel_size(18);
        box->append(*image);
        m_layout_label.set_valign(Gtk::Align::CENTER);
        box->append(m_layout_label);
        m_layout_button.set_child(*box);
        m_layout_button.set_tooltip_text(_("Pane position and direct-entry mode"));
    }
    pack_bar_start(m_header_bar, m_layout_button);
    {
        auto layout = Gio::SimpleActionGroup::create();
        auto activate = [this](const Glib::ustring& name) {
            if (name == "right")
                set_pane_layout(PaneLayout::Right);
            else if (name == "left")
                set_pane_layout(PaneLayout::Left);
            else if (name == "bottom")
                set_pane_layout(PaneLayout::Bottom);
            else if (name == "top")
                set_pane_layout(PaneLayout::Top);
            else
                set_pane_layout(PaneLayout::Keyboard);
        };
        for (const char* n : {"right", "left", "bottom", "top", "keyboard"}) {
            auto action = Gio::SimpleAction::create(n);
            action->signal_activate().connect([activate, n](const Glib::VariantBase&) { activate(n); });
            layout->add_action(action);
        }
        insert_action_group("layout", layout);
    }
    pack_bar_start(m_header_bar, *Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    pack_bar_start(m_header_bar, m_pref_button);
    m_header_bar.add_css_class("topbar");

    m_preferences_window.signal_close_request().connect(
        [this]() {
            m_preferences_window.set_visible(false);
            return true;
        },
        false);
    // RFC 0015: keyboard-mode opacity slider in Preferences → Output reads and
    // writes MainWindow's persisted value (live while keyboard mode is on).
    m_preferences_window.set_keyboard_opacity_access([this]() { return keyboard_opacity(); },
                                                     [this](double v) { set_keyboard_opacity(v); });
    // RFC 0006: appearance controls (palette + canvas font) live in
    // Preferences → Customization, like Windows/Apple — not the footer. The
    // font callback applies the picked family/style to renderer + engine.
    m_preferences_window.set_appearance_handler([this](const Glib::ustring& family, bool italic, bool bold) {
        const auto slant = italic ? Cairo::ToyFontFace::Slant::ITALIC : Cairo::ToyFontFace::Slant::NORMAL;
        const auto weight = bold ? Cairo::ToyFontFace::Weight::BOLD : Cairo::ToyFontFace::Weight::NORMAL;
        m_canvas.bridge->set_canvas_font(family, slant, weight);
    });
    // Re-read footer widgets from the engine after "Reset engine settings to
    // defaults" (dasher_reset_settings fires change notifications, but nothing
    // pushes them into the synced widgets yet).
    m_preferences_window.OnSettingsReset.connect([this]() {
        m_alphabet_chooser.update_from_bridge();
        update_speed_display();
        m_learning_switch.update_from_bridge();
    });
    // The footer dropdowns are constructed before the engine realises, so
    // their initial model is empty and they render blank. The canvas emits
    // OnEngineReady once set_screen_size() has returned — after realisation —
    // which is the first point the alphabet/palette lists actually exist.
    m_canvas.OnEngineReady.connect([this]() {
        m_alphabet_chooser.update_from_bridge();
        update_speed_display();
        m_learning_switch.update_from_bridge();
    });
    m_pref_button.signal_clicked().connect([this]() {
        // present() brings the window forward AND focuses it, where
        // set_visible(true) alone is a no-op if it's already visible behind.
        m_preferences_window.present();
    });

    m_new_button.signal_clicked().connect([this]() {
        // Full reset: clear the text AND restart the model from the root so the
        // prediction context is dropped. reset_output_text() alone would keep
        // the learned position (the canvas would resume mid-sentence). The
        // canvas shadow buffer clears itself via output event type 2
        // (DasherCore v0.2.3).
        m_canvas.bridge->reset();
    });

    // Speed spinner: take the range from the engine manifest rather than the
    // previous hardcoded 20–400. Those are raw LP_MAX_BITRATE units (v5's
    // MaxBitRateTimes100), so the old cap limited v5 users to "4.0" when v5
    // itself allowed 0.1–8.0; the engine accepts 1–1000.
    // Speed stepper in the Windows bottom-bar style ("Speed [–] 5.7 [+]"),
    // showing the v5-style value (raw LP_MAX_BITRATE / 100) instead of the
    // raw integer. Steps and range come from the engine manifest, converted
    // to v5 units.
    {
        const auto info = m_canvas.bridge->find_parameter_info("LP_MAX_BITRATE");
        if (info.key >= 0 && info.max_val > info.min_val) {
            m_speed_min = info.min_val / 100.0;
            m_speed_max = info.max_val / 100.0;
            m_speed_step = std::max(0.1, info.step / 100.0);
        }
    }
    m_speed_down_btn.set_label("\u2212"); // proper minus sign
    m_speed_up_btn.set_label("+");
    m_speed_value_label.set_width_chars(4);
    m_speed_value_label.set_halign(Gtk::Align::CENTER);
    for (auto* btn : {&m_speed_down_btn, &m_speed_up_btn}) {
        btn->set_valign(Gtk::Align::CENTER);
        btn->set_tooltip_text("Maximum writing speed, Dasher 5 scale (1.0 = 100).");
    }
    m_speed_down_btn.signal_clicked().connect([this]() { nudge_speed(-m_speed_step); });
    m_speed_up_btn.signal_clicked().connect([this]() { nudge_speed(+m_speed_step); });
    update_speed_display();

    m_open_button.signal_clicked().connect([this]() {
        auto dialog = Gtk::FileDialog::create();
        dialog->set_title(_("Open Text File"));
        auto filter = Gtk::FileFilter::create();
        filter->set_name(_("Text files"));
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
                // Shadow buffer clears via output event 2 (DasherCore v0.2.3).
                m_text_view.get_buffer()->set_text(contents);
            } catch (const Glib::Error& e) {
                m_message_overlay.show_message(std::string(_("Failed to open: ")) + std::string(e.what()));
            }
        });
    });

    m_save_button.signal_clicked().connect([this]() {
        auto dialog = Gtk::FileDialog::create();
        dialog->set_title(_("Save Text File"));
        auto filter = Gtk::FileFilter::create();
        filter->set_name(_("Text files"));
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
                m_message_overlay.show_message(std::string(_("Saved: ")) + file->get_basename(), true);
            } catch (const Glib::Error& e) {
                m_message_overlay.show_message(std::string(_("Failed to save: ")) + std::string(e.what()));
            }
        });
    });

    auto event_controller = Gtk::EventControllerKey::create();
    event_controller->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
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
        },
        false);
    event_controller->signal_key_released().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
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
        },
        false);
    add_controller(event_controller);

    pack_bar_start(m_footer_bar, m_alphabet_chooser);
    pack_bar_start(m_footer_bar, *Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    m_speed_value_label.set_valign(Gtk::Align::CENTER);
    m_speed_label.set_valign(Gtk::Align::CENTER);
    pack_bar_start(m_footer_bar, m_speed_label);
    pack_bar_start(m_footer_bar, m_speed_down_btn);
    pack_bar_start(m_footer_bar, m_speed_value_label);
    pack_bar_start(m_footer_bar, m_speed_up_btn);
    pack_bar_start(m_footer_bar, m_learning_label);
    pack_bar_start(m_footer_bar, m_learning_switch);
    pack_bar_start(m_footer_bar, *Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    pack_bar_start(m_footer_bar, m_speech_label);
    pack_bar_start(m_footer_bar, m_speech_switch);
    // Colour palette and canvas-font pickers moved to Preferences ->
    // Customization (RFC 0006; mirrors Dasher-Windows/Apple, which keep
    // appearance settings in Settings, not the toolbar). Widgets stay
    // members so the existing reset/refresh wiring keeps working.
    m_speech_switch.set_valign(Gtk::Align::CENTER);
    m_learning_switch.set_valign(Gtk::Align::CENTER);

    m_direct_mode = std::make_unique<DirectModeService>();
    // Mid-session injection failures (daemon died, permissions lost) are
    // reported from DirectModeService's worker thread; marshal to the GTK
    // main loop before touching widgets. The alive flag guards the race
    // where the idle is queued while the window is being destroyed: the
    // destructor clears the flag on this same (main) thread, so the check
    // inside the idle is authoritative.
    m_direct_mode->set_failure_callback([this, alive = m_ui_alive]() {
        Glib::signal_idle().connect_once([this, alive]() {
            if (!alive->load()) return;
            // Input events outrank default-priority idles, so a Retry click
            // can be dispatched between the failure notification being queued
            // and this idle running. If the service recovered in between
            // (Retry succeeded, generation moved on), the notification is
            // stale — don't turn the freshly re-enabled mode off again.
            if (m_direct_mode && m_direct_mode->is_available()) return;
            handle_keyboard_mode_failure();
        });
    });
    m_tts = std::make_unique<TtsService>();
    m_speech_switch.set_sensitive(m_tts->is_available());
    // Always explain what the button does; the setup-helper note below replaces
    // it when ydotool is missing ("I have no idea what the keyboard button
    // does" — first-impressions feedback).
    m_keyboard_button.set_tooltip_text("Keyboard mode: hide the output pane and type Dasher's text straight into "
                                       "the app you are pointing at (needs ydotool)");
    // Keyboard mode stays clickable even without ydotool: clicking it opens the
    // setup helper (issue #38) rather than being a dead greyed-out button.
    if (!m_direct_mode->is_available()) {
        m_keyboard_button.set_tooltip_text(_("Click to set up keyboard mode (needs ydotool)"));
    }
    if (!m_tts->is_available()) {
        // The switch itself is insensitive, so tooltip the (sensitive) label
        // beside it, otherwise GTK never shows the hint.
        m_speech_label.set_tooltip_text(_(
            "No speech engine available. Build with the system TTS feature, or set up a cloud voice in Preferences."));
    }

    m_keyboard_button.signal_toggled().connect([this]() {
        bool on = m_keyboard_button.get_active();
        if (on && !m_direct_mode->is_available()) {
            // ydotool missing: guide the user through installing it instead of
            // silently failing; revert until it is actually available (issue #38).
            m_keyboard_button.set_active(false);
            show_keyboard_setup_dialog();
            return;
        }
        // Capture the LEAVING mode's geometry before any layout change, so
        // per-mode bounds round-trip exactly (issue #74).
        ui::WindowGeometry leaving;
        capture_geometry(leaving); // still the old mode's size here

        m_direct_mode_active = on;
        // v5/Windows/Apple behaviour: hide the editor AND both tool bars —
        // the canvas fills the window as an on-screen keyboard, with only a
        // small floating mini bar (settings + exit) left (RFC 0015).
        m_side_panel.set_visible(!on);
        m_header_bar.set_visible(!on);
        m_footer_bar.set_visible(!on);
        m_keyboard_minibar.set_visible(on);

        // Persist the leaving mode's bounds, then restore the entering
        // mode's saved placement (where the user last parked it).
        {
            ui::UiSettings settings = ui::UiSettings::load();
            if (on)
                settings.set_window_geometry(leaving);
            else
                settings.set_keyboard_geometry(leaving);
            settings.save();
            m_ui_settings = settings;
        }
        const auto entering = on ? m_ui_settings.keyboard_geometry() : m_ui_settings.window_geometry();
        if (entering.valid) {
            apply_geometry(entering, /*live=*/true);
            // Returning to a normal-mode window that was maximized when the
            // user left it: apply_geometry dropped the (possibly keyboard-
            // mode) maximization to apply saved bounds — put it back.
            if (!on && entering.maximized) maximize();
        }

        if (on) {
            // Stay visible but never take keyboard focus, so injected text
            // lands in the user's document, not in Dasher (v5's
            // accept-focus-off + keep-above + stick, via Xlib since GTK4
            // dropped the APIs). No-op on Wayland — see RFC 0015.
            KeyboardWindowX11::engage(*this);
            KeyboardWindowX11::set_opacity(*this, m_ui_settings.keyboard_opacity());
            m_pane.set_shrink_end_child(false);
        } else {
            KeyboardWindowX11::release(*this);
            m_pane.set_shrink_end_child(true);
            m_pane.set_position(get_width() * 2 / 3);
        }
    });

    m_pane.set_start_child(m_message_overlay);
    m_pane.set_resize_start_child(true);
    m_pane.set_end_child(m_side_panel);

    // Keyboard-mode mini bar (Dasher-Windows/Apple pattern): floating
    // top-right over the canvas with settings + exit, visible only while
    // keyboard mode is on and the main bars are hidden.
    m_minibar_settings_btn.set_icon_name("settings");
    m_minibar_settings_btn.set_tooltip_text(_("Settings"));
    m_minibar_settings_btn.set_valign(Gtk::Align::START);
    m_minibar_settings_btn.signal_clicked().connect([this]() { m_preferences_window.present(); });
    m_minibar_exit_btn.set_icon_name("keyboard");
    m_minibar_exit_btn.set_tooltip_text(_("Exit keyboard mode"));
    m_minibar_exit_btn.set_valign(Gtk::Align::START);
    m_minibar_exit_btn.signal_clicked().connect([this]() { m_keyboard_button.set_active(false); });
    m_keyboard_minibar.append(m_minibar_settings_btn);
    m_keyboard_minibar.append(m_minibar_exit_btn);
    m_keyboard_minibar.set_halign(Gtk::Align::END);
    m_keyboard_minibar.set_valign(Gtk::Align::START);
    m_keyboard_minibar.set_margin(6);
    m_keyboard_minibar.set_visible(false); // keyboard mode off at start
    m_message_overlay.float_widget(m_keyboard_minibar);

    pack_bar_start(m_panel_bar, m_copy_button);
    pack_bar_start(m_panel_bar, m_copyall_button);
    pack_bar_start(m_panel_bar, m_paste_button);
    pack_bar_start(m_panel_bar, *Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    pack_bar_start(m_panel_bar, m_read_button);
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
        bool ok = true;
        if (event_type == 0) {
            ok = m_direct_mode->inject_text(text);
        } else if (event_type == 1) {
            // inject_delete counts UTF-8 characters, not bytes, so multibyte
            // output doesn't over-delete.
            ok = m_direct_mode->inject_delete(text);
        }
        if (!ok) handle_keyboard_mode_failure();
    });

    m_side_panel.append(m_panel_bar);
    m_side_panel.append(m_text_view);
    m_text_view.set_vexpand(true);
    m_text_view.set_valign(Gtk::Align::FILL);
    m_text_view.set_margin(5);
    m_text_view.add_css_class("dasher-output");

    m_paste_button.signal_clicked().connect([this]() { m_text_view.activate_action("clipboard.paste"); });
    m_copy_button.signal_clicked().connect([this]() { m_text_view.activate_action("clipboard.copy"); });
    m_copyall_button.signal_clicked().connect([this]() {
        m_text_view.activate_action("selection.select-all");
        m_text_view.activate_action("clipboard.copy");
    });

    // Analytics event wiring. Every capture() is opt-in gated inside
    // AnalyticsClient, so these can be connected unconditionally.
    m_alphabet_chooser.OnSelectionChanged.connect([](Glib::ustring id) {
        analytics::AnalyticsClient::instance().capture("alphabet_selected", {{"alphabet_id", std::string(id)}});
    });
    capture_app_launched(); // no-op unless already opted in

    // RFC 0017: passive update check for self-managed builds. Runs on a
    // background thread after the window is up (never blocks launch), at
    // most weekly. On success, shows a non-modal notification via the
    // message overlay — just a link, never a download or modal dialog.
    // Flatpak builds skip entirely (flatpak update owns updates).
    if (UpdateChecker::should_check()) {
        const std::string version = DASHER_GTK_VERSION;
        // Capture ONLY the alive token (shared_ptr keeps the atomic on the
        // heap) and the info by value — never `this`, which may be gone by
        // the time the idle callback runs. The overlay's show_message is
        // called via a raw pointer that the alive flag guards; a full fix
        // would use a weak reference to the overlay widget, but the flag
        // plus the main-thread marshalling makes the window safe: the
        // destructor sets the flag false before any widget teardown, and
        // this idle runs on the same (main) thread as the destructor.
        auto alive = m_ui_alive;
        auto* overlay = &m_message_overlay;
        std::thread([alive, overlay, version]() {
            const auto info = UpdateChecker::check(version);
            UpdateChecker::record_check();
            if (!info.available) return;
            Glib::signal_idle().connect_once([alive, overlay, info]() {
                if (!alive->load()) return;
                overlay->show_message_markup(std::string("<a href=\"") + info.release_url +
                                             "\" title=\"Open the release page\">" + "Dasher " +
                                             Glib::Markup::escape_text(info.latest_tag) + " is available</a> — " +
                                             Glib::Markup::escape_text(info.release_name));
            });
        }).detach();
    }
    maybe_show_consent_dialog(); // first launch only

    // Live typing-rate readout at the end of the footer (RFC 0012; Windows'
    // right-aligned WpmLabel). Always shows the current rolling-window rate
    // (0.0 at rest, keeping the last rate while paused, per the RFC) rather
    // than hiding; the engine keeps a 5s window and ~2 Hz matches the
    // Preferences readout.
    m_wpm_label.set_text("0.0 cps \xc2\xb7 0 wpm");
    m_wpm_label.add_css_class("dim-label");
    m_wpm_label.set_valign(Gtk::Align::CENTER);
    m_footer_bar.pack_end(m_wpm_label);

    // Game mode target overlay (Windows parity — SyncGameModeState): styled
    // label floating at the top of the canvas showing the target sentence
    // with the correct prefix, wrong text in brackets, remaining text, and
    // live WPM. Visible only while game mode is active.
    m_game_target_label.set_visible(false);
    m_game_target_label.set_halign(Gtk::Align::CENTER);
    m_game_target_label.set_valign(Gtk::Align::START);
    m_game_target_label.set_margin_top(8);
    m_game_target_label.set_use_markup(true);
    m_game_target_label.set_wrap(true);
    m_game_target_label.set_max_width_chars(60);
    m_game_target_label.set_name("GameTargetLabel");
    m_message_overlay.float_widget(m_game_target_label);
    Glib::signal_timeout().connect(
        [this]() -> bool {
            const double cps = m_canvas.bridge->get_cps();
            const double wpm = m_canvas.bridge->get_wpm();
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.1f cps \xc2\xb7 %d wpm", cps, static_cast<int>(std::lround(wpm)));
            m_wpm_label.set_text(buf);
            update_game_target();
            return true;
        },
        500);
}

void MainWindow::update_game_target() {
    if (!m_canvas.bridge->game_mode_active()) {
        m_game_target_label.set_visible(false);
        return;
    }

    const std::string target = m_canvas.bridge->game_target_text();
    const int correct = m_canvas.bridge->game_correct_count();
    const std::string wrong = m_canvas.bridge->game_wrong_text();
    const int total = m_canvas.bridge->game_target_length();

    if (target.empty() || total < 0) {
        m_game_target_label.set_visible(false);
        return;
    }

    // Build the display: green correct prefix, red wrong in brackets,
    // normal remaining, WPM on a second line. Pango markup.
    const int safe_correct = std::min(correct, static_cast<int>(target.size()));
    const std::string correct_part = Glib::Markup::escape_text(target.substr(0, safe_correct));
    const std::string wrong_part = Glib::Markup::escape_text(wrong);
    const std::string remaining = Glib::Markup::escape_text(target.substr(safe_correct));

    const double wpm = m_canvas.bridge->get_wpm();
    char wpm_line[32];
    std::snprintf(wpm_line, sizeof(wpm_line), "%d wpm", static_cast<int>(std::llround(wpm)));

    m_game_target_label.set_markup(std::string("<span foreground='#4ade80' weight='bold'>") + correct_part +
                                   "</span><span foreground='#f87171'>[" + wrong_part + "]</span>" + remaining +
                                   "\n<span size='smaller' foreground='#aaa'>" + wpm_line + "</span>");
    m_game_target_label.set_visible(true);
}

MainWindow::~MainWindow() {
    // Retire cross-thread idles before any member (or widget) teardown: the
    // DirectModeService worker's failure callback checks this flag inside its
    // queued idle, so nothing reaches a half-destroyed window afterwards.
    m_ui_alive->store(false);
}

void MainWindow::capture_app_launched() {
    analytics::AnalyticsClient::instance().capture("app_launched", {{"locale", m_canvas.bridge->get_locale()}});
}

void MainWindow::pack_bar_start(Gtk::ActionBar& bar, Gtk::Widget& child) {
    // 3px either side = ~6px between neighbours; separators a touch more so
    // groups read as groups without eating the bar's width.
    const int margin = dynamic_cast<Gtk::Separator*>(&child) ? 4 : 3;
    child.set_margin_start(margin);
    child.set_margin_end(margin);
    bar.pack_start(child);
}

void MainWindow::maybe_show_consent_dialog() {
    if (m_analytics.prompt_shown()) return;

    auto dialog = Gtk::AlertDialog::create();
    dialog->set_modal(true);
    dialog->set_message("Help improve Dasher?");
    dialog->set_detail("Share anonymous usage analytics and crash reports? No typed text, clipboard "
                       "contents or personal information is ever collected. You can change this any "
                       "time under Preferences → Privacy.");
    dialog->set_buttons({"Not now", "Help improve Dasher"});
    dialog->set_default_button(1);
    dialog->set_cancel_button(0);
    dialog->choose(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        int choice = 0;
        try {
            choice = dialog->choose_finish(result);
        } catch (const Glib::Error&) {
            choice = 0; // dismissed → treat as "not now"
        }
        const bool opted = (choice == 1);
        m_analytics.set_opted_in(opted);
        m_analytics.set_prompt_shown(true);
        m_analytics.save();
        analytics::AnalyticsClient::instance().set_opted_in(opted);
        if (opted) {
            analytics::AnalyticsClient::instance().init(m_analytics);
            analytics::AnalyticsClient::instance().capture("analytics_opted_in");
            capture_app_launched();
        }
    });
}

void MainWindow::show_keyboard_setup_dialog() {
    if (!m_keyboard_setup_dialog) {
        m_keyboard_setup_dialog = std::make_unique<KeyboardSetupDialog>(
            *this, m_direct_mode.get(), [this]() { m_keyboard_button.set_active(true); });
    }
    m_keyboard_setup_dialog->present();
}

void MainWindow::handle_keyboard_mode_failure() {
    m_direct_mode_active = false;
    // Flipping the toggle runs the normal teardown path (restores the pane).
    if (m_keyboard_button.get_active()) {
        m_keyboard_button.set_active(false);
    }
    m_message_overlay.show_message(
        std::string(_("Keyboard mode stopped: ydotool could not inject text. Is the ydotoold ")) +
        _("service running? See the setup help on the Keyboard button for install commands."));
}

double MainWindow::keyboard_opacity() const {
    return m_ui_settings.keyboard_opacity();
}

void MainWindow::nudge_speed(double delta_v5) {
    const int bitrate_key = m_canvas.bridge->find_parameter_key("LP_MAX_BITRATE");
    if (bitrate_key < 0) return;
    const double current = m_canvas.bridge->get_long_parameter(bitrate_key) / 100.0;
    double next = current + delta_v5;
    if (next < m_speed_min) next = m_speed_min;
    if (next > m_speed_max) next = m_speed_max;
    m_canvas.bridge->set_long_parameter(bitrate_key, static_cast<long>(std::lround(next * 100.0)));
    update_speed_display();
}

void MainWindow::update_speed_display() {
    const int bitrate_key = m_canvas.bridge->find_parameter_key("LP_MAX_BITRATE");
    if (bitrate_key < 0) return;
    const double v5 = m_canvas.bridge->get_long_parameter(bitrate_key) / 100.0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", v5);
    m_speed_value_label.set_text(buf);
}

void MainWindow::set_pane_layout(PaneLayout layout) {
    // Keyboard layout is just keyboard mode (which hides everything); the
    // four pane layouts re-orient the canvas/text split and leave mode alone.
    const bool keyboard = (layout == PaneLayout::Keyboard);
    if (keyboard) {
        if (!m_keyboard_button.get_active()) m_keyboard_button.set_active(true);
        m_layout_label.set_text(_("Keyboard"));
        return;
    }
    if (m_keyboard_button.get_active()) m_keyboard_button.set_active(false);

    m_pane_layout = layout;

    // The main box order is fixed (header, pane, footer) — the footer never
    // moves. Only the pane re-orients: start_child renders top (vertical) or
    // left (horizontal), so the TEXT panel (m_side_panel) goes in start for
    // Top/Left and in end for Bottom/Right. Swapping live children needs both
    // slots cleared first, or GTK4 Paned keeps stale positions.
    // Detach both children before re-seating them in the new slots; GTK
    // keeps stale packing when the same widget moves between slots live.
    gtk_paned_set_start_child(m_pane.gobj(), nullptr);
    gtk_paned_set_end_child(m_pane.gobj(), nullptr);
    switch (layout) {
    case PaneLayout::Left:
        m_pane.set_orientation(Gtk::Orientation::HORIZONTAL);
        m_pane.set_start_child(m_side_panel);
        m_pane.set_end_child(m_message_overlay);
        m_layout_label.set_text(_("Left side"));
        break;
    case PaneLayout::Bottom:
        m_pane.set_orientation(Gtk::Orientation::VERTICAL);
        m_pane.set_start_child(m_message_overlay);
        m_pane.set_end_child(m_side_panel);
        m_layout_label.set_text(_("Bottom"));
        break;
    case PaneLayout::Top:
        m_pane.set_orientation(Gtk::Orientation::VERTICAL);
        m_pane.set_start_child(m_side_panel);
        m_pane.set_end_child(m_message_overlay);
        m_layout_label.set_text(_("Top"));
        break;
    case PaneLayout::Right:
    default:
        m_pane.set_orientation(Gtk::Orientation::HORIZONTAL);
        m_pane.set_start_child(m_message_overlay);
        m_pane.set_end_child(m_side_panel);
        m_layout_label.set_text(_("Right side"));
        break;
    }
    m_pane.set_position(get_width() / 3);
    m_side_panel.set_visible(true);
}

void MainWindow::set_keyboard_opacity(double v) {
    // Read-modify-write: load the current settings (which may have been
    // changed by the Preferences window since startup), apply this one
    // change, then save. Saving a startup-time snapshot would clobber any
    // concurrent preference update — the update-check toggle was the bug.
    ui::UiSettings settings = ui::UiSettings::load();
    settings.set_keyboard_opacity(v);
    settings.save();
    // Refresh the cached value used for the live window opacity.
    m_ui_settings = settings;
    if (m_direct_mode_active) {
        KeyboardWindowX11::set_opacity(*this, v); // live, like Windows/Apple
    }
}

// ── Saved window geometry (issue #74) ─────────────────────────────────────

void MainWindow::capture_geometry(ui::WindowGeometry& g) {
    const int w = get_width();
    const int h = get_height();
    if (w < 50 || h < 50) return; // not mapped yet / degenerate
    g.w = w;
    g.h = h;
    g.valid = true;
    g.maximized = is_maximized();
    // Position via XLib (GTK4 removed gtk_window_get_position). Off X11 the
    // window keeps its size memory but not its placement — Wayland compositors
    // own positioning anyway.
    int x = 0, y = 0;
    if (WindowPlacementX11::query(*this, x, y)) {
        g.x = x;
        g.y = y;
        g.has_position = true;
    }
}

void MainWindow::save_geometry_for_current_mode() {
    ui::WindowGeometry g;
    capture_geometry(g);
    if (!g.valid) return;
    ui::UiSettings settings = ui::UiSettings::load();
    if (m_direct_mode_active)
        settings.set_keyboard_geometry(g);
    else
        settings.set_window_geometry(g);
    settings.save();
    m_ui_settings = settings;
}

bool MainWindow::geometry_on_any_monitor(const ui::WindowGeometry& g) {
    // Guard against restoring onto a disconnected monitor: require the
    // window's centre to land inside a connected monitor.
    auto display = Gdk::Display::get_default();
    if (!display) return false;
    const int cx = g.x + g.w / 2;
    const int cy = g.y + g.h / 2;
    auto monitors = display->get_monitors();
    if (!monitors) return false;
    const guint n = monitors->get_n_items();
    for (guint i = 0; i < n; i++) {
        auto monitor = std::dynamic_pointer_cast<Gdk::Monitor>(monitors->get_object(i));
        if (!monitor) continue;
        Gdk::Rectangle bounds;
        monitor->get_geometry(bounds);
        if (cx >= bounds.get_x() && cx < bounds.get_x() + bounds.get_width() && cy >= bounds.get_y() &&
            cy < bounds.get_y() + bounds.get_height())
            return true;
    }
    return false;
}

void MainWindow::apply_geometry(const ui::WindowGeometry& g, bool live) {
    if (live) {
        // A maximized window ignores configure requests — drop to floating
        // before applying saved bounds. Callers re-maximize afterwards when
        // the saved state says so (the keyboard slot never is).
        if (is_maximized()) unmaximize();
        if (g.has_position && geometry_on_any_monitor(g))
            WindowPlacementX11::move_to(*this, g.x, g.y, g.w, g.h);
        else
            WindowPlacementX11::resize(*this, g.w, g.h);
        // Off X11 (Wayland) the helpers are no-ops and live programmatic
        // resize isn't possible, but a default-size change still applies to
        // a window GTK hasn't user-resized — best effort beats silently
        // keeping the leaving mode's dimensions.
        set_default_size(g.w, g.h);
        return;
    }
    // Pre-map: default size (and the caller handles maximize).
    set_default_size(g.w, g.h);
}

bool MainWindow::on_close_request() {
    // Last chance to persist where the user left the window (issue #74).
    save_geometry_for_current_mode();
    return Gtk::Window::on_close_request(); // false = allow the close
}
