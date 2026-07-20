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
    static constexpr char kPlatform[] = "linux";
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
    void capture_exception(const CrashEnvelope& env);

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
