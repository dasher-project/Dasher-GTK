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
