#include "MainWindow.h"
#include "Analytics/AnalyticsClient.h"
#include "Analytics/AnalyticsSettings.h"
#include "Analytics/CrashReporter.h"
#include <gtkmm/application.h>
#ifdef _WIN32
    // for __argc & __argv
    #include <cstdlib>
#endif

int main(int argc, char* argv[])
{
    // Install crash handlers before anything else can fault, then report any
    // crash left by a previous run. Analytics is opt-in (default off), so the
    // report is only transmitted once the user has consented.
    analytics::CrashReporter::install();

    analytics::AnalyticsSettings settings = analytics::AnalyticsSettings::load();
    analytics::AnalyticsClient::instance().init(settings);
    analytics::CrashReporter::flush_pending(
        [](const analytics::CrashEnvelope& env) { analytics::AnalyticsClient::instance().capture_exception(env); });

    auto app = Gtk::Application::create("org.dasher.gtk");
    //Shows the window and returns when it is closed.
    return app->make_window_and_run<MainWindow>(argc, argv);
}

#ifdef _WIN32
    //Distinction is need to not open console window on Windows alongside the UI
    int WinMain(void* hInstance, void* hPrevInstance, char* argv, int nCmdShow)
    {
        return main(__argc, __argv);
    }
#endif