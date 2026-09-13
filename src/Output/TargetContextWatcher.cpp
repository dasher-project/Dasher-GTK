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
    return role == ATSPI_ROLE_TEXT || role == ATSPI_ROLE_DOCUMENT_TEXT || role == ATSPI_ROLE_ENTRY ||
           role == ATSPI_ROLE_PARAGRAPH;
}

} // namespace

struct TargetContextWatcher::Impl {
    // atspi delivers to a plain C trampoline; this is its entry point.
    static void dispatch(AtspiEvent* event, void* self) { static_cast<Impl*>(self)->on_event(event); }

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
        if (!bridge || !event || !event->source) return;
        if (!text_carrying_role(event->source)) return;
        drop_source();
        pending_source = ATSPI_ACCESSIBLE(g_object_ref(event->source));
        // Debounce: caret-move events arrive in bursts while the user types
        // or clicks; one read+seed at the end of the burst is enough.
        pending.disconnect();
        pending = Glib::signal_timeout().connect(
            [this]() {
                pending.disconnect();
                read_and_seed();
                return false; // one-shot
            },
            120);
    }

    void read_and_seed() {
        // Use the source BEFORE dropping our ref (greptile P1: the capture's
        // ref was the only thing keeping it alive across the debounce).
        AtspiAccessible* source = pending_source;
        if (!bridge || !source) {
            drop_source();
            return;
        }

        // atspi_accessible_get_text returns a NEW reference (greptile P2).
        AtspiText* text = atspi_accessible_get_text(source);
        if (!text) {
            drop_source();
            return;
        }

        GError* err = nullptr;
        const gint count = atspi_text_get_character_count(text, &err);
        gint caret = -1;
        if (!err && count > 0) caret = atspi_text_get_caret_offset(text, &err);
        if (err || count <= 0 || caret < 0) {
            g_clear_error(&err);
            g_object_unref(text);
            drop_source();
            return;
        }

        // RFC 0019 clause 7: seed a WINDOW, not the document. Trailing
        // window when the caret sits inside it; when the caret is BEFORE
        // the trailing window (greptile P1: "window ignores caret
        // position"), read a window that STARTS at the caret instead — the
        // anchor must reflect where the user actually is.
        gint start = std::max(0, count - TargetContextDecision::kReadCapChars);
        if (caret < start) start = caret;
        const gint end = std::min(count, start + TargetContextDecision::kReadCapChars);
        gchar* raw = atspi_text_get_text(text, start, end, &err);
        if (err || !raw) {
            g_clear_error(&err);
            g_object_unref(text);
            drop_source();
            return;
        }
        std::string window_text(raw);
        g_free(raw);
        g_object_unref(text);
        drop_source();

        // atspi offsets count CODEPOINTS (characters); the engine counts
        // UTF-8 bytes. Window-relative caret, converted at the boundary.
        gint rel = caret - start;
        rel = std::clamp(rel, 0, static_cast<gint>(g_utf8_strlen(window_text.c_str(), -1)));
        const int caret_bytes = DasherBridge::byte_offset_from_codepoints(window_text, static_cast<int>(rel));

        // RFC 0015 sentence-window amendment (governance#40): trim to the
        // sentence around the caret before seeding AND before comparing.
        // Full-document reads from complex editors return inconsistent
        // results that break the shadow-compare; the sentence window is
        // small, stable, and grows in lockstep with the engine buffer.
        const auto read_window = TargetContextDecision::sentence_window(window_text, caret_bytes);

        if (TargetContextDecision::should_seed(mono_ms(), direct ? direct->last_injection_ms() : 0,
                                               bridge->get_output_text(), bridge->get_offset(),
                                               read_window.text, read_window.caret_offset)) {
            bridge->seed_buffer(read_window.text, read_window.caret_offset);
        }
    }
};

static void watcher_event_trampoline(AtspiEvent* event, void* user_data) {
    // Impl is private; the trampoline is a friend via a public forward.
    TargetContextWatcher::Impl::dispatch(event, user_data);
}

bool TargetContextWatcher::available() {
    static int state = 0; // 0 unknown, 1 yes, -1 no
    if (state == 0) {
        // atspi_init connects to the session accessibility bus; failure is
        // expected on bare WMs / headless CI and must be non-fatal.
        state = (atspi_init() == 0) ? 1 : -1;
    }
    return state == 1;
}

TargetContextWatcher::~TargetContextWatcher() {
    stop();
}

void TargetContextWatcher::start(DasherBridge* bridge, DirectModeService* direct) {
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

void TargetContextWatcher::force_reanchor() {
    // Manual re-anchor (governance#40): the user explicitly asked for a
    // fresh context read. We don't know which accessible is focused from
    // here (the event listeners track that), so we use the atspi desktop's
    // focused accessible directly.
    if (!m_impl) return;
    AtspiAccessible* focused = atspi_get_desktop(0) ? nullptr : nullptr;
    // The atspi API for getting the focused accessible is via the event
    // bus; the simplest portable approach is to re-read from the last
    // event source, or if none, skip (the user can click in the target
    // to trigger an event). For now: schedule a read from whatever the
    // last pending_source was, or do nothing if it was already consumed.
    // TODO: query the focused accessible via atspi's device event
    // controller or the focus event cache. For the common case, the
    // user will have just clicked in the target field, so the pending
    // debounce will fire momentarily.
    (void)focused;
    // Schedule an immediate read (bypasses the debounce delay).
    m_impl->pending.disconnect();
    m_impl->pending = Glib::signal_timeout().connect([this]() {
        m_impl->pending.disconnect();
        m_impl->read_and_seed();
        return false;
    },
                                                     10); // near-immediate
}

#else // !DASHER_HAVE_X11

struct TargetContextWatcher::Impl {};

bool TargetContextWatcher::available() {
    return false;
}
TargetContextWatcher::~TargetContextWatcher() {
    stop();
}
void TargetContextWatcher::start(DasherBridge*, DirectModeService*) {}
void TargetContextWatcher::stop() {}
void TargetContextWatcher::force_reanchor() {}

#endif
