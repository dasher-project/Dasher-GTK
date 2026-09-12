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
