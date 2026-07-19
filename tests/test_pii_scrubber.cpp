// Unit tests for PiiScrubber — the redaction applied to crash payloads before
// they are written to disk or sent (RFC 0009).
#include "Analytics/PiiScrubber.h"

#include <doctest/doctest.h>

#include <string>

using analytics::PiiScrubber;

TEST_CASE("scrub redacts the Linux home-directory user segment") {
    CHECK(PiiScrubber::scrub("/home/alice/.config/dasher/log") == "/home/<user>/.config/dasher/log");
}

TEST_CASE("scrub redacts the macOS home-directory user segment") {
    CHECK(PiiScrubber::scrub("/Users/bob/Library/Dasher") == "/Users/<user>/Library/Dasher");
}

TEST_CASE("scrub redacts a Windows user profile path") {
    CHECK(PiiScrubber::scrub("C:\\Users\\Carol\\AppData") == "C:\\Users\\<user>\\AppData");
}

TEST_CASE("scrub redacts email addresses") {
    CHECK(PiiScrubber::scrub("reached alice.b+test@example.co.uk here") == "reached <email> here");
}

TEST_CASE("scrub leaves non-PII paths untouched") {
    CHECK(PiiScrubber::scrub("/usr/lib/dasher/libdasher.so") == "/usr/lib/dasher/libdasher.so");
}

TEST_CASE("truncate enforces the byte cap") {
    std::string s(100, 'x');
    CHECK(PiiScrubber::truncate(s, 10).size() == 10);
    CHECK(PiiScrubber::truncate(s, 200) == s); // shorter than the cap: unchanged
}

TEST_CASE("truncate does not split a UTF-8 sequence") {
    // "é" encodes as 0xC3 0xA9. Cutting at 2 bytes would land mid-sequence, so
    // truncate must back off to the 1-byte "a".
    std::string s = "a\xC3\xA9";
    CHECK(PiiScrubber::truncate(s, 2) == "a");
}
