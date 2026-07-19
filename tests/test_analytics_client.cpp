// Unit tests for the analytics payload serialisation (the pure, network-free
// parts of AnalyticsClient).
#include "Analytics/AnalyticsClient.h"

#include <doctest/doctest.h>

#include <map>
#include <string>

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
