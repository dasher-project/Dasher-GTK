#pragma once

#include "Engine/DasherBridge.h"
#include "Engine/CommandRenderer.h"
#include "Input/InputManager.h"
#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <memory>

class RenderingCanvas : public Gtk::DrawingArea {
public:
    RenderingCanvas();
    ~RenderingCanvas() override;

    std::shared_ptr<DasherBridge> bridge;
    std::unique_ptr<CommandRenderer> renderer;
    std::unique_ptr<InputManager> input_manager;

    sigc::signal<void(const std::string&)> OnBufferChange;

private:
    Glib::RefPtr<Gtk::EventControllerMotion> m_motion_controller;
    Glib::RefPtr<Gtk::GestureClick> m_primary_click;
    Glib::RefPtr<Gtk::GestureClick> m_secondary_click;

    float m_mouse_x = 0.0f;
    float m_mouse_y = 0.0f;
    bool m_mouse_down = false;

    Cairo::RefPtr<Cairo::RecordingSurface> m_recording_surface;
    Cairo::RefPtr<Cairo::Context> m_recording_context;

    std::string m_output_buffer;
};
