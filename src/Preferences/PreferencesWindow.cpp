#include "PreferencesWindow.h"

PreferencesWindow::PreferencesWindow(std::shared_ptr<DasherBridge> bridge)
    : m_bridge(bridge)
{
    set_child(m_layout);
    set_title("Dasher Preferences");
    set_default_size(600, 500);

    m_layout.append(m_sidebar);
    m_layout.append(m_stack);
    m_sidebar.set_stack(m_stack);

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
    }

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
