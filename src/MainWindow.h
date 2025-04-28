#pragma once

#include "RenderingCanvas.h"
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>
#include <gtkmm/box.h>

class MainWindow : public Gtk::Window
{

public:
    MainWindow();

protected:

    //Member widgets:
    Gtk::Button m_button;
    Gtk::Box m_box;
    RenderingCanvas Canvas;
};