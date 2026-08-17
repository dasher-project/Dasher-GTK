// Headless reproducer for issue #42: PreferencesWindow::rebuild_sections()
// use-after-free. Drives the same path as the real trigger (Prefs → Locale →
// pick a language): the window is fully mapped and drawn FIRST, then the
// rebuild fires between real frames — the reviewer's crash needs mapped
// children (StackSidebar holds notify handlers on each stack child).
//
// Exit 0 = clean; crash = reproduced.
#include "Preferences/PreferencesWindow.h"
#include "Engine/DasherBridge.h"
#include <gtkmm/application.h>
#include <glibmm/main.h>
#include <cstdio>

// Friend of PreferencesWindow (see PreferencesWindow.h) so the selftest can
// drive the private rebuild path exactly like the locale dropdown handler.
class PrefsRebuildSelftest {
  public:
    static void trigger_locale_rebuild(PreferencesWindow& prefs, int round) {
        // Mirror the handler: set_locale() then rebuild_sections().
        prefs.m_bridge->set_locale(round % 2 == 0 ? "de" : "en");
        prefs.rebuild_sections();
    }
};

namespace {

int g_round = 0;
constexpr int kRounds = 8;

bool rebuild_round(PreferencesWindow* prefs, Glib::RefPtr<Gtk::Application> app) {
    PrefsRebuildSelftest::trigger_locale_rebuild(*prefs, g_round);
    std::printf("round %d ok\n", g_round);
    std::fflush(stdout);
    g_round++;
    if (g_round >= kRounds) {
        std::printf("SELFTEST CLEAN\n");
        app->quit();
        return false;
    }
    // Re-arm on a timeout so real frames/idles run between rounds.
    Glib::signal_timeout().connect_once(
        [prefs, app]() { Glib::signal_idle().connect([prefs, app]() -> bool { return rebuild_round(prefs, app); }); },
        50);
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create("org.dasher.gtk.prefs-selftest");

    auto bridge = std::make_shared<DasherBridge>("Data", "");
    auto prefs = std::make_unique<PreferencesWindow>(bridge, nullptr);
    PreferencesWindow* prefs_raw = prefs.get();

    app->signal_activate().connect([&, prefs_raw]() {
        app->add_window(*prefs_raw);
        prefs_raw->present();
        // Wait until the window is actually mapped + realized (the crash in
        // #42 involves teardown of mapped children), then start the chain.
        Glib::signal_idle().connect([prefs_raw, app]() -> bool {
            if (!prefs_raw->get_mapped() || !prefs_raw->get_realized()) {
                return true; // keep waiting
            }
            Glib::signal_idle().connect([prefs_raw, app]() -> bool { return rebuild_round(prefs_raw, app); });
            return false;
        });
    });

    const int rc = app->run(argc, argv);
    prefs.reset();
    return rc;
}
