#include "CrashReporter.h"

#include "EngineLogRingBuffer.h"
#include "PiiScrubber.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <typeinfo>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if defined(__has_include)
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define DASHER_HAVE_CXXABI 1
#endif
#endif

#ifndef DASHER_GTK_VERSION
#define DASHER_GTK_VERSION "0.0.0-dev"
#endif

namespace analytics {

namespace {

constexpr char kEngineLogSentinel[] = "--- engine log ---";

// Set once a crash file has been written, so the SIGABRT raised by std::abort()
// at the end of the terminate handler does not overwrite the rich record with a
// minimal one. sig_atomic_t is safe to touch from an async signal handler.
volatile std::sig_atomic_t g_crash_written = 0;

std::string os_version() {
#if defined(__unix__) || defined(__APPLE__)
    struct utsname u {};
    if (uname(&u) == 0) {
        return std::string(u.sysname) + " " + u.release;
    }
#endif
    return "unknown";
}

std::string demangle(const char* name) {
#if defined(DASHER_HAVE_CXXABI)
    int status = 0;
    char* out = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && out) {
        std::string result = out;
        std::free(out);
        return result;
    }
#endif
    return name ? name : "unknown";
}

#if defined(__unix__) || defined(__APPLE__)
// --- async-signal-safe crash path -----------------------------------------
// Everything the signal handler touches is prepared here, in normal context.

constexpr std::size_t kPathMax = 4096;
char g_crash_path_buf[kPathMax] = {0};

struct SignalPayload {
    int sig;
    const char* bytes;
    std::size_t len;
};
// Filled at install(); the handler only reads these (async-signal-safe).
std::string g_payload_storage[3];
SignalPayload g_payloads[3];
std::size_t g_payload_count = 0;

const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGILL:
        return "SIGILL";
    default:
        return "SIGNAL";
    }
}

// Minimal crash record for a signal: empty stack (native symbolication is out
// of scope, RFC 0009) and empty tail (recovered from engine.log next launch).
std::string build_signal_payload(int sig, const std::string& app_ver, const std::string& os_ver) {
    std::string p;
    p += "exception_type=Signal:";
    p += signal_name(sig);
    p += "\nsource=signal_handler\napp_version=";
    p += app_ver;
    p += "\nos_version=";
    p += os_ver;
    p += "\n\n"; // end of header (blank line) + empty stack section
    p += kEngineLogSentinel;
    p += "\n"; // empty engine log tail
    return p;
}

extern "C" void crash_signal_handler(int sig) {
    if (!g_crash_written) {
        g_crash_written = 1;
        for (std::size_t i = 0; i < g_payload_count; ++i) {
            if (g_payloads[i].sig == sig) {
                int fd = open(g_crash_path_buf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd >= 0) {
                    std::size_t off = 0;
                    while (off < g_payloads[i].len) {
                        ssize_t n = write(fd, g_payloads[i].bytes + off, g_payloads[i].len - off);
                        if (n <= 0) break;
                        off += static_cast<std::size_t>(n);
                    }
                    close(fd);
                }
                break;
            }
        }
    }
    // Restore the default disposition and re-raise so the process still
    // terminates (and can dump core) as it normally would.
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif // unix

// --- rich crash path (std::set_terminate) ----------------------------------

std::terminate_handler g_previous_terminate = nullptr;

void terminate_handler() {
    // Prevent the SIGABRT from the eventual abort() below (and any reentrancy)
    // from overwriting the record we are about to write.
    g_crash_written = 1;

    CrashEnvelope env;
    env.source = "std::terminate";
    env.app_version = DASHER_GTK_VERSION;
    env.os_version = os_version();
    env.exception_type = "std::terminate";

    if (std::exception_ptr ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            env.exception_type = demangle(typeid(e).name());
            env.stack_trace = e.what();
        } catch (...) {
            env.exception_type = "unknown exception";
        }
    }

    env.stack_trace = PiiScrubber::truncate(PiiScrubber::scrub(env.stack_trace), PiiScrubber::kStackTraceCap);
    env.engine_log_tail =
        PiiScrubber::truncate(PiiScrubber::scrub(engine_log_buffer().snapshot()), PiiScrubber::kEngineLogTailCap);

    CrashReporter::write_crash_file(CrashReporter::crash_file_path(), env);

    if (g_previous_terminate) {
        g_previous_terminate();
    }
    std::abort();
}

} // namespace

std::string CrashReporter::crash_file_path() {
    char* dir = g_build_filename(g_get_user_data_dir(), "dasher", nullptr);
    g_mkdir_with_parents(dir, 0700);
    char* path = g_build_filename(dir, "pending_crash.txt", nullptr);
    std::string result = path ? path : "";
    g_free(dir);
    g_free(path);
    return result;
}

bool CrashReporter::write_crash_file(const std::string& path, const CrashEnvelope& env) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) return false;
    f << "exception_type=" << env.exception_type << "\n";
    f << "source=" << env.source << "\n";
    f << "app_version=" << env.app_version << "\n";
    f << "os_version=" << env.os_version << "\n";
    f << "\n"; // blank line terminates the header
    f << env.stack_trace;
    if (!env.stack_trace.empty() && env.stack_trace.back() != '\n') f << "\n";
    f << kEngineLogSentinel << "\n";
    f << env.engine_log_tail;
    return static_cast<bool>(f);
}

bool CrashReporter::parse_crash_file(const std::string& path, CrashEnvelope& out) {
    std::ifstream f(path);
    if (!f) return false;

    out = CrashEnvelope{};
    enum { HEADER, STACK, TAIL } phase = HEADER;
    std::string line;
    std::string stack, tail;
    bool first_stack = true, first_tail = true;

    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (phase == HEADER) {
            if (line.empty()) {
                phase = STACK;
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if (key == "exception_type")
                out.exception_type = val;
            else if (key == "source")
                out.source = val;
            else if (key == "app_version")
                out.app_version = val;
            else if (key == "os_version")
                out.os_version = val;
        } else if (phase == STACK) {
            if (line == kEngineLogSentinel) {
                phase = TAIL;
                continue;
            }
            if (!first_stack) stack += '\n';
            stack += line;
            first_stack = false;
        } else { // TAIL
            if (!first_tail) tail += '\n';
            tail += line;
            first_tail = false;
        }
    }

    out.stack_trace = stack;
    out.engine_log_tail = tail;
    return true;
}

void CrashReporter::install() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    g_previous_terminate = std::set_terminate(&terminate_handler);

#if defined(__unix__) || defined(__APPLE__)
    const std::string path = crash_file_path();
    std::strncpy(g_crash_path_buf, path.c_str(), kPathMax - 1);

    const std::string app_ver = DASHER_GTK_VERSION;
    const std::string os_ver = os_version();
    const int sigs[] = {SIGSEGV, SIGABRT, SIGILL};
    g_payload_count = sizeof(sigs) / sizeof(sigs[0]);
    for (std::size_t i = 0; i < g_payload_count; ++i) {
        g_payload_storage[i] = build_signal_payload(sigs[i], app_ver, os_ver);
        g_payloads[i].sig = sigs[i];
        g_payloads[i].bytes = g_payload_storage[i].data();
        g_payloads[i].len = g_payload_storage[i].size();
    }

    struct sigaction sa {};
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (std::size_t i = 0; i < g_payload_count; ++i) {
        sigaction(sigs[i], &sa, nullptr);
    }
#endif
}

void CrashReporter::flush_pending(const std::function<void(const CrashEnvelope&)>& sink) {
    const std::string path = crash_file_path();

#if defined(__unix__) || defined(__APPLE__)
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return; // no crash file
    const long age = static_cast<long>(g_get_real_time() / G_USEC_PER_SEC) - static_cast<long>(st.st_mtime);
    if (age > kMaxAgeSeconds) {
        g_unlink(path.c_str());
        return;
    }
#else
    if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) return;
#endif

    CrashEnvelope env;
    if (parse_crash_file(path, env)) {
        // Signal-path records carry no tail; recover it from the mirrored log.
        if (env.engine_log_tail.empty()) {
            std::ifstream log(engine_log_mirror_path(), std::ios::binary);
            if (log) {
                std::stringstream ss;
                ss << log.rdbuf();
                std::string tail = ss.str();
                tail = PiiScrubber::truncate(PiiScrubber::scrub(tail), PiiScrubber::kEngineLogTailCap);
                env.engine_log_tail = tail;
            }
        }
        if (sink) sink(env);
    }
    g_unlink(path.c_str());
}

} // namespace analytics
