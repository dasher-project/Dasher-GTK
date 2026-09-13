#include "Output/TargetContextDecision.h"

#include <algorithm>

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

    // Guard 2: shadow-compare skip with SYMMETRIC sentence-window trimming
    // (governance#40): both sides go through the same window function so
    // the comparison is stable even when the full document reads vary
    // (signatures, formatting, CRLF divergence).
    const auto engine_w = sentence_window(engine_text, engine_caret_bytes);
    const auto read_w = sentence_window(read_text, read_caret_bytes);
    return engine_w.text != read_w.text || engine_w.caret_offset != read_w.caret_offset;
}

// ── Sentence-window trimming ────────────────────────────────────────────────

namespace {

bool is_sentence_boundary(const char c) {
    return c == '.' || c == '!' || c == '?' || c == ';' || c == ':' || c == '\n' || c == '\r';
}

// True if the byte at position i in utf8_text is a UTF-8 lead byte (i.e. the
// start of a codepoint). Continuation bytes are 0b10xxxxxx; leads are
// everything else above 0x7F, plus all ASCII.
bool is_lead_byte(const std::string& s, const size_t i) {
    if (i >= s.size()) return false;
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return true;                        // ASCII
    if ((c & 0xC0) == 0x80) return false;             // continuation
    return true;                                       // lead byte (C0-F4)
}

// Nudge a byte offset forward to the nearest lead byte (max 3 nudges for a
// 4-byte sequence).
size_t snap_to_lead_byte(const std::string& s, size_t offset) {
    while (offset < s.size() && !is_lead_byte(s, offset)) ++offset;
    return offset;
}

} // namespace

TargetContextDecision::SentenceWindow TargetContextDecision::sentence_window(const std::string& utf8_text,
                                                                             const int caret_byte_offset) {
    SentenceWindow result;

    // Clamp the caret into the string.
    int caret = caret_byte_offset;
    if (caret < 0) caret = 0;
    if (caret > static_cast<int>(utf8_text.size())) caret = static_cast<int>(utf8_text.size());

    // Scan backwards from the caret for the nearest sentence boundary.
    // The boundary character itself is NOT included in the window (it
    // belongs to the previous sentence).
    size_t start = static_cast<size_t>(caret);
    while (start > 0) {
        const char c = utf8_text[start - 1];
        if (is_sentence_boundary(c)) break;
        --start;
        // Cap the backward scan at kSentenceWindowMaxChars bytes.
        if (caret - static_cast<int>(start) >= kSentenceWindowMaxChars) break;
    }

    // UTF-8 safety: the start could have landed mid-codepoint (if the cap
    // broke the scan rather than a boundary). Snap forward to the nearest
    // lead byte.
    start = snap_to_lead_byte(utf8_text, start);

    // Skip whitespace between the boundary and the sentence text (a
    // boundary is typically followed by one or more spaces / a newline
    // before the next word). Without this, "Previous. Hello" trims to
    // " Hello" with a leading space that makes the shadow-compare fail
    // against an engine buffer that never had it.
    while (start < static_cast<size_t>(caret) &&
           (utf8_text[start] == ' ' || utf8_text[start] == '\t' || utf8_text[start] == '\n')) {
        ++start;
    }

    // The window is [start, caret]. The caret_offset is relative to start.
    result.text = utf8_text.substr(start, caret - start);
    result.caret_offset = caret - static_cast<int>(start);
    return result;
}
