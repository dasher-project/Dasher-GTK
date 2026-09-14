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
    // The most recently FOCUSED text accessible (ref'd). Updated by the
    // focus listener on EVERY qualifying focus event — not just reads —
    // so force_reanchor reads the CURRENT target even when no caret event
    // has fired since the last field switch (greptile P1: "re-anchor uses
    // stale target"). If the app fires NO focus event at all, this is
    // stale — same gap as the event-driven path.
    AtspiAccessible* last_focused = nullptr;
    sigc::connection pending;

    ~Impl() {
        drop_source();
        clear_stale_focus();
    }

    void drop_source() {
        if (pending_source) {
            g_object_unref(pending_source);
            pending_source = nullptr;
        }
    }
    void clear_stale_focus() {
        if (last_focused) {
            g_object_unref(last_focused);
            last_focused = nullptr;
        }
    }

    void on_event(AtspiEvent* event) {
        if (!bridge || !event || !event->source) return;
        if (!text_carrying_role(event->source)) return;
        // Track the focused text accessible on EVERY qualifying event
        // (focus AND caret) — this is what force_reanchor reads from.
        clear_stale_focus();
        last_focused = ATSPI_ACCESSIBLE(g_object_ref(event->source));
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

    void read_and_seed(bool force = false) {
        // Use the source BEFORE dropping our ref. Fall back to last_focused
        // (the most recently FOCUSED text accessible — updated by every
        // qualifying event) so force_reanchor reads the current target.
        AtspiAccessible* source = pending_source ? pending_source : last_focused;
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

        // RFC 0015 sentence-window amendment (governance#40): should_seed
        // trims both sides symmetrically for the compare (see its
        // implementation). When it returns true, seed with the TRIMMED
        // sentence — the engine gets the context it needs, not the
        // formatting noise it doesn't.
        const auto read_window = TargetContextDecision::sentence_window(window_text, caret_bytes);
        if (TargetContextDecision::should_seed(mono_ms(), force ? 0 : (direct ? direct->last_injection_ms() : 0),
                                               bridge->get_output_text(), bridge->get_offset(), window_text,
                                               caret_bytes)) {
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
    // fresh context read. Uses the cached last_source (kept alive between
    // reads for exactly this case — some apps' a11y providers miss
    // same-field clicks, so no event fires and pending_source is null).
    // The quiet window is BYPASSED — the user asked for this.
    if (!m_impl) return;
    m_impl->pending.disconnect();
    m_impl->pending = Glib::signal_timeout().connect(
        [this]() {
            m_impl->pending.disconnect();
            m_impl->read_and_seed(/*force=*/true);
            return false;
        },
        10); // near-immediate
}

#else // !DASHER_HAVE_X11

void TargetContextWatcher::force_reanchor() {}

struct TargetContextWatcher::Impl {};

bool TargetContextWatcher::available() {
    return false;
}
TargetContextWatcher::~TargetContextWatcher() {
    stop();
}
void TargetContextWatcher::start(DasherBridge*, DirectModeService*) {}
void TargetContextWatcher::stop() {}

#endif
