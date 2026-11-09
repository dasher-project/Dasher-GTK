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

// Coarse OS name matching the $os values the official PostHog SDKs send, so OS
// breakdowns treat this frontend like the SDK-backed ones. This raw-HTTP
// transport sends no SDK context of its own.
std::string os_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

std::string iso8601_now() {
    GDateTime* dt = g_date_time_new_now_utc();
    char* s = dt ? g_date_time_format_iso8601(dt) : nullptr;
    std::string out = s ? s : "";
    g_free(s);
    if (dt) g_date_time_unref(dt);
    return out;
}

// #114: extract a frame's function name from one canonical backtrace line —
// "module(function+0xoff) [0xaddr]" — the shape CrashReporter's capture path
// writes (glibc prints it directly; macOS lines are normalised to it before
// saving). Returns "" for anything else (message lines, scrubber damage,
// empty lines, a tail truncated mid-frame by the size cap) so callers skip
// non-frames without knowing a second grammar. Unsymbolised frames
// ("module(+0xoff)" — no dynamic symbol) keep the "module(+0xoff)" text:
// which binary the frame belongs to is still signal for Error Tracking.
std::string frame_function_of_line(const std::string& line) {
    const std::size_t tail = line.rfind(") [0x");
    if (tail == std::string::npos || line.empty() || line.back() != ']' || tail + 5 >= line.size()) return "";
    // The grammar's '(' is the FIRST one — demangled function names contain
    // their own parentheses ("ns::f(int)"), so searching backwards for the
    // last '(' would split inside the signature.
    const std::size_t open = line.find('(');
    if (open == std::string::npos || open >= tail || tail == open + 1) return "";
    const std::string symbol = line.substr(open + 1, tail - open - 1);
    const std::size_t plus = symbol.find("+0x");
    if (plus == 0) return line.substr(0, tail + 1);
    return plus == std::string::npos ? symbol : symbol.substr(0, plus);
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
        // RFC 0022 item 4: the bundled DasherCore's version — makes engine
        // drift visible in PostHog version adoption.
        {"engine_version", DASHER_ENGINE_VERSION},
        {"os_version", os_version()},
        {"$os", os_name()},
        // RFC 0001 promises no location data. PostHog Cloud derives $geoip_* (city,
        // postal code, lat/lon) from the client IP even with project IP
        // anonymisation on — this per-event flag is the only Cloud-level control.
        // Emitted as a JSON boolean by build_capture_body.
        {"$geoip_disable", "true"},
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

void AnalyticsClient::capture_exception(const CrashEnvelope& env, bool deferred) {
    if (!m_opted_in) return;
    auto props = default_properties();
    props["exception_type"] = env.exception_type;
    props["stack_trace"] = env.stack_trace;
    props["engine_log_tail"] = env.engine_log_tail;
    props["source"] = env.source;
    // #114: the full Error Tracking envelope. PostHog groups $exception
    // events on $exception_type alone unless the event also carries an
    // $exception_list with structured frames — before this, every crash
    // reported a message with no functions and unrelated crashes merged.
    // The shape mirrors the posthog SDKs' serialisation (Dasher-Windows#116,
    // verified against the SDK source); stack_trace and app_version stay in
    // properties as raw evidence for the detail pane.
    const std::string message = exception_message(env.exception_type, env.stack_trace);
    props["$exception_type"] = env.exception_type;
    props["$exception_message"] = message;
    props["$exception_list"] = build_exception_list(env.exception_type, message, env.stack_trace);
    // Replay of a previous session's crash file — matches the Android
    // frontend's deferred flag (issue #114).
    if (deferred) props["deferred"] = "true";
    ensure_worker();
    enqueue("$exception", props);
}

std::vector<std::string> AnalyticsClient::parse_exception_frames(const std::string& stack_trace) {
    std::vector<std::string> frames;
    std::size_t pos = 0;
    while (pos != std::string::npos && frames.size() < kMaxExceptionFrames) {
        const std::size_t eol = stack_trace.find('\n', pos);
        std::string line = stack_trace.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::string fn = frame_function_of_line(line); !fn.empty()) frames.push_back(std::move(fn));
        pos = eol == std::string::npos ? std::string::npos : eol + 1;
    }
    return frames;
}

std::string AnalyticsClient::exception_message(const std::string& exception_type, const std::string& stack_trace) {
    if (stack_trace.empty()) return exception_type; // signal records: type-only

    const std::size_t eol = stack_trace.find('\n');
    std::string first = stack_trace.substr(0, eol == std::string::npos ? std::string::npos : eol);
    if (!first.empty() && first.back() == '\r') first.pop_back();

    // The message, when there is one, is saved as the first line — either
    // "Type: message" (the terminate handler follows ex.ToString()) or a
    // free-form line (the engine sticky-error report). A first line that
    // parses as a frame means there is no message line at all.
    if (frame_function_of_line(first).empty() && !first.empty()) {
        if (first.rfind(exception_type, 0) == 0) {
            std::string rest = first.substr(exception_type.size());
            const std::size_t colon = rest.find(':');
            if (colon != std::string::npos) rest = rest.substr(colon + 1);
            const std::size_t text = rest.find_first_not_of(' ');
            if (text != std::string::npos) return rest.substr(text);
            // "Type:" with an empty message — fall through to the frame
            // fallback rather than reporting an empty string.
        } else {
            return first;
        }
    }

    // No message text: "<type>: <top frame>" keeps grouping and the
    // $exception_functions column informative (issue #114's minimum).
    // Skip the crash machinery's OWN leading frames — terminate_handler /
    // __cxa_throw name the reporter, not the throw site (review r1: the
    // first USER frame below them is the useful signal).
    const std::vector<std::string> frames = parse_exception_frames(stack_trace);
    for (const std::string& f : frames) {
        if (f.rfind("terminate_handler", 0) == 0 || f.rfind("__cxa_throw", 0) == 0 || f.rfind("std::terminate", 0) == 0)
            continue;
        return exception_type + ": " + f;
    }
    if (!frames.empty()) return exception_type + ": " + frames.front();
    return exception_type;
}

std::string AnalyticsClient::build_exception_list(const std::string& exception_type, const std::string& value,
                                                  const std::string& stack_trace) {
    std::string frames_json;
    for (const std::string& fn : parse_exception_frames(stack_trace)) {
        if (!frames_json.empty()) frames_json += ",";
        // filename/lineno are omitted: backtrace_symbols() carries neither,
        // and symbolication is out of scope (RFC 0009). platform/lang are
        // Required per PostHog's manual-capture schema (review r1: without
        // them ingestion may degrade the frames — silently restoring the
        // type-only grouping this PR exists to fix); the Windows #116
        // envelope carries the same pair.
        frames_json += "{\"platform\":\"custom\",\"lang\":\"native\",\"function\":\"" + json_escape(fn) + "\"}";
    }
    // Single exception per record. mechanism.handled=false — it reached
    // std::terminate (or a fatal signal), so nothing caught it.
    return "[{\"type\":\"" + json_escape(exception_type) + "\",\"value\":\"" + json_escape(value) +
           "\",\"mechanism\":{\"handled\":false,\"type\":\"generic\"},"
           "\"stacktrace\":{\"type\":\"raw\",\"frames\":[" +
           frames_json + "]}}]";
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
        // PostHog control flags must be JSON booleans, not strings. Restricted
        // to a known set of boolean control properties: a broad $-prefix check
        // would retype textual fields such as $exception_type or
        // $exception_message whose value happens to be exactly "true"/"false",
        // so all other properties (user data included) stay strings. #114 adds
        // "deferred" to the set so the replayed-crash flag matches the Android
        // frontend's boolean true, not the string "true".
        const bool boolean_flag = kv.first == "$geoip_disable" || kv.first == "deferred";
        const bool as_bool = boolean_flag && (kv.second == "true" || kv.second == "false");
        // #114: $exception_list is pre-serialised JSON — an array built by
        // build_exception_list, never from user input — and must be emitted
        // verbatim so Error Tracking receives structured frames rather than a
        // stringified copy. An empty value can only be a bug; degrade to an
        // empty array rather than emitting invalid JSON.
        const bool raw_json = kv.first == "$exception_list";
        body += "\"" + json_escape(kv.first) + "\":";
        if (raw_json)
            body += kv.second.empty() ? "[]" : kv.second;
        else
            body += as_bool ? kv.second : "\"" + json_escape(kv.second) + "\"";
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
