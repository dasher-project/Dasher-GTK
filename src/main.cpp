#include "MainWindow.h"
#include <gtkmm/application.h>
#ifdef _WIN32
    // for __argc & __argv
    #include <cstdlib>
#endif

int main(int argc, char* argv[])
{
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