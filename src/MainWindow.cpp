#include "MainWindow.h"
#include "gtkmm/enums.h"
#include <gtkmm/window.h>
#include <iostream>

MainWindow::MainWindow()
    : m_button("Hello World"), m_box(Gtk::Orientation::VERTICAL)
{
    g_setenv("GTK_CSD", "0", false);

    set_title("Dasher v6");

    // Sets the margin around the button.
    m_button.set_margin(35);

    // When the button receives the "clicked" signal, it will call the
    // on_button_clicked() method defined below.
    m_button.signal_clicked().connect([this]()
    {
        std::cout << "Hello World" << std::endl;
    });

    set_child(m_box);
    m_box.append(Canvas);
    m_box.append(m_button);
}