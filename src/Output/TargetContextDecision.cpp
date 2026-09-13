#include "Output/TargetContextDecision.h"

#include <algorithm>

bool TargetContextDecision::should_seed(const int64_t now_ms, const int64_t last_injection_ms,
                                        const std::string& engine_text, const int engine_caret_bytes,
                                        const std::string& read_text, const int read_caret_bytes) {
    // Guard 1: self-injection echo. Sustained dashing keeps re-arming the
    // quiet window, which is exactly right: our own output is already in
    // the engine's buffer (tier-1 session context).
    if (last_injection_ms > 0 && now_ms - last_injection_ms < kQuietWindowMs) return false;

    // Tiny FIELDS carry no useful context (measured on the untrimmed read,
    // not the sentence window — a caret right after ". " in a large document
    // has an empty window but a large field; the engine should still re-seed
    // with the empty sentence, clearing stale context from the previous one).
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
    return c == '.' || c == '!' || c == '?' || c == ';' || c == '\n' || c == '\r';
}

// A '.' followed by a digit is a decimal point, not a sentence boundary
// (URLs: "example.com", numbers: "3.14", IPs: "192.168.1.1").
// A ':' between digits is a time/ratio, not a boundary ("10:30", "16:9").
bool is_boundary_in_context(const std::string& s, const size_t i) {
    const char c = s[i];
    // '.' and ':' are boundaries only when followed by whitespace, a
    // closer (" )]), or end-of-string — "word. Next" is a boundary,
    // "example.com" and "10:30" are not.
    if (c == '.' || c == ':') {
        if (i + 1 >= s.size()) return true; // end of text → boundary
        const char next = s[i + 1];
        return next == ' ' || next == '\t' || next == '\n' || next == '\r' ||
               next == ')' || next == '"' || next == ']';
    }
    // ';' is always a boundary
    return is_sentence_boundary(c);
}

// Count UTF-8 codepoints in the byte range [from, to).
int count_codepoints(const std::string& s, const size_t from, const size_t to) {
    int n = 0;
    for (size_t i = from; i < to; ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80 || (c & 0xC0) != 0x80) ++n; // ASCII or lead byte
    }
    return n;
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

    // Scan backwards from the caret for the nearest sentence boundary,
    // counting CODEPOINTS (not bytes) toward the cap — the RFC specifies
    // ~200 UTF-16 units, and a byte cap gives CJK a 3x smaller window.
    size_t start = static_cast<size_t>(caret);
    while (start > 0) {
        if (is_boundary_in_context(utf8_text, start - 1)) break;
        --start;
        if (count_codepoints(utf8_text, start, static_cast<size_t>(caret)) >= kSentenceWindowMaxChars) break;
    }

    // UTF-8 safety: the start could have landed mid-codepoint (if the cap
    // broke the scan rather than a boundary). Snap forward to the nearest
    // lead byte.
    start = snap_to_lead_byte(utf8_text, start);

    // Skip whitespace AND boundary-adjacent closers (a `)` or `"` after a
    // period belongs to the previous sentence: "Sent one.) Next" → start
    // at "Next", not ") Next".
    while (start < static_cast<size_t>(caret)) {
        const char c = utf8_text[start];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++start; continue; }
        if ((c == ')' || c == '"') && start + 1 < static_cast<size_t>(caret)) {
            // Only skip if the NEXT char is whitespace or another closer
            // (i.e. this closer is adjacent to the boundary, not part of
            // the current sentence's content).
            const char next = utf8_text[start + 1];
            if (next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == ')' || next == '"') {
                ++start;
                continue;
            }
        }
        break;
    }

    // The window is [start, caret). The caret_offset is relative to start.
    result.text = utf8_text.substr(start, caret - start);
    result.caret_offset = caret - static_cast<int>(start);
    return result;
}
