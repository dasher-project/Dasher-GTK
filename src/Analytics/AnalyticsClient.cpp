#include "AnalyticsClient.h"

#include <glib.h>

#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/utsname.h>
#endif

#ifndef DASHER_GTK_VERSION
#define DASHER_GTK_VERSION "0.0.0-dev"
#endif

namespace analytics {

constexpr char AnalyticsClient::kPlatform[];
constexpr char AnalyticsClient::kAppVariant[];
constexpr char AnalyticsClient::kPostHogHost[];

namespace {

std::string os_version() {
#if defined(__unix__) || defined(__APPLE__)
    struct utsname u {};
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
#endif
    return "unknown";
}

} // namespace

AnalyticsClient& AnalyticsClient::instance() {
    static AnalyticsClient client;
    return client;
}

void AnalyticsClient::init(AnalyticsSettings& settings) {
    m_opted_in = settings.opted_in();
    // Only materialise (and persist) an anonymous id once the user has opted in,
    // so an opted-out install never gets a stable identifier written to disk.
    if (m_opted_in) {
        m_distinct_id = settings.anonymous_id();
    }
}

void AnalyticsClient::set_opted_in(bool opted_in) {
    m_opted_in = opted_in;
}

std::map<std::string, std::string> AnalyticsClient::default_properties() const {
    return {
        {"platform", kPlatform},
        {"app_variant", kAppVariant},
        {"app_version", DASHER_GTK_VERSION},
        {"os_version", os_version()},
    };
}

void AnalyticsClient::capture(const std::string& event, const std::map<std::string, std::string>& properties) {
    if (!m_opted_in) return;
    auto props = default_properties();
    for (const auto& kv : properties)
        props[kv.first] = kv.second;
    send(event, props);
}

void AnalyticsClient::capture_exception(const CrashEnvelope& env) {
    if (!m_opted_in) return;
    auto props = default_properties();
    props["exception_type"] = env.exception_type;
    props["stack_trace"] = env.stack_trace;
    props["engine_log_tail"] = env.engine_log_tail;
    props["source"] = env.source;
    send("$exception", props);
}

void AnalyticsClient::send(const std::string& event, const std::map<std::string, std::string>& properties) {
    // Transport stub: the network POST to kPostHogHost lands in the follow-up
    // PR. Log at debug level so the gating and property assembly are observable
    // without emitting anything off-device.
    std::string summary = event + " {";
    bool first = true;
    for (const auto& kv : properties) {
        if (!first) summary += ", ";
        summary += kv.first + "=" + kv.second;
        first = false;
    }
    summary += "}";
    g_debug("analytics (not yet transmitted): %s", summary.c_str());
}

} // namespace analytics
