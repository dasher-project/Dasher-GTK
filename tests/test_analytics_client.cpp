// Unit tests for the analytics payload serialisation (the pure, network-free
// parts of AnalyticsClient).
#include "Analytics/AnalyticsClient.h"

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

using analytics::AnalyticsClient;

TEST_CASE("json_escape handles quotes, backslashes and control characters") {
    CHECK(AnalyticsClient::json_escape("a\"b\\c") == "a\\\"b\\\\c");
    CHECK(AnalyticsClient::json_escape("line1\nline2\t!") == "line1\\nline2\\t!");
    CHECK(AnalyticsClient::json_escape(std::string("\x01", 1)) == "\\u0001");
    CHECK(AnalyticsClient::json_escape("plain text") == "plain text");
}

TEST_CASE("build_capture_body produces a well-formed PostHog payload") {
    std::map<std::string, std::string> props = {{"platform", "linux"}, {"tab_name", "Privacy"}};
    std::string body =
        AnalyticsClient::build_capture_body("settings_viewed", props, "uuid-123", "phc_test", "2026-07-19T00:00:00Z");
    CHECK(body.front() == '{');
    CHECK(body.back() == '}');
    CHECK(body.find("\"api_key\":\"phc_test\"") != std::string::npos);
    CHECK(body.find("\"event\":\"settings_viewed\"") != std::string::npos);
    CHECK(body.find("\"distinct_id\":\"uuid-123\"") != std::string::npos);
    CHECK(body.find("\"timestamp\":\"2026-07-19T00:00:00Z\"") != std::string::npos);
    CHECK(body.find("\"platform\":\"linux\"") != std::string::npos);
    CHECK(body.find("\"tab_name\":\"Privacy\"") != std::string::npos);
}

TEST_CASE("build_capture_body escapes special characters in property values") {
    std::map<std::string, std::string> props = {{"stack_trace", "at /home/x\n\"boom\""}};
    std::string body = AnalyticsClient::build_capture_body("$exception", props, "id", "tok", "ts");
    CHECK(body.find('\n') == std::string::npos);           // no raw newline survives in the JSON
    CHECK(body.find("\\n") != std::string::npos);          // it was escaped
    CHECK(body.find("\\\"boom\\\"") != std::string::npos); // quotes escaped
}

TEST_CASE("known boolean control flags serialise as JSON booleans, everything else stays a string") {
    std::map<std::string, std::string> props = {{"$geoip_disable", "true"},
                                                {"$os", "Linux"},
                                                {"note", "true"},
                                                {"$custom", "false"},
                                                // textual $-prefixed fields whose value happens to look
                                                // boolean must never be retyped (PostHog Error Tracking)
                                                {"$exception_type", "true"},
                                                {"$exception_message", "false"}};
    std::string body = AnalyticsClient::build_capture_body("app_launched", props, "id", "tok", "ts");
    CHECK(body.find("\"$geoip_disable\":true") != std::string::npos);          // boolean, unquoted
    CHECK(body.find("\"$os\":\"Linux\"") != std::string::npos);                // $-key, non-bool value: string
    CHECK(body.find("\"note\":\"true\"") != std::string::npos);                // user data never becomes a boolean
    CHECK(body.find("\"$custom\":\"false\"") != std::string::npos);            // unknown $-flag stays a string
    CHECK(body.find("\"$exception_type\":\"true\"") != std::string::npos);     // textual field, not retyped
    CHECK(body.find("\"$exception_message\":\"false\"") != std::string::npos); // textual field, not retyped
}

// --- #114: the crash-frame envelope (crash reports carried no frames) ------

TEST_CASE("#114 parse_exception_frames extracts functions from a captured stack") {
    // The exact shape the terminate handler saves: message line first, then
    // one canonical backtrace_symbols line per frame (demangled).
    const std::string captured = "std::runtime_error: socket read failed\n"
                                 "/usr/bin/dasher(main+0x2a1) [0x55d6e2c2a7f4]\n"
                                 "/usr/lib/libdashercore.so(dasher::DasherBridge::frame(long)+0x1e) [0x7f2c1b03d9c0]\n"
                                 "/usr/lib/libc.so.6(+0x18b4a) [0x7f2c1ae38b4a]";
    const std::vector<std::string> frames = AnalyticsClient::parse_exception_frames(captured);
    REQUIRE(frames.size() == 3);
    CHECK(frames[0] == "main");
    CHECK(frames[1] == "dasher::DasherBridge::frame(long)");
    // Unsymbolised frames keep module+offset — which binary the frame sits in
    // is still signal for Error Tracking.
    CHECK(frames[2] == "/usr/lib/libc.so.6(+0x18b4a)");
}

TEST_CASE("#114 message lines, malformed lines and empty stacks yield no frames") {
    CHECK(AnalyticsClient::parse_exception_frames("").empty());
    CHECK(AnalyticsClient::parse_exception_frames("std::runtime_error: boom").empty());
    CHECK(AnalyticsClient::parse_exception_frames("random debris\nnot a frame line\n\n").empty());
    // A stack whose last line was cut mid-frame by the size cap must not
    // produce a garbage frame.
    CHECK(AnalyticsClient::parse_exception_frames("std::runtime_error: boom\n/usr/bin/dasher(main+0x2a) [0x55d6e2c2a")
              .empty());
}

TEST_CASE("#114 frames are capped at the PostHog SDK's 50") {
    std::string captured = "std::runtime_error: boom\n";
    for (int i = 0; i < 120; ++i)
        captured += "/usr/bin/dasher(fn" + std::to_string(i) + "+0x1) [0x55d6e2c2a000]\n";
    const std::vector<std::string> frames = AnalyticsClient::parse_exception_frames(captured);
    CHECK(frames.size() == AnalyticsClient::kMaxExceptionFrames);
    CHECK(frames.front() == "fn0");
    CHECK(frames.back() == "fn49");
}

TEST_CASE("#114 exception_message prefers the saved message, falls back to type + top frame") {
    // Saved message line (terminate path): the "Type:" prefix is stripped so
    // $exception_message carries just the message text.
    CHECK(AnalyticsClient::exception_message(
              "std::runtime_error", "std::runtime_error: socket read failed\n/usr/bin/dasher(main+0x2a) [0x55d6]") ==
          "socket read failed");
    // Frames only (the catch(...) path): "<type>: <top frame>".
    CHECK(AnalyticsClient::exception_message("unknown exception", "/usr/bin/dasher(main+0x2a) [0x55d6]") ==
          "unknown exception: main");
    // Empty stack (native signal record): type only.
    CHECK(AnalyticsClient::exception_message("Signal:SIGSEGV", "") == "Signal:SIGSEGV");
    // Free-form message line (the live engine sticky-error report) is kept
    // verbatim.
    CHECK(AnalyticsClient::exception_message("DasherEngineError", "dasher_has_engine_error: sticky error flag set") ==
          "dasher_has_engine_error: sticky error flag set");
    // "Type:" with an empty message falls back rather than reporting "".
    CHECK(AnalyticsClient::exception_message("std::runtime_error",
                                             "std::runtime_error: \n/usr/bin/dasher(main+0x2a) [0x55d6]") ==
          "std::runtime_error: main");
}

TEST_CASE("#114 build_exception_list mirrors the SDK envelope shape") {
    const std::string json = AnalyticsClient::build_exception_list(
        "std::runtime_error", "boom \"quoted\"",
        "std::runtime_error: boom \"quoted\"\n/usr/bin/dasher(main+0x2a) [0x55d6]");
    CHECK(json.front() == '[');
    CHECK(json.back() == ']');
    CHECK(json.find("\"type\":\"std::runtime_error\"") != std::string::npos);
    CHECK(json.find("\"value\":\"boom \\\"quoted\\\"\"") != std::string::npos);
    CHECK(json.find("\"mechanism\":{\"handled\":false,\"type\":\"generic\"}") != std::string::npos);
    CHECK(json.find("\"stacktrace\":{\"type\":\"raw\",\"frames\":[{\"platform\":\"custom\",\"lang\":\"native\","
                    "\"function\":\"main\"}]}") != std::string::npos);
}

TEST_CASE("#114 r1: every frame object carries the Required platform/lang pair (Windows #116 shape)") {
    const std::string json = AnalyticsClient::build_exception_list(
        "std::runtime_error", "boom",
        "std::runtime_error: boom\n/lib/x.so(Widget::paint(int)+0x1f) [0x7f001]\n/lib/x.so(+0x2a) [0x7f002]");
    // The frame objects must match the SDK-verified envelope: PostHog's
    // manual-capture schema marks platform/lang Required on each frame —
    // function-only frames risk silent ingestion degradation.
    CHECK(json.find("\"platform\":\"custom\",\"lang\":\"native\",\"function\":\"Widget::paint(int)\"") !=
          std::string::npos);
    CHECK(json.find("\"function\":\"/lib/x.so(+0x2a)\"") !=
          std::string::npos); // full module+offset per the unsymbolised-frame rule
}

TEST_CASE("#114 build_exception_list emits an empty frames array for frame-less records") {
    const std::string json = AnalyticsClient::build_exception_list("Signal:SIGSEGV", "Signal:SIGSEGV", "");
    CHECK(json.find("\"type\":\"Signal:SIGSEGV\"") != std::string::npos);
    CHECK(json.find("\"frames\":[]") != std::string::npos);
}

TEST_CASE("#114 $exception_list serialises as a JSON array and deferred as a boolean") {
    std::map<std::string, std::string> props = {
        {"$exception_type", "std::runtime_error"},
        {"$exception_list",
         AnalyticsClient::build_exception_list("std::runtime_error", "boom",
                                               "std::runtime_error: boom\n/usr/bin/dasher(main+0x2a) [0x55d6]")},
        {"deferred", "true"}};
    const std::string body = AnalyticsClient::build_capture_body("$exception", props, "id", "tok", "ts");
    CHECK(body.find("\"$exception_list\":[{") != std::string::npos);                     // array, not a string
    CHECK(body.find("\\\"frames\\\"") == std::string::npos);                             // not stringified JSON
    CHECK(body.find("\"deferred\":true") != std::string::npos);                          // boolean, matches Android
    CHECK(body.find("\"deferred\":\"true\"") == std::string::npos);                      // never the string form
    CHECK(body.find("\"$exception_type\":\"std::runtime_error\"") != std::string::npos); // textual fields stay strings
}
