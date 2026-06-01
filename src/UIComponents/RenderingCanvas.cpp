#include "RenderingCanvas.h"
#include <gdkmm/frameclock.h>
#include <gdk/gdkkeysyms.h>
#include <glibmm/datetime.h>

RenderingCanvas::RenderingCanvas() {
    set_size_request(500, 500);
    set_hexpand(true);
    set_vexpand(true);
    set_valign(Gtk::Align::FILL);
    set_halign(Gtk::Align::FILL);

    m_motion_controller = Gtk::EventControllerMotion::create();
    m_motion_controller->signal_motion().connect([this](double x, double y) {
        m_mouse_x = static_cast<float>(x);
        m_mouse_y = static_cast<float>(y);
        bridge->mouse_move(m_mouse_x, m_mouse_y);
    });
    add_controller(m_motion_controller);

    m_primary_click = Gtk::GestureClick::create();
    m_primary_click->set_button(GDK_BUTTON_PRIMARY);
    m_primary_click->signal_pressed().connect([this](int, double, double) {
        m_mouse_down = true;
        bridge->mouse_down();
    });
    m_primary_click->signal_released().connect([this](int, double, double) {
        m_mouse_down = false;
        bridge->mouse_up();
    });
    add_controller(m_primary_click);

    m_secondary_click = Gtk::GestureClick::create();
    m_secondary_click->set_button(GDK_BUTTON_SECONDARY);
    m_secondary_click->signal_pressed().connect([this](int, double, double) {
        bridge->key_event(100, 1);
    });
    m_secondary_click->signal_released().connect([this](int, double, double) {
        bridge->key_event(100, 0);
    });
    add_controller(m_secondary_click);

    m_recording_surface = Cairo::RecordingSurface::create();
    m_recording_context = Cairo::Context::create(m_recording_surface);

    bridge = std::make_shared<DasherBridge>("Data", "");
    renderer = std::make_unique<CommandRenderer>();
    input_manager = std::make_unique<InputManager>(bridge);
    input_manager->activate();

    bridge->set_output_callback([this](int event_type, const std::string& text) {
        if (event_type == 0) {
            m_output_buffer.append(text);
        } else if (event_type == 1) {
            size_t len = text.length();
            if (m_output_buffer.length() >= len &&
                m_output_buffer.compare(m_output_buffer.length() - len, len, text) == 0) {
                m_output_buffer.erase(m_output_buffer.length() - len, len);
            }
        }
        OnBufferChange.emit(m_output_buffer);
        OnOutputEvent.emit(event_type, text);
    });

    signal_resize().connect([this](int width, int height) {
        bridge->set_screen_size(width, height);
        input_manager->set_canvas_size(width, height);
    });

    set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int, int) {
        cr->set_source(m_recording_surface, 0, 0);
        cr->paint();

        m_recording_context->save();
        m_recording_context->set_operator(Cairo::Context::Operator::CLEAR);
        m_recording_context->paint();
        m_recording_context->restore();
    });

    add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
        int width = get_width();
        int height = get_height();
        if (width <= 0 || height <= 0) return true;

        DasherBridge::FrameResult result = bridge->frame(bridge->get_current_time_ms());
        if (!result.commands.empty()) {
            m_recording_context->save();
            renderer->render(result, m_recording_context);
            m_recording_context->restore();
            queue_draw();
        }
        return true;
    });
}

RenderingCanvas::~RenderingCanvas() {
    if (input_manager) {
        input_manager->deactivate();
    }
}
