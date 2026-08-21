// Unit tests for DirectModeService's pure UTF-8 counting. The ydotool calls
// themselves are integration-level and not exercised here.
#include "Output/DirectModeService.h"

#include <doctest/doctest.h>

// Keyboard-mode backspace injection must count characters, not bytes: on UTF-8
// output like "é" (2 bytes) or "€" (3 bytes) a byte count would press backspace
// too many times and eat preceding characters. The counting itself is pure
// logic, so it is unit-tested directly; the ydotool calls around it are not.
TEST_CASE("utf8_length counts code points, not bytes") {
    CHECK(DirectModeService::utf8_length("") == 0);
    CHECK(DirectModeService::utf8_length("a") == 1);
    CHECK(DirectModeService::utf8_length("abc") == 3);
    CHECK(DirectModeService::utf8_length("\xC3\xA9") == 1);            // é, 2 bytes
    CHECK(DirectModeService::utf8_length("\xE2\x82\xAC") == 1);        // €, 3 bytes
    CHECK(DirectModeService::utf8_length("\xF0\x9F\x98\x80") == 1);    // 😀, 4 bytes
    CHECK(DirectModeService::utf8_length("a\xC3\xA9" "b") == 3);      // mixed (split the
                                                                       // literal: \xA9b would
                                                                       // parse as one escape)
    CHECK(DirectModeService::utf8_length("h\xC3\xA9llo") == 5);        // héllo
}
