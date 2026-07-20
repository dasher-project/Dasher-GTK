#include "PreferencesWindow.h"
#include "Analytics/AnalyticsClient.h"
#include "Output/TtsService.h"
#include "Input/DwellClickHandler.h"
#include "UIComponents/PopoverMenuButtonInfo.h"
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/scale.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/switch.h>
#include <gtkmm/label.h>
#include <functional>
#include <sstream>
#include <algorithm>

static std::vector<std::string> parse_json_keys(const std::string& json) {
    std::vector<std::string> keys;
    if (json.size() < 2 || json[0] != '[') return keys;
    std::string inner = json.substr(1, json.size() - 2);
    std::stringstream ss(inner);
    std::string token;
    while (std::getline(ss, token, ',')) {
        size_t start = token.find('"');
        size_t end = token.rfind('"');
        if (start != std::string::npos && end > start) {
            keys.push_back(token.substr(start + 1, end - start - 1));
        }
    }
    return keys;
}

static std::string build_credentials_json(
    const std::vector<std::string>& keys,
    const std::vector<Gtk::Entry*>& entries)
{
    std::string json = "{";
    for (size_t i = 0; i < keys.size() && i < entries.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + keys[i] + "\":\"" + entries[i]->get_text() + "\"";
    }
    json += "}";
    return json;
}

static std::string prettify_key(const std::string& key) {
    std::string result;
    for (size_t i = 0; i < key.size(); i++) {
        if (i > 0 && std::isupper(key[i])) result += ' ';
        result += key[i];
    }
    if (!result.empty()) result[0] = std::toupper(result[0]);
    return result;
}

PreferencesWindow::PreferencesWindow(std::shared_ptr<DasherBridge> bridge, DwellClickHandler* dwell_handler)
    : m_bridge(bridge), m_dwell_handler(dwell_handler) {
    set_child(m_layout);
    set_title("Dasher Preferences");
    set_default_size(600, 500);

    m_layout.append(m_sidebar);
    m_layout.append(m_stack);
    m_sidebar.set_stack(m_stack);

    rebuild_sections();
    add_locale_section();
    add_privacy_section();

    auto* scrolled_help = Gtk::make_managed<Gtk::ScrolledWindow>();
    auto* help_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    help_box->set_margin(12);
    auto* about = Gtk::make_managed<Gtk::Label>("");
    about->set_markup("<big><b>Dasher v6</b></big>\nGTK Edition — CAPI Architecture");
    about->set_justify(Gtk::Justification::CENTER);
    help_box->append(*about);
    scrolled_help->set_child(*help_box);
    m_stack.add(*scrolled_help, "help", "About");

    // Record which settings tab the user opens (opt-in gated in AnalyticsClient).
    m_stack.property_visible_child().signal_changed().connect([this]() {
        Gtk::Widget* child = m_stack.get_visible_child();
        if (!child) return;
        auto page = m_stack.get_page(*child);
        if (!page) return;
        analytics::AnalyticsClient::instance().capture("settings_viewed",
                                                       {{"tab_name", std::string(page->get_title())}});
    });
}

void PreferencesWindow::rebuild_sections() {
    for (auto* w : m_dynamic_pages) {
        m_stack.remove(*w);
    }
    m_dynamic_pages.clear();

    struct GroupDef {
        std::string id;
        std::string label;
        std::string group;
    };

    std::vector<GroupDef> groups = {
        {"customization", "Customization", "Customization"},
        {"input", "Input", "Input"},
        {"language", "Language", "Language"},
        {"output", "Output", "Output"},
        {"gamemode", "Game Mode", "Game Mode"},
    };

    for (auto& g : groups) {
        auto* scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
        auto* section = Gtk::make_managed<SettingsSection>(g.label, m_bridge, g.group);
        // Dwell-to-click is a GTK-side input mode (no backing DasherCore parameter,
        // so SettingsSection can't auto-render it). Hand-build its toggle here so it
        // lives under Input alongside the engine's input settings, instead of in the
        // footer bar (issue #35). The row mirrors SettingsSection's layout.
        if (g.id == "input" && m_dwell_handler) {
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            row->set_margin_top(4);
            row->set_margin_bottom(4);

            auto* name_label = Gtk::make_managed<Gtk::Label>("Dwell to click");
            name_label->set_halign(Gtk::Align::START);
            name_label->set_hexpand(true);
            row->append(*name_label);

            auto* dwell_switch = Gtk::make_managed<Gtk::Switch>();
            dwell_switch->set_valign(Gtk::Align::CENTER);
            dwell_switch->set_active(m_dwell_handler->get_enabled());
            dwell_switch->property_active().signal_changed().connect(
                [this, dwell_switch]() { m_dwell_handler->set_enabled(dwell_switch->get_active()); });
            row->append(*dwell_switch);

            row->append(*Gtk::make_managed<PopoverMenuButtonInfo>(
                "Hover in one spot to trigger a click, instead of pressing a button or switch."));

            section->append(*row);
        }
        scrolled->set_child(*section);
        scrolled->set_vexpand(true);
        scrolled->set_hexpand(true);
        m_stack.add(*scrolled, g.id, g.label);
        m_dynamic_pages.push_back(scrolled);
    }

    auto* scrolled_speech = Gtk::make_managed<Gtk::ScrolledWindow>();
    auto* speech_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    speech_box->set_margin(12);

    auto* speech_title = Gtk::make_managed<Gtk::Label>("");
    speech_title->set_markup("<b>Speech Settings</b>");
    speech_title->set_halign(Gtk::Align::START);
    speech_box->append(*speech_title);

    auto tts = std::make_shared<TtsService>();
    if (tts->is_available()) {
        auto engines = tts->get_engines();
        std::vector<Glib::ustring> engine_names;
        for (auto& e : engines) {
            engine_names.push_back(e.name.empty() ? e.id : e.name);
        }

        auto* engine_label = Gtk::make_managed<Gtk::Label>("TTS Engine:");
        engine_label->set_halign(Gtk::Align::START);
        speech_box->append(*engine_label);

        auto engine_list = Gtk::StringList::create(engine_names);
        auto* engine_dropdown = Gtk::make_managed<Gtk::DropDown>(engine_list);
        speech_box->append(*engine_dropdown);

        auto* creds_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        speech_box->append(*creds_box);

        auto cred_keys = std::make_shared<std::vector<std::string>>();
        auto cred_entries = std::make_shared<std::vector<Gtk::Entry*>>();

        auto* voice_label = Gtk::make_managed<Gtk::Label>("Voice:");
        voice_label->set_halign(Gtk::Align::START);
        speech_box->append(*voice_label);

        auto voice_list = Gtk::StringList::create({});
        auto* voice_dropdown = Gtk::make_managed<Gtk::DropDown>(voice_list);
        speech_box->append(*voice_dropdown);

        auto refresh_voices = [tts, voice_list]() {
            while (voice_list->get_n_items() > 0)
                voice_list->remove(0);
            auto voices = tts->get_voices();
            for (auto& v : voices) {
                voice_list->append(v.name.empty() ? v.id : v.name);
            }
            if (!voices.empty()) {
                tts->set_voice(voices[0].id);
            }
        };

        auto rebuild_creds = [creds_box, cred_keys, cred_entries, tts, engines, engine_dropdown, refresh_voices]() {
            while (creds_box->get_first_child() != nullptr) {
                auto* child = creds_box->get_first_child();
                creds_box->remove(*child);
            }
            cred_keys->clear();
            cred_entries->clear();

            guint idx = engine_dropdown->get_selected();
            if (idx >= engines.size()) return;

            auto& engine = engines[idx];
            auto keys = parse_json_keys(engine.credential_keys_json);

            if (keys.empty()) {
                tts->set_engine(engine.id);
                refresh_voices();
                return;
            }

            auto* title = Gtk::make_managed<Gtk::Label>("");
            title->set_markup("<b>Credentials</b>");
            title->set_halign(Gtk::Align::START);
            creds_box->append(*title);

            for (auto& key : keys) {
                auto* lbl = Gtk::make_managed<Gtk::Label>(prettify_key(key) + ":");
                lbl->set_halign(Gtk::Align::START);
                creds_box->append(*lbl);

                auto* entry = Gtk::make_managed<Gtk::Entry>();
                entry->set_hexpand(true);
                entry->set_visibility(false);
                creds_box->append(*entry);
                cred_keys->push_back(key);
                cred_entries->push_back(entry);
            }

            auto* connect_btn = Gtk::make_managed<Gtk::Button>("Connect");
            connect_btn->set_halign(Gtk::Align::START);
            connect_btn->set_margin_top(4);
            creds_box->append(*connect_btn);

            connect_btn->signal_clicked().connect(
                [tts, engine, cred_keys, cred_entries, refresh_voices]() {
                    auto json = build_credentials_json(*cred_keys, *cred_entries);
                    tts->set_engine(engine.id, json);
                    refresh_voices();
                });
        };

        engine_dropdown->property_selected().signal_changed().connect(
            [rebuild_creds]() {
                rebuild_creds();
            });

        voice_dropdown->property_selected().signal_changed().connect(
            [tts, voice_dropdown]() {
                auto voices = tts->get_voices();
                guint idx = voice_dropdown->get_selected();
                if (idx < voices.size()) {
                    tts->set_voice(voices[idx].id);
                }
            });

        rebuild_creds();

        auto add_slider = [&](const Glib::ustring& label_text, float min, float max, float def, float step,
                              std::function<void(float)> setter) {
            auto* lbl = Gtk::make_managed<Gtk::Label>(label_text);
            lbl->set_halign(Gtk::Align::START);
            speech_box->append(*lbl);

            auto adj = Gtk::Adjustment::create(def, min, max, step, step * 5, 0);
            auto* scale = Gtk::make_managed<Gtk::Scale>(adj, Gtk::Orientation::HORIZONTAL);
            scale->set_hexpand(true);
            scale->set_digits(1);
            scale->set_draw_value(true);
            speech_box->append(*scale);

            adj->signal_value_changed().connect([adj, setter = std::move(setter)]() {
                setter(static_cast<float>(adj->get_value()));
            });
        };

        add_slider("Rate:", 0.1, 3.0, 1.0, 0.1, [tts](float v) { tts->set_rate(v); });
        add_slider("Pitch:", 0.1, 3.0, 1.0, 0.1, [tts](float v) { tts->set_pitch(v); });
        add_slider("Volume:", 0.0, 2.0, 1.0, 0.1, [tts](float v) { tts->set_volume(v); });

        auto* preview_btn = Gtk::make_managed<Gtk::Button>("Test Voice");
        preview_btn->set_halign(Gtk::Align::START);
        preview_btn->set_margin_top(8);
        speech_box->append(*preview_btn);

        preview_btn->signal_clicked().connect([tts]() {
            tts->stop();
            tts->speak("Hello, this is a test of the text to speech engine.");
        });
    } else {
        auto* no_tts = Gtk::make_managed<Gtk::Label>("No TTS engine available.\nInstall speech-dispatcher for system TTS.");
        no_tts->set_halign(Gtk::Align::START);
        speech_box->append(*no_tts);
    }

    scrolled_speech->set_child(*speech_box);
    m_stack.add(*scrolled_speech, "speech", "Speech");
    m_dynamic_pages.push_back(scrolled_speech);
}

void PreferencesWindow::add_locale_section() {
    auto* scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(12);

    auto* title = Gtk::make_managed<Gtk::Label>("");
    title->set_markup("<b>Language / Locale</b>");
    title->set_halign(Gtk::Align::START);
    box->append(*title);

    auto locales = m_bridge->get_available_locales();
    std::string current = m_bridge->get_locale();

    std::vector<Glib::ustring> names;
    guint selected_idx = 0;
    for (size_t i = 0; i < locales.size(); i++) {
        names.push_back(locales[i].display_name + " (" + locales[i].code + ")");
        if (locales[i].code == current) {
            selected_idx = static_cast<guint>(i);
        }
    }

    auto list = Gtk::StringList::create(names);
    auto* dropdown = Gtk::make_managed<Gtk::DropDown>(list);
    dropdown->set_selected(selected_idx);
    box->append(*dropdown);

    dropdown->property_selected().signal_changed().connect([this, dropdown, locales]() {
        guint idx = dropdown->get_selected();
        if (idx < locales.size()) {
            m_bridge->set_locale(locales[idx].code);
            rebuild_sections();
        }
    });

    scrolled->set_child(*box);
    m_stack.add(*scrolled, "locale", "Locale");
}

void PreferencesWindow::add_privacy_section() {
    auto* scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(12);

    auto* title = Gtk::make_managed<Gtk::Label>("");
    title->set_markup("<b>Privacy</b>");
    title->set_halign(Gtk::Align::START);
    box->append(*title);

    auto* blurb = Gtk::make_managed<Gtk::Label>("Help improve Dasher by sharing anonymous usage analytics and crash "
                                                "reports. No typed text, clipboard contents or personal information is "
                                                "ever collected, and reports are only sent if you opt in.");
    blurb->set_halign(Gtk::Align::START);
    blurb->set_wrap(true);
    blurb->set_xalign(0.0f);
    box->append(*blurb);

    auto* opt_in = Gtk::make_managed<Gtk::CheckButton>("Share anonymous analytics and crash reports");
    opt_in->set_active(m_analytics.opted_in());
    box->append(*opt_in);

    auto* reset_btn = Gtk::make_managed<Gtk::Button>("Reset analytics ID");
    reset_btn->set_halign(Gtk::Align::START);
    reset_btn->set_sensitive(m_analytics.opted_in());
    box->append(*reset_btn);

    opt_in->signal_toggled().connect([this, opt_in, reset_btn]() {
        bool on = opt_in->get_active();
        m_analytics.set_opted_in(on);
        m_analytics.set_prompt_shown(true);
        m_analytics.save();
        analytics::AnalyticsClient::instance().set_opted_in(on);
        reset_btn->set_sensitive(on);
        if (on) {
            // Materialise the anonymous id and record the consent event.
            analytics::AnalyticsClient::instance().init(m_analytics);
            analytics::AnalyticsClient::instance().capture("analytics_opted_in");
        }
    });

    reset_btn->signal_clicked().connect([this]() {
        m_analytics.reset_anonymous_id();
        analytics::AnalyticsClient::instance().init(m_analytics);
        analytics::AnalyticsClient::instance().capture("analytics_id_reset");
    });

    scrolled->set_child(*box);
    m_stack.add(*scrolled, "privacy", "Privacy");
}
