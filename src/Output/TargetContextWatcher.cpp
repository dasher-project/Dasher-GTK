#include "Output/TargetContextWatcher.h"
#include "DirectModeService.h"
#include "Engine/DasherBridge.h"

// ── atspi plumbing ──────────────────────────────────────────────────────────
// The watcher only runs where keyboard mode does (the X11 build); atspi
// itself is backend-agnostic, but there is no direct mode to feed elsewhere.
#if defined(DASHER_HAVE_X11) && defined(DASHER_HAVE_ATSPI)

#include <atspi/atspi.h>
#include <glib.h>
#include <glibmm/main.h>
#include <sigc++/connection.h>

namespace {

int64_t mono_ms() {
    return static_cast<int64_t>(g_get_monotonic_time()) / 1000;
}

// Roles worth reading text from. Others (labels, buttons) either have no
// caret or no meaningful language-model context.
bool text_carrying_role(AtspiAccessible* accessible) {
    const AtspiRole role = atspi_accessible_get_role(accessible, nullptr);
    return role == ATSPI_ROLE_TEXT || role == ATSPI_ROLE_DOCUMENT_TEXT ||
           role == ATSPI_ROLE_ENTRY || role == ATSPI_ROLE_PARAGRAPH;
}

} // namespace

struct TargetContextWatcher::Impl {
    // atspi delivers to a plain C trampoline; this is its entry point.
    static void dispatch(AtspiEvent* event, void* self) {
        static_cast<Impl*>(self)->on_event(event);
    }

    DasherBridge* bridge = nullptr;
    DirectModeService* direct = nullptr;

    AtspiEventListener* caret_listener = nullptr;
    AtspiEventListener* focus_listener = nullptr;

    // The source of the latest qualifying event, ref'd for the debounce
    // window (the event's accessible is not guaranteed to live that long).
    AtspiAccessible* pending_source = nullptr;
    sigc::connection pending;

    ~Impl() { drop_source(); }

    void drop_source() {
        if (pending_source) {
            g_object_unref(pending_source);
            pending_source = nullptr;
        }
    }

    void on_event(AtspiEvent* event) {
        g_debug("TCW event type=%s", event && event->type ? event->type : "(null)");
        if (!bridge || !event || !event->source) return;
        if (!text_carrying_role(event->source)) return;
        drop_source();
        pending_source = ATSPI_ACCESSIBLE(g_object_ref(event->source));
        // Debounce: caret-move events arrive in bursts while the user types
        // or clicks; one read+seed at the end of the burst is enough.
        pending.disconnect();
        pending = Glib::signal_timeout().connect([this]() {
            pending.disconnect();
            read_and_seed();
            return false; // one-shot
        },
                                                 120);
    }

    void read_and_seed() {
        AtspiAccessible* source = pending_source;
        drop_source();
        g_debug("TCW read_and_seed src=%p", (void*)source);
        if (!bridge || !source) return;

        AtspiText* text = atpi_text_of(source);
        if (!text) return;

        GError* err = nullptr;
        const gint count = atspi_text_get_character_count(text, &err);
        if (err || count <= 0) {
            g_clear_error(&err);
            return;
        }
        gint caret = atspi_text_get_caret_offset(text, &err);
        if (err || caret < 0) {
            g_clear_error(&err);
            return;
        }

        // RFC 0019 clause 7: seed the trailing window, not the document.
        const gint start = std::max(0, count - TargetContextDecision::kReadCapChars);
        gchar* raw = atspi_text_get_text(text, start, count, &err);
        if (err || !raw) {
            g_clear_error(&err);
            return;
        }
        std::string window_text(raw);
        g_free(raw);

        // atspi offsets count CODEPOINTS (characters); the engine counts
        // UTF-8 bytes. Window-relative caret, converted at the boundary.
        gint rel = caret - start;
        rel = std::clamp(rel, 0, static_cast<gint>(g_utf8_strlen(window_text.c_str(), -1)));
        const int caret_bytes = DasherBridge::byte_offset_from_codepoints(window_text, static_cast<int>(rel));

        g_debug("TCW read count=%d caret=%d quiet=%lld engine_len=%zu read_len=%zu",
                (int)count, (int)caret, (long long)(direct ? direct->last_injection_ms() : -1),
                bridge->get_output_text().size(), window_text.size());
        if (TargetContextDecision::should_seed(mono_ms(), direct ? direct->last_injection_ms() : 0,
                        bridge->get_output_text(), bridge->get_offset(),
                        window_text, caret_bytes)) {
            bridge->seed_buffer(window_text, caret_bytes);
        }
    }

    static AtspiText* atpi_text_of(AtspiAccessible* accessible) {
        return accessible ? atspi_accessible_get_text(accessible) : nullptr;
    }
};

static void watcher_event_trampoline(AtspiEvent* event, void* user_data) {
    // Impl is private; the trampoline is a friend via a public forward.
    TargetContextWatcher::Impl::dispatch(event, user_data);
}

bool TargetContextWatcher::available() {
    static int state = 0; // 0 unknown, 1 yes, -1 no
    if (state == 0) {
        g_debug("TCW atspi_init...");
        // atspi_init connects to the session accessibility bus; failure is
        // expected on bare WMs / headless CI and must be non-fatal.
        state = (atspi_init() == 0) ? 1 : -1;
        g_debug("TCW atspi_init -> %d", state);
    }
    return state == 1;
}

TargetContextWatcher::~TargetContextWatcher() { stop(); }

void TargetContextWatcher::start(DasherBridge* bridge, DirectModeService* direct) {
    g_debug("TCW start called");
    if (m_impl) return; // already running
    if (!available()) return;
    m_impl = new Impl();
    m_impl->bridge = bridge;
    m_impl->direct = direct;
    m_impl->caret_listener = atspi_event_listener_new(watcher_event_trampoline, m_impl, nullptr);
    m_impl->focus_listener = atspi_event_listener_new(watcher_event_trampoline, m_impl, nullptr);
    atspi_event_listener_register(m_impl->caret_listener, "object:text-caret-moved", nullptr);
    // Tier 2: switching fields re-reads and re-seeds instead of resetting to
    // empty — the focus event drives the same debounced read.
    atspi_event_listener_register(m_impl->focus_listener, "focus:", nullptr);
}

void TargetContextWatcher::stop() {
    if (!m_impl) return;
    m_impl->pending.disconnect();
    if (m_impl->caret_listener) {
        atspi_event_listener_deregister(m_impl->caret_listener, "object:text-caret-moved", nullptr);
        g_object_unref(m_impl->caret_listener);
    }
    if (m_impl->focus_listener) {
        atspi_event_listener_deregister(m_impl->focus_listener, "focus:", nullptr);
        g_object_unref(m_impl->focus_listener);
    }
    delete m_impl;
    m_impl = nullptr;
}

#else // !DASHER_HAVE_X11

struct TargetContextWatcher::Impl {};

bool TargetContextWatcher::available() { return false; }
TargetContextWatcher::~TargetContextWatcher() { stop(); }
void TargetContextWatcher::start(DasherBridge*, DirectModeService*) {}
void TargetContextWatcher::stop() {}

#endif
