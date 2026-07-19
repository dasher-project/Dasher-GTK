#pragma once

#include "AnalyticsSettings.h"
#include "CrashReporter.h"

#include <map>
#include <string>

namespace analytics {

// Front-end analytics client. All Dasher frontends share one PostHog project and
// are distinguished by the default properties (platform / app_variant); events
// are opt-in gated (RFC 0001) and crashes are reported as $exception (RFC 0009).
//
// This is the transport-free foundation: events and exceptions are validated and
// gated here but not yet sent over the network. The libcurl POST to
// https://eu.i.posthog.com (with the shared public project token) and the
// offline queue land in a follow-up PR; the interface is stable so that change
// is localised to send().
class AnalyticsClient {
  public:
    // GTK-specific default properties (RFC 0001 / analytics-events.json).
    static constexpr char kPlatform[] = "linux";
    static constexpr char kAppVariant[] = "dasher-gtk";
    static constexpr char kPostHogHost[] = "https://eu.i.posthog.com";

    static AnalyticsClient& instance();

    // Adopt consent state + anonymous id from settings.
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

  private:
    AnalyticsClient() = default;

    // Hand the (gated, property-complete) payload to the transport. Currently a
    // diagnostic stub; the network send is added in the follow-up PR.
    void send(const std::string& event, const std::map<std::string, std::string>& properties);

    bool m_opted_in = false;
    std::string m_distinct_id;
};

} // namespace analytics
