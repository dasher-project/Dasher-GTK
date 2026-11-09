#pragma once

#include <functional>
#include <string>

namespace analytics {

// A crash record, reconstructed on the next launch and reported to PostHog as a
// $exception (RFC 0009). Field names match the shared crash-file format.
struct CrashEnvelope {
    std::string exception_type; // e.g. "std::runtime_error", "Signal:SIGSEGV"
    std::string source;         // capture hook, e.g. "std::terminate", "signal_handler"
    std::string app_version;
    std::string os_version;
    // PII-scrubbed. #114: the terminate path saves an ex.ToString()-style
    // message line ("<type>: <what>") followed by one canonical
    // "module(function+0xoff) [0xaddr]" backtrace line per frame, so the next
    // launch can rebuild structured frames for PostHog Error Tracking. May be
    // empty for native signals (see the frame-less decision in CrashReporter).
    std::string stack_trace;
    std::string engine_log_tail; // last engine log lines, PII-scrubbed
};

// Installs crash handlers that write a small crash file synchronously (no heap,
// no network, no SDK calls) and, on the next launch, flushes it to the analytics
// sink. Two paths per RFC 0009:
//   * std::set_terminate  — uncaught C++ exceptions; runs in normal context, so
//     it captures the exception type, message, a real (demangled) backtrace
//     (#114) and the live engine log tail.
//   * SIGSEGV/SIGABRT/SIGILL/SIGBUS — async-signal-safe: writes a pre-built
//     minimal record; the log tail is recovered from the mirrored engine.log
//     next launch. Deliberately frame-less: see the decision comment at the
//     signal handler in CrashReporter.cpp (#114).
class CrashReporter {
  public:
    // Discard crash files older than this without reporting (RFC 0009).
    static constexpr long kMaxAgeSeconds = 7 * 24 * 60 * 60;

    // Install handlers. Idempotent; call once, early in main().
    static void install();

    // Report and delete a crash file left by a previous run, if any. Files
    // older than kMaxAgeSeconds are deleted without reporting.
    static void flush_pending(const std::function<void(const CrashEnvelope&)>& sink);

    // <XDG_DATA_HOME>/dasher/pending_crash.txt.
    static std::string crash_file_path();

    // Serialise/parse the crash-file text format. Exposed for unit tests.
    static bool write_crash_file(const std::string& path, const CrashEnvelope& env);
    static bool parse_crash_file(const std::string& path, CrashEnvelope& out);

    // Pre-filled envelope (app/os version) for caught, non-fatal reports —
    // e.g. the engine's sticky error flag (RFC 0009 A2).
    static CrashEnvelope make_envelope(const std::string& exception_type, const std::string& source);
};

} // namespace analytics
