// RFC 0015 clause 8 / RFC 0019 clause 6 — the caret-watch decision logic:
// the two guards the Windows experience (Dasher-Windows#58) demands, plus
// the tier rules. Pure function — no atspi bus needed in CI.
#include "Output/TargetContextDecision.h"

#include <doctest/doctest.h>

using T = TargetContextDecision;

TEST_CASE("self-injection echo is dropped") {
    // Our injection 50ms ago: the target's caret event is our own echo —
    // seeding would reset the canvas mid-dash (the Outlook Enter bug).
    CHECK_FALSE(T::should_seed(/*now*/ 10'000, /*last_inject*/ 9'950, "old", 0, "new text", 4));
}

TEST_CASE("events after the quiet window pass") {
    CHECK(T::should_seed(10'000, 10'000 - T::kQuietWindowMs - 1, "old", 0, "new text", 4));
}

TEST_CASE("never-injected events pass") {
    CHECK(T::should_seed(10'000, 0, "", 0, "hello notes app", 5));
}

TEST_CASE("tiny fields are skipped (tier-1 covers them)") {
    CHECK_FALSE(T::should_seed(10'000, 0, "", 0, "a", 1));
}

TEST_CASE("shadow-compare skip: identical text and caret") {
    // A seed here is a model no-op but a full canvas rebuild — the "canvas
    // resets even when you don't want it to" report.
    CHECK_FALSE(T::should_seed(10'000, 0, "same text", 4, "same text", 4));
}

TEST_CASE("same text, moved caret still seeds") {
    // Clicking elsewhere in the field is the whole point of clause 3/6.
    CHECK(T::should_seed(10'000, 0, "same text", 4, "same text", 8));
}

TEST_CASE("different text seeds") {
    CHECK(T::should_seed(10'000, 0, "old", 0, "changed", 3));
}

TEST_CASE("quiet window dominates everything") {
    // Even an identical-text echo is dropped in the window — but so is a
    // genuine user click 10ms after our injection; the debounce + the next
    // event will deliver it once the window lapses.
    CHECK_FALSE(T::should_seed(10'000, 9'999, "", 0, "anything", 0));
}

// ── Sentence-window trimming (governance#40) ──

TEST_CASE("sentence_window: basic sentence extraction") {
    const std::string text = "First sentence. Second sentence with more words. Third.";
    auto w = T::sentence_window(text, 28); // caret in "Second sentence" (byte 28 = 'n')
    // Boundary at "." (byte 14); skip space at 15; start=16 ("S")
    // Window = [16, 28] = "Second sente"
    CHECK(w.text == "Second sente");
    CHECK(w.caret_offset == 12); // 28 - 16 = 12
}

TEST_CASE("sentence_window: first sentence has no boundary before it") {
    const std::string text = "Just one long sentence without terminators here";
    auto w = T::sentence_window(text, 20);
    CHECK(w.text == text.substr(0, 20));
    CHECK(w.caret_offset == 20);
}

TEST_CASE("sentence_window: newline is a boundary") {
    const std::string text = "line one\nline two is here";
    auto w = T::sentence_window(text, 18); // caret after "line two "
    // Boundary at "\n" (byte 8); skip nothing (l is not ws); start=9
    // Window = [9, 18) = "line two " (9 bytes)
    CHECK(w.text == "line two ");
}

TEST_CASE("sentence_window: cap at kSentenceWindowMaxChars") {
    std::string text(T::kSentenceWindowMaxChars + 100, 'x'); // no boundaries at all
    auto w = T::sentence_window(text, static_cast<int>(text.size()));
    CHECK(w.text.size() <= T::kSentenceWindowMaxChars);
    CHECK(w.text == text.substr(text.size() - T::kSentenceWindowMaxChars)); // trailing bytes
}

TEST_CASE("sentence_window: caret at document start") {
    const std::string text = "Hello from the beginning.";
    auto w = T::sentence_window(text, 0);
    CHECK(w.text.empty());
    CHECK(w.caret_offset == 0);
}

TEST_CASE("sentence_window: UTF-8 multibyte content") {
    const std::string text = "Prévious. Héllo wörld with accénts.";
    auto w = T::sentence_window(text, 16);
    // "Prévious." = 10 bytes (boundary at 9), space at 10, "Héllo" starts at 11
    // Window = [11, 16) = "Héll" (H+é(2)+l+l = 5 bytes) — é NOT split
    CHECK(w.text.size() == 5);
    CHECK(w.text[0] == 'H');
    CHECK(w.text.substr(1, 2) == "\xC3\xA9"); // é as UTF-8
}

TEST_CASE("sentence_window: symmetric trimming makes echoes match") {
    // The core governance#40 insight: during typing, engine buffer and
    // target read differ (engine has extra prefix) but their sentence
    // windows match → should_seed returns false (echo, skip).
    const std::string engine = "Previous paragraph. Hello wor";
    const std::string target = "Different prefix text! Hello wor";
    CHECK_FALSE(T::should_seed(10000, 0, engine, engine.size(), target, target.size()));
}

TEST_CASE("sentence_window: different sentence triggers seed") {
    const std::string engine = "Completely different context here";
    const std::string target = "Something else entirely now";
    CHECK(T::should_seed(10000, 0, engine, engine.size(), target, target.size()));
}

TEST_CASE("sentence_window: caret move within same sentence") {
    // Same text, different caret → different window extent → seed
    const std::string text = "Same sentence different caret positions";
    const auto w1 = T::sentence_window(text, 15);
    const auto w2 = T::sentence_window(text, 25);
    CHECK(w1.text.size() < w2.text.size());
}

// ── Review-loop fixes: boundary context sensitivity ──

TEST_CASE("sentence_window: decimal point is NOT a boundary") {
    const std::string text = "The value is 3.14159 approximately";
    auto w = T::sentence_window(text, 20); // caret after "3.14"
    // '.' before '1' at position 18 → decimal, not boundary
    // Window should include text back to at least "The value is 3.14"
    CHECK(w.text.find("3.14") != std::string::npos);
}

TEST_CASE("sentence_window: time colon is NOT a boundary") {
    const std::string text = "Meet at 10:30 sharp";
    auto w = T::sentence_window(text, 14); // caret after "10:30"
    CHECK(w.text.find("10:30") != std::string::npos);
}

TEST_CASE("sentence_window: URL dots are NOT boundaries") {
    const std::string text = "Visit https://example.com for more info";
    auto w = T::sentence_window(text, 26); // caret in the URL
    CHECK(w.text.find("example.com") != std::string::npos);
}

TEST_CASE("sentence_window: codepoint cap for CJK") {
    // 100 CJK chars = 300 UTF-8 bytes, no sentence boundaries
    std::string text;
    for (int i = 0; i < 100; i++) text += "\xE4\xBD\xA0\xE5\xA5\xBD"; // 你好 (2 chars, 6 bytes)
    // 100 iterations × 2 chars = 200 chars = 600 bytes — exceeds the 200-codepoint cap
    auto w = T::sentence_window(text, static_cast<int>(text.size()));
    // Should be capped at ~200 codepoints ≈ 600 bytes of CJK
    // (or less — the cap counts codepoints, not bytes)
    CHECK(w.text.size() <= 200 * 3); // 3 bytes per CJK char max
    CHECK(w.text.size() > 100);     // not truncated to a tiny window
}

TEST_CASE("sentence_window: empty window at sentence start still allows seed") {
    // A caret right after ". " has an empty window but a large field —
    // the engine should seed-empty (clearing stale context)
    const std::string target = "Stale old sentence. New sentence starts here";
    const std::string engine = "Completely different context";
    CHECK(T::should_seed(10000, 0, engine, engine.size(), target, 20)); // caret at ". "
    // ^ the target field is large (kMinFieldLength passes on untrimmed),
    // the sentence windows differ → seed
}

TEST_CASE("sentence_window: closer paren after boundary is skipped") {
    const std::string text = "(First one.) Second two starts here";
    auto w = T::sentence_window(text, 25); // caret in "Second"
    // Boundary at '.' (byte 11), then ')' and ' ' — all should be skipped
    CHECK(w.text.find("Second") == 0);
}
