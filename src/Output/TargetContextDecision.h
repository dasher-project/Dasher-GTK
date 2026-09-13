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

    // RFC 0015 sentence-window amendment (governance#40): seed (and compare)
    // the SENTENCE around the caret, not the full document. Full-document
    // reads from complex editors return inconsistent results (signatures,
    // formatting, timing) that break the shadow-compare → visible canvas
    // reset on every keystroke. The sentence window is small, stable, and
    // grows in lockstep with the engine buffer during typing.
    static constexpr int kSentenceWindowMaxChars = 200; // codepoints, matching the RFC's ~200 UTF-16 units

    // Returns true when a read should be seeded into the engine.
    static bool should_seed(int64_t now_ms, int64_t last_injection_ms, const std::string& engine_text,
                            int engine_caret_bytes, const std::string& read_text, int read_caret_bytes);

    // ── Sentence-window trimming (governance#40) ──
    //
    // Returns the sentence around the caret: from the last sentence
    // boundary (`.`, `!`, `?`, `;`, `:`, `\n`, with `)` and `"` treated as
    // boundary-adjacent) backwards, capped at kSentenceWindowMaxChars
    // UTF-8 bytes. The output is a pair: {text, caret_byte_offset_in_text}.
    //
    // Both the TARGET READ and the ENGINE BUFFER pass through this before
    // the shadow-compare (symmetric trimming) — without it, typing a
    // sentence terminator breaks lockstep: the engine grows to include
    // the period while the window trims to empty.
    //
    // UTF-8 safe: boundaries are single-byte ASCII; the byte cap can land
    // mid-codepoint and is nudged to the nearest lead byte.
    struct SentenceWindow {
        std::string text;
        int caret_offset; // byte offset of the caret within `text`
    };
    static SentenceWindow sentence_window(const std::string& utf8_text, int caret_byte_offset);

    // Convenience overload: caret at end of text (common for reads where
    // the caret position wasn't available).
    static SentenceWindow sentence_window(const std::string& utf8_text) {
        return sentence_window(utf8_text, static_cast<int>(utf8_text.size()));
    }
};
