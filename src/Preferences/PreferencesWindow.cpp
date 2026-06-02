#include "PreferencesWindow.h"
#include "Output/TtsService.h"
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>

PreferencesWindow::PreferencesWindow(std::shared_ptr<DasherBridge> bridge)
    : m_bridge(bridge)
{
    set_child(m_layout);
    set_title("Dasher Preferences");
    set_default_size(600, 500);

    m_layout.append(m_sidebar);
    m_layout.append(m_stack);
    m_sidebar.set_stack(m_stack);

    rebuild_sections();
    add_locale_section();

    auto* scrolled_help = Gtk::make_managed<Gtk::ScrolledWindow>();
    auto* help_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    help_box->set_margin(12);
    auto* about = Gtk::make_managed<Gtk::Label>("");
    about->set_markup("<big><b>Dasher v6</b></big>\nGTK Edition — CAPI Architecture");
    about->set_justify(Gtk::Justification::CENTER);
    help_box->append(*about);
    scrolled_help->set_child(*help_box);
    m_stack.add(*scrolled_help, "help", "About");
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

        engine_dropdown->property_selected().signal_changed().connect(
            [tts, engines, engine_dropdown]() {
                guint idx = engine_dropdown->get_selected();
                if (idx < engines.size()) {
                    tts->set_engine(engines[idx].id);
                }
            });

        auto* voice_label = Gtk::make_managed<Gtk::Label>("Voice:");
        voice_label->set_halign(Gtk::Align::START);
        speech_box->append(*voice_label);

        auto voices = tts->get_voices();
        std::vector<Glib::ustring> voice_names;
        for (auto& v : voices) {
            voice_names.push_back(v.name.empty() ? v.id : v.name);
        }
        auto voice_list = Gtk::StringList::create(voice_names);
        auto* voice_dropdown = Gtk::make_managed<Gtk::DropDown>(voice_list);
        speech_box->append(*voice_dropdown);

        voice_dropdown->property_selected().signal_changed().connect(
            [tts, voices, voice_dropdown]() {
                guint idx = voice_dropdown->get_selected();
                if (idx < voices.size()) {
                    tts->set_voice(voices[idx].id);
                }
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
