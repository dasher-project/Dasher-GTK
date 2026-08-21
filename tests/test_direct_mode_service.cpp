// Unit tests for DirectModeService's pure UTF-8 counting. The ydotool calls
// themselves are integration-level and not exercised here.
//
// Hex escapes: adjacent string literals are split ("\xC3\xA9" "b") where a
// following character could otherwise be parsed as extra hex digits (\xA9b).
#include "Output/DirectModeService.h"

#include <doctest/doctest.h>

// Keyboard-mode backspace injection must count characters, not bytes: on UTF-8
// output like the 2-byte "e-acute" or the 3-byte euro sign, a byte count would
// press backspace too many times and eat preceding characters.
TEST_CASE("utf8_length counts code points, not bytes") {
    CHECK(DirectModeService::utf8_length("") == 0);
    CHECK(DirectModeService::utf8_length("a") == 1);
    CHECK(DirectModeService::utf8_length("abc") == 3);
    // e-acute (2 bytes), euro sign (3 bytes), grinning face (4 bytes)
    CHECK(DirectModeService::utf8_length("\xC3\xA9") == 1);
    CHECK(DirectModeService::utf8_length("\xE2\x82\xAC") == 1);
    CHECK(DirectModeService::utf8_length("\xF0\x9F\x98\x80") == 1);
    // Mixed ASCII and multibyte: a + e-acute + b, and h-e-acute-l-l-o
    CHECK(DirectModeService::utf8_length("a\xC3\xA9"
                                         "b") == 3);
    CHECK(DirectModeService::utf8_length("h\xC3\xA9llo") == 5);
}
