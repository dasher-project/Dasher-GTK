#include "MainWindow.h"
#include "Analytics/AnalyticsClient.h"
#include "Analytics/AnalyticsSettings.h"
#include "Analytics/CrashReporter.h"
#include <gtkmm/application.h>
#ifdef _WIN32
    // for __argc & __argv
    #include <cstdlib>
#endif

#ifdef ENABLE_NLS
#include <libintl.h>
#include <locale.h>
#include <climits>
#include <unistd.h>
#include <string>
#else
// Stubs so unwrapped builds still compile with _() in the sources.
#define _(String) (String)
#define gettext(String) (String)
#endif

    int main(int argc, char* argv[]) {
        // RFC 0003: bind the gettext domain before any widget is built, so the
        // UI chrome localises alongside the engine strings (which go through
        // dasher_set_locale). bindtextdomain keeps ONE binding per domain (the
        // last call wins — it does not accumulate fallbacks), so pick the
        // first directory that actually contains a catalog:
        //   1. build tree: <binary>/../po/ (dev runs from build/Dasher/)
        //   2. AppImage/portable install: <binary>/../share/locale/
        //   3. compile-time LOCALEDIR (system installs)
#ifdef ENABLE_NLS
        setlocale(LC_ALL, "");
        {
            std::string domain_dir;
            char exe[PATH_MAX];
            const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            if (n > 0) {
                exe[n] = '\0';
                std::string bin_dir(exe);
                const auto slash = bin_dir.find_last_of('/');
                if (slash != std::string::npos) {
                    bin_dir.resize(slash);
                    // Dev: build tree catalogs at build/po/<locale>/LC_MESSAGES/
                    const std::string build_po = bin_dir + "/../po";
                    const std::string po_catalog = build_po + "/fr/LC_MESSAGES/" GETTEXT_PACKAGE ".mo";
                    if (access(po_catalog.c_str(), R_OK) == 0) {
                        domain_dir = build_po;
                    }
                    // AppImage / portable: <binary>/../share/locale/<locale>/LC_MESSAGES/
                    if (domain_dir.empty()) {
                        const std::string share_locale = bin_dir + "/../share/locale";
                        const std::string share_catalog = share_locale + "/fr/LC_MESSAGES/" GETTEXT_PACKAGE ".mo";
                        if (access(share_catalog.c_str(), R_OK) == 0) {
                            domain_dir = share_locale;
                        }
                    }
                }
            }
            // Compile-time install prefix (system installs, Flatpak)
            if (domain_dir.empty()) {
                domain_dir = LOCALEDIR;
            }
            bindtextdomain(GETTEXT_PACKAGE, domain_dir.c_str());
            bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
            textdomain(GETTEXT_PACKAGE);
        }
#endif

        // Install crash handlers before anything else can fault, then report any
        // crash left by a previous run. Analytics is opt-in (default off), so the
        // report is only transmitted once the user has consented.
        analytics::CrashReporter::install();

        analytics::AnalyticsSettings settings = analytics::AnalyticsSettings::load();
        analytics::AnalyticsClient::instance().init(settings);
        analytics::CrashReporter::flush_pending(
            [](const analytics::CrashEnvelope& env) { analytics::AnalyticsClient::instance().capture_exception(env); });

        auto app = Gtk::Application::create("org.dasher.gtk");
        // Shows the window and returns when it is closed.
        return app->make_window_and_run<MainWindow>(argc, argv);
    }

#ifdef _WIN32
    //Distinction is need to not open console window on Windows alongside the UI
    int WinMain(void* hInstance, void* hPrevInstance, char* argv, int nCmdShow)
    {
        return main(__argc, __argv);
    }
#endif