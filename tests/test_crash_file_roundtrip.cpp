// Unit tests for the RFC 0009 crash-file text format: what the std::terminate
// and signal handlers write must parse back into the same envelope on the next
// launch.
#include "Analytics/AnalyticsClient.h"
#include "Analytics/CrashReporter.h"

#include <doctest/doctest.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <string>

using analytics::AnalyticsClient;
using analytics::CrashEnvelope;
using analytics::CrashReporter;

namespace {
std::string temp_path(const char* name) {
    char* p = g_build_filename(g_get_tmp_dir(), name, nullptr);
    std::string s = p ? p : "";
    g_free(p);
    return s;
}
} // namespace

TEST_CASE("rich crash envelope round-trips through the file format") {
    const std::string path = temp_path("dasher_test_crash_rich.txt");

    CrashEnvelope in;
    in.exception_type = "std::runtime_error";
    in.source = "std::terminate";
    in.app_version = "1.2.3";
    in.os_version = "Linux 6.8.0";
    in.stack_trace = "frame one\nframe two";
    in.engine_log_tail = "[I] started\n[E] boom";

    REQUIRE(CrashReporter::write_crash_file(path, in));

    CrashEnvelope out;
    REQUIRE(CrashReporter::parse_crash_file(path, out));
    CHECK(out.exception_type == in.exception_type);
    CHECK(out.source == in.source);
    CHECK(out.app_version == in.app_version);
    CHECK(out.os_version == in.os_version);
    CHECK(out.stack_trace == in.stack_trace);
    CHECK(out.engine_log_tail == in.engine_log_tail);

    g_remove(path.c_str());
}

TEST_CASE("signal-style record with empty stack and tail parses cleanly") {
    const std::string path = temp_path("dasher_test_crash_signal.txt");

    CrashEnvelope in;
    in.exception_type = "Signal:SIGSEGV";
    in.source = "signal_handler";
    in.app_version = "1.0";
    in.os_version = "Linux";
    // stack_trace and engine_log_tail intentionally empty (native signal path)

    REQUIRE(CrashReporter::write_crash_file(path, in));

    CrashEnvelope out;
    REQUIRE(CrashReporter::parse_crash_file(path, out));
    CHECK(out.exception_type == "Signal:SIGSEGV");
    CHECK(out.source == "signal_handler");
    CHECK(out.stack_trace.empty());
    CHECK(out.engine_log_tail.empty());

    g_remove(path.c_str());
}

TEST_CASE("#114 terminate-style record (message line + frames) round-trips") {
    const std::string path = temp_path("dasher_test_crash_frames.txt");

    CrashEnvelope in;
    in.exception_type = "std::runtime_error";
    in.source = "std::terminate";
    in.app_version = "1.2.3";
    in.os_version = "Linux 6.8.0";
    in.stack_trace = "std::runtime_error: socket read failed\n"
                     "/usr/bin/dasher(main+0x2a1) [0x55d6e2c2a7f4]\n"
                     "/usr/lib/libc.so.6(+0x18b4a) [0x7f2c1ae38b4a]";
    in.engine_log_tail = "[E] boom";

    REQUIRE(CrashReporter::write_crash_file(path, in));

    CrashEnvelope out;
    REQUIRE(CrashReporter::parse_crash_file(path, out));
    CHECK(out.exception_type == in.exception_type);
    CHECK(out.stack_trace == in.stack_trace);
    // The saved text parses back into frames and a message for the envelope
    // the next launch sends to PostHog Error Tracking.
    CHECK(AnalyticsClient::parse_exception_frames(out.stack_trace).size() == 2);
    CHECK(AnalyticsClient::exception_message(out.exception_type, out.stack_trace) == "socket read failed");

    g_remove(path.c_str());
}

TEST_CASE("parsing a missing file fails without throwing") {
    CrashEnvelope out;
    CHECK_FALSE(CrashReporter::parse_crash_file(temp_path("dasher_no_such_crash.txt"), out));
}
