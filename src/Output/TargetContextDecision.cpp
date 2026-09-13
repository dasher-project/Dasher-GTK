#include "Output/TargetContextDecision.h"

bool TargetContextDecision::should_seed(const int64_t now_ms, const int64_t last_injection_ms,
                                        const std::string& engine_text, const int engine_caret_bytes,
                                        const std::string& read_text, const int read_caret_bytes) {
    // Guard 1: self-injection echo. Sustained dashing keeps re-arming the
    // quiet window, which is exactly right: our own output is already in
    // the engine's buffer (tier-1 session context).
    if (last_injection_ms > 0 && now_ms - last_injection_ms < kQuietWindowMs) return false;

    // Tiny fields carry no useful context; tier-1 already covers them and a
    // seed resets the canvas for nothing.
    if (static_cast<int>(read_text.size()) < kMinFieldLength) return false;

    // Guard 2: shadow-compare skip. Identical text + caret means the seed is
    // a model no-op but a full node-tree rebuild — the visible "canvas
    // resets even when you don't want it to".
    return engine_text != read_text || engine_caret_bytes != read_caret_bytes;
}
