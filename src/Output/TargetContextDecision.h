#pragma once

#include <string>

// RFC 0015 clause 8 / RFC 0019 clause 6 — the caret-watch decision rules,
// isolated from atspi so they compile and test anywhere (the unit suite
// builds without submodules or an accessibility bus).
struct TargetContextDecision {
    // A caret read is only meaningful for a field at least this many
    // characters; below it tier-1 session context is doing the same job.
    static constexpr int kMinFieldLength = 2;

    // Events within this window after our own injection are echoes of that
    // injection — dropped (Dasher-Windows#58: "every Enter resets the
    // canvas" — our injections fire the target's own caret events).
    static constexpr int kQuietWindowMs = 150;

    // The seed text cap (RFC 0019 clause 7): the trailing window of the
    // field is what the model needs; whole documents cost a rebuild per
    // event and slow the next frame.
    static constexpr int kReadCapChars = 8192;

    // Returns true when a read should be seeded into the engine.
    static bool should_seed(int64_t now_ms, int64_t last_injection_ms, const std::string& engine_text,
                            int engine_caret_bytes, const std::string& read_text, int read_caret_bytes);
};
