#include "RenderingCanvas.h"
#include "Analytics/AnalyticsClient.h"
#include "Analytics/CrashReporter.h"
#include "Analytics/EngineLogRingBuffer.h"
#include "Analytics/PiiScrubber.h"
#include <gdkmm/frameclock.h>
#include <gdk/gdkkeysyms.h>
#include <glib.h>
#include <glibmm/datetime.h>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

RenderingCanvas::RenderingCanvas() {
    set_size_request(500, 500);
    set_hexpand(true);
    set_vexpand(true);
    set_valign(Gtk::Align::FILL);
    set_halign(Gtk::Align::FILL);

    // User-writable directory for settings, training deltas, and the
    // settings-persisted state — kept strictly separate from the read-only
    // data directory per DasherCore's CAPI contract. Without this, settings
    // were written to Data/ and lost on every rebuild or reinstall (the same
    // bug Android fixed in their #29).
    std::string user_dir;
    {
        char* dir = g_build_filename(g_get_user_data_dir(), "dasher", nullptr);
        g_mkdir_with_parents(dir, 0700);
        user_dir = dir;
        g_free(dir);
    }
    bridge = std::make_shared<DasherBridge>("Data", user_dir);

    auto locales = bridge->get_available_locales();
    auto* const* sys_langs = g_get_language_names();
    if (sys_langs) {
        for (int i = 0; sys_langs[i]; i++) {
            std::string lang(sys_langs[i]);
            bool found = false;
            for (auto& loc : locales) {
                if (loc.code == lang) {
                    bridge->set_locale(loc.code);
                    found = true;
                    break;
                }
            }
            if (found) break;
            std::string prefix = lang.substr(0, 2);
            for (auto& loc : locales) {
                if (loc.code.substr(0, 2) == prefix) {
                    bridge->set_locale(loc.code);
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }

    renderer = std::make_unique<CommandRenderer>();
    renderer->set_bridge(bridge);
    input_manager = std::make_unique<InputManager>(bridge);
    input_manager->activate();
    dwell_handler = std::make_unique<DwellClickHandler>();

    dwell_handler->signal_dwell_click().connect([this]() {
        m_mouse_down = true;
        bridge->mouse_down();
    });
    dwell_handler->signal_dwell_unclick().connect([this]() {
        m_mouse_down = false;
        bridge->mouse_up();
    });

    m_motion_controller = Gtk::EventControllerMotion::create();
    m_motion_controller->signal_motion().connect([this](double x, double y) {
        m_mouse_x = static_cast<float>(x);
        m_mouse_y = static_cast<float>(y);
        bridge->mouse_move(m_mouse_x, m_mouse_y);
        if (dwell_handler->get_enabled()) {
            dwell_handler->on_pointer_move(m_mouse_x, m_mouse_y);
        }
    });
    add_controller(m_motion_controller);

    m_primary_click = Gtk::GestureClick::create();
    m_primary_click->set_button(GDK_BUTTON_PRIMARY);
    m_primary_click->signal_pressed().connect([this](int, double, double) {
        if (dwell_handler->get_enabled()) return;
        m_mouse_down = true;
        bridge->mouse_down();
    });
    m_primary_click->signal_released().connect([this](int, double, double) {
        if (dwell_handler->get_enabled()) return;
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

    bridge->set_output_callback([this](int event_type, const std::string& text) {
        if (event_type == 0) {
            m_output_buffer.append(text);
        } else if (event_type == 1) {
            size_t len = text.length();
            if (m_output_buffer.length() >= len &&
                m_output_buffer.compare(m_output_buffer.length() - len, len, text) == 0) {
                m_output_buffer.erase(m_output_buffer.length() - len, len);
            }
        } else if (event_type == 2) {
            // Buffer cleared wholesale by the engine (reset, alphabet change —
            // DasherCore v0.2.3). Deltas can't express this; drop the shadow.
            m_output_buffer.clear();
        }
        OnBufferChange.emit(m_output_buffer);
        OnOutputEvent.emit(event_type, text);
    });

    signal_resize().connect([this](int width, int height) {
        bridge->set_screen_size(width, height);
        input_manager->set_canvas_size(width, height);
        // Fires after set_screen_size has returned, i.e. after the engine has
        // actually realised (alphabets/palettes scanned, filters registered).
        // Consumers querying permitted values on this signal can't race the
        // realisation the way a plain resize handler can.
        if (!m_engine_ready_emitted) {
            m_engine_ready_emitted = true;
            OnEngineReady.emit();
        }
    });

    set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int, int) {
        cr->set_source(m_recording_surface, 0, 0);
        cr->paint();

        m_recording_context->save();
        m_recording_context->set_operator(Cairo::Context::Operator::CLEAR);
        m_recording_context->paint();
        m_recording_context->restore();

        if (dwell_handler->get_enabled() && dwell_handler->is_active()) {
            draw_dwell_indicator(cr);
        }
    });

    add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
        int width = get_width();
        int height = get_height();
        if (width <= 0 || height <= 0) return true;

        if (dwell_handler->get_enabled()) {
            dwell_handler->on_frame();
        }

        DasherBridge::FrameResult result = bridge->frame(bridge->get_current_time_ms());
        // RFC 0009 A2: sticky error flag — report once per session as a
        // $exception (opt-in gated inside the client), carrying the scrubbed
        // engine log tail so each report is actionable rather than a
        // byte-identical "error happened" (mirrors the terminate handler).
        if (!m_engine_error_reported && bridge->has_engine_error()) {
            m_engine_error_reported = true;
            g_warning("DasherCore: sticky engine error flag set — the engine may be in an inconsistent state");
            analytics::CrashEnvelope env = analytics::CrashReporter::make_envelope("DasherEngineError", "frame_tick");
            env.stack_trace = "dasher_has_engine_error: sticky error flag set";
            env.engine_log_tail = analytics::PiiScrubber::truncate(
                analytics::PiiScrubber::scrub(analytics::engine_log_buffer().snapshot()),
                analytics::PiiScrubber::kEngineLogTailCap);
            analytics::AnalyticsClient::instance().capture_exception(env);
        }
        if (!result.commands.empty()) {
            m_recording_context->save();
            renderer->render(result, m_recording_context);
            m_recording_context->restore();
            queue_draw();
        } else if (dwell_handler->get_enabled() && dwell_handler->is_active()) {
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

void RenderingCanvas::draw_dwell_indicator(const Cairo::RefPtr<Cairo::Context>& cr) {
    float progress = dwell_handler->get_progress();
    if (progress <= 0.0f) return;

    double cx = static_cast<double>(dwell_handler->get_center_x());
    double cy = static_cast<double>(dwell_handler->get_center_y());
    double outer_radius = 25.0;

    cr->save();
    cr->set_source_rgba(0.2, 0.6, 0.8, 0.4);
    cr->set_line_width(3.0);
    cr->arc(cx, cy, outer_radius, 0.0, 2.0 * M_PI);
    cr->stroke();

    cr->set_source_rgba(0.2, 0.8, 0.9, 0.8);
    cr->set_line_width(4.0);
    cr->arc(cx, cy, outer_radius, -M_PI / 2.0, -M_PI / 2.0 + 2.0 * M_PI * static_cast<double>(progress));
    cr->stroke();
    cr->restore();
}
