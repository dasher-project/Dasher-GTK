#pragma once

#include "AnalyticsSettings.h"
#include "CrashReporter.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace analytics {

// Front-end analytics client. All Dasher frontends share one PostHog project and
// are distinguished by the default properties (platform / app_variant); events
// are opt-in gated (RFC 0001) and crashes are reported as $exception (RFC 0009).
//
// Events are serialised and appended to a bounded queue; a background worker
// POSTs them to PostHog and, when offline, keeps them for the next launch (the
// queue is persisted to disk). Nothing is sent unless the user has opted in.
class AnalyticsClient {
  public:
    // GTK-specific default properties (RFC 0001 / analytics-events.json).
    // The GTK frontend is multiplatform (Linux is primary, but Windows and macOS
    // builds ship too) — report the host OS, don't assume Linux.
#if defined(_WIN32)
    static constexpr char kPlatform[] = "windows";
#elif defined(__APPLE__)
    static constexpr char kPlatform[] = "macos";
#else
    static constexpr char kPlatform[] = "linux";
#endif
    static constexpr char kAppVariant[] = "dasher-gtk";
    static constexpr char kPostHogHost[] = "https://eu.i.posthog.com";
    // Public PostHog project token, shared across all Dasher frontends. A phc_
    // token is a write-only ingestion key, safe to ship in the client.
    static constexpr char kProjectToken[] = "phc_ubtNRuCT7Zqo4dVrVWRnJRYE9m9WqGeTyK7zVDKQ968J";
    // Locally-queued events are capped here; the oldest are dropped past it (RFC 0001).
    static constexpr std::size_t kMaxQueue = 500;

    static AnalyticsClient& instance();
    ~AnalyticsClient();

    // Adopt consent state + anonymous id from settings, load any persisted queue.
    void init(AnalyticsSettings& settings);

    // Reflect a consent change made in the Privacy preferences.
    void set_opted_in(bool opted_in);
    bool opted_in() const { return m_opted_in; }

    // Record an event. No-op unless opted in.
    void capture(const std::string& event, const std::map<std::string, std::string>& properties = {});

    // Report a recovered crash as a $exception. No-op unless opted in.
    // `deferred` (issue #114) marks the report as the replay of a previous
    // session's crash file — the flag the Android frontend already sends on
    // its pending-file flush path, so PostHog filters can tell live from
    // replayed crashes across frontends.
    void capture_exception(const CrashEnvelope& env, bool deferred = false);

    // Default properties appended to every event.
    std::map<std::string, std::string> default_properties() const;

    // --- pure helpers, exposed for tests ---
    // JSON-escape a string value (quotes, backslashes, control chars).
    static std::string json_escape(const std::string& s);
    // Build the body of a PostHog /capture/ request.
    static std::string build_capture_body(const std::string& event,
                                          const std::map<std::string, std::string>& properties,
                                          const std::string& distinct_id, const std::string& token,
                                          const std::string& timestamp);

    // --- #114: crash-frame envelope helpers (pure, exposed for tests) ---
    // PostHog's SDKs cap an exception's frames at 50; match them.
    static constexpr std::size_t kMaxExceptionFrames = 50;

    // Parse a saved stack trace (the terminate handler's format: optional
    // "<type>: message" line, then one canonical "module(function+0xoff)
    // [0xaddr]" line per frame) into frame function names, top of stack
    // first, capped at kMaxExceptionFrames. Message lines and anything
    // malformed (scrubber damage, a size-cap-truncated tail) are skipped.
    static std::vector<std::string> parse_exception_frames(const std::string& stack_trace);

    // The $exception_message for a crash envelope: the saved message line's
    // text when one exists, else "<type>: <top frame>" so grouping and the
    // $exception_functions column keep signal (issue #114's minimum).
    static std::string exception_message(const std::string& exception_type, const std::string& stack_trace);

    // Build the $exception_list JSON array (type/value/mechanism{handled:
    // false, generic}/stacktrace{type: raw, frames[{function}]}) — the shape
    // PostHog Error Tracking groups and lists functions from, verified
    // against the SDK source in Dasher-Windows#116.
    static std::string build_exception_list(const std::string& exception_type, const std::string& value,
                                            const std::string& stack_trace);

  private:
    AnalyticsClient() = default;

    // Ingestion host; honours $DASHER_ANALYTICS_HOST so tests can point at a mock.
    static std::string host();
    static bool http_post(const std::string& url, const std::string& body);

    void enqueue(const std::string& event, const std::map<std::string, std::string>& properties);
    void ensure_worker();
    void worker_loop();

    std::string queue_path() const;
    void load_persisted_queue();
    void persist_queue_locked();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue; // serialised request bodies, oldest first
    std::thread m_worker;
    bool m_worker_started = false;
    bool m_stop = false;

    bool m_opted_in = false;
    std::string m_distinct_id;
};

} // namespace analytics
