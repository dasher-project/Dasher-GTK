#include "AnalyticsClient.h"

#ifdef HAS_ANALYTICS_CURL
#include <curl/curl.h>
#endif
#include <glib.h>
#include <glib/gstdio.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
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
constexpr char AnalyticsClient::kProjectToken[];

namespace {

#ifdef HAS_ANALYTICS_CURL
std::once_flag g_curl_once;
#endif

std::string os_version() {
#if defined(__unix__) || defined(__APPLE__)
    struct utsname u {};
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
#endif
    return "unknown";
}

std::string iso8601_now() {
    GDateTime* dt = g_date_time_new_now_utc();
    char* s = dt ? g_date_time_format_iso8601(dt) : nullptr;
    std::string out = s ? s : "";
    g_free(s);
    if (dt) g_date_time_unref(dt);
    return out;
}

} // namespace

AnalyticsClient& AnalyticsClient::instance() {
    static AnalyticsClient client;
    return client;
}

AnalyticsClient::~AnalyticsClient() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

std::string AnalyticsClient::host() {
    if (const char* override_host = std::getenv("DASHER_ANALYTICS_HOST")) {
        if (*override_host) return override_host;
    }
    return kPostHogHost;
}

void AnalyticsClient::init(AnalyticsSettings& settings) {
    m_opted_in = settings.opted_in();
    // Only materialise (and persist) an anonymous id once the user has opted in,
    // so an opted-out install never gets a stable identifier written to disk.
    if (m_opted_in) {
        m_distinct_id = settings.anonymous_id();
        load_persisted_queue();
        ensure_worker(); // flush anything queued from a previous session
    }
}

void AnalyticsClient::set_opted_in(bool opted_in) {
    m_opted_in = opted_in;
    if (opted_in) ensure_worker();
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
    ensure_worker();
    enqueue(event, props);
}

void AnalyticsClient::capture_exception(const CrashEnvelope& env) {
    if (!m_opted_in) return;
    auto props = default_properties();
    props["exception_type"] = env.exception_type;
    props["stack_trace"] = env.stack_trace;
    props["engine_log_tail"] = env.engine_log_tail;
    props["source"] = env.source;
    // PostHog Error Tracking keys so the crash surfaces as a $exception rather
    // than a bare custom event.
    props["$exception_type"] = env.exception_type;
    props["$exception_message"] = env.stack_trace.empty() ? env.source : env.stack_trace;
    ensure_worker();
    enqueue("$exception", props);
}

std::string AnalyticsClient::json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string AnalyticsClient::build_capture_body(const std::string& event,
                                                const std::map<std::string, std::string>& properties,
                                                const std::string& distinct_id, const std::string& token,
                                                const std::string& timestamp) {
    std::string body = "{";
    body += "\"api_key\":\"" + json_escape(token) + "\",";
    body += "\"event\":\"" + json_escape(event) + "\",";
    body += "\"distinct_id\":\"" + json_escape(distinct_id) + "\",";
    body += "\"timestamp\":\"" + json_escape(timestamp) + "\",";
    body += "\"properties\":{";
    bool first = true;
    for (const auto& kv : properties) {
        if (!first) body += ",";
        body += "\"" + json_escape(kv.first) + "\":\"" + json_escape(kv.second) + "\"";
        first = false;
    }
    body += "}}";
    return body;
}

bool AnalyticsClient::http_post(const std::string& url, const std::string& body) {
#ifdef HAS_ANALYTICS_CURL
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // safe to call off the main thread
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION, +[](char*, size_t size, size_t nmemb, void*) { return size * nmemb; });

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res == CURLE_OK && code >= 200 && code < 300;
#else
    // No HTTP backend on this build; the caller keeps the event queued.
    (void)url;
    (void)body;
    return false;
#endif
}

void AnalyticsClient::enqueue(const std::string& event, const std::map<std::string, std::string>& properties) {
    std::string body = build_capture_body(event, properties, m_distinct_id, kProjectToken, iso8601_now());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(body));
        while (m_queue.size() > kMaxQueue)
            m_queue.pop_front();
    }
    m_cv.notify_one();
}

void AnalyticsClient::ensure_worker() {
#ifdef HAS_ANALYTICS_CURL
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
#endif
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_worker_started) {
        m_worker_started = true;
        m_worker = std::thread(&AnalyticsClient::worker_loop, this);
    }
}

void AnalyticsClient::worker_loop() {
    const std::string url = host() + "/capture/";
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stop) {
        m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        while (!m_stop && !m_queue.empty()) {
            std::string body = std::move(m_queue.front());
            m_queue.pop_front();
            lock.unlock();
            bool ok = http_post(url, body);
            lock.lock();
            if (!ok) {
                // Offline / server error: keep the event and back off so it is
                // retried this session or persisted for the next launch.
                m_queue.push_front(std::move(body));
                m_cv.wait_for(lock, std::chrono::seconds(30), [this] { return m_stop; });
                break;
            }
        }
    }
    persist_queue_locked();
}

std::string AnalyticsClient::queue_path() const {
    char* dir = g_build_filename(g_get_user_data_dir(), "dasher", nullptr);
    g_mkdir_with_parents(dir, 0700);
    char* path = g_build_filename(dir, "analytics_queue.jsonl", nullptr);
    std::string result = path ? path : "";
    g_free(dir);
    g_free(path);
    return result;
}

void AnalyticsClient::load_persisted_queue() {
    std::ifstream f(queue_path());
    if (!f) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) m_queue.push_back(line);
    }
    while (m_queue.size() > kMaxQueue)
        m_queue.pop_front();
    // The file is rewritten from the live queue on shutdown; clear it now so a
    // crash before then doesn't double-count already-loaded events.
    g_unlink(queue_path().c_str());
}

void AnalyticsClient::persist_queue_locked() {
    const std::string path = queue_path();
    if (m_queue.empty()) {
        g_unlink(path.c_str());
        return;
    }
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) return;
    for (const auto& body : m_queue) {
        f << body << '\n';
    }
}

} // namespace analytics
