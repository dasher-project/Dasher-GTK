#include "CrashReporter.h"

#include "EngineLogRingBuffer.h"
#include "PiiScrubber.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

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
// #114: execinfo's backtrace()/backtrace_symbols() are what the terminate
// handler uses to capture real frames. glibc and macOS provide it; musl and
// MSVC do not — feature-test rather than platform-guess so all four keep
// building, with the no-execinfo builds degrading to a message line and zero
// frames (AnalyticsClient::parse_exception_frames handles the empty case).
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define DASHER_HAVE_EXECINFO 1
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

// --- #114: frame capture for the terminate path ----------------------------
// Issue #114: the handler used to save e.what() as stack_trace — a message,
// not a stack — so no crash report ever carried a function name. Everything
// below exists to turn one backtrace_symbols() blob into the canonical
// "module(function+0xoff) [0xaddr]" text the next-launch envelope builder
// (AnalyticsClient::parse_exception_frames) can parse back into frames.

#if defined(DASHER_HAVE_EXECINFO)
// Matches the 50-frame cap PostHog's SDKs apply per exception.
constexpr int kCaptureDepth = 50;
#endif

std::string normalise_macos_line(const std::string& line) {
    // macOS renders frames as "<n>  <module>  <0xaddr>  <mangled> + <off>"
    // (whitespace-separated; mangled names never contain whitespace), so
    // tokenise and re-render in the canonical grammar. Anything that does not
    // match exactly (symbol-less frames print the bare address as the fourth
    // token) is returned untouched; the parser skips lines it cannot read.
    std::vector<std::string> tok;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i > start) tok.push_back(line.substr(start, i - start));
    }
    if (tok.size() < 6 || tok[4] != "+" || tok[2].rfind("0x", 0) != 0 || tok[3].rfind("0x", 0) == 0) return line;
    for (char c : tok[0])
        if (!std::isdigit(static_cast<unsigned char>(c))) return line;
    // Offset: macOS prints it bare ("main+32"); the canonical grammar and
    // the parser both expect "+0x…" (review r1: bare offsets leak into the
    // function name as "main+32" instead of "main").
    const std::string off = tok[5].rfind("0x", 0) == 0 ? tok[5] : "0x" + tok[5];
    // UnsSymbolised-mangled? tok[3] starts 0x → the symbol slot IS an
    // address (macOS prints the unsymbolised form with the address there);
    // keep it like glibc's "module(+0xoff)" rather than dropping the frame.
    if (tok[3].rfind("0x", 0) == 0) return tok[1] + "(+" + off + ") [" + tok[2] + "]";
    return tok[1] + "(" + demangle(tok[3].c_str()) + "+" + off + ") [" + tok[2] + "]";
}

// Demangle one backtrace_symbols() line in place, keeping (or converting to)
// the canonical "module(function+0xoff) [0xaddr]" grammar. glibc prints that
// shape directly; macOS prints the numbered form and is normalised above.
// Mangled names cannot contain '+', so splitting on "+0x" is unambiguous.
std::string demangle_frame_line(const char* raw) {
    if (!raw) return "";
    const std::string line(raw);
    const std::size_t tail = line.rfind(") [0x");
    if (tail == std::string::npos) return normalise_macos_line(line);
    // The grammar's '(' is the FIRST one — demangled names carry their own
    // parentheses ("ns::f(int)"), so a backwards search would split inside
    // the signature.
    const std::size_t open = line.find('(');
    if (open == std::string::npos || open >= tail || tail <= open + 1) return line;
    const std::string symbol = line.substr(open + 1, tail - open - 1);
    const std::size_t plus = symbol.find("+0x");
    if (plus == 0) return line; // "module(+0xoff)": unsymbolised, nothing to demangle
    const std::string name = plus == std::string::npos ? symbol : symbol.substr(0, plus);
    const std::string demangled = demangle(name.c_str());
    if (demangled == name) return line; // C symbol or already plain
    return line.substr(0, open + 1) + demangled + line.substr(tail);
}

// #114: the frame-capture wrapper. Deliberately thin — everything testable
// (parsing, capping, envelope building) lives in AnalyticsClient as pure
// functions; this only walks the real stack, which cannot be exercised
// headlessly. backtrace_symbols() mallocs: fine here, because the terminate
// handler runs in normal context, not a signal handler (if the heap is
// already dead the call may abort — std::abort inside std::terminate is
// legal, and g_crash_written stops the SIGABRT shim overwriting anything).
std::string capture_backtrace_text() {
#if defined(DASHER_HAVE_EXECINFO)
    void* addresses[kCaptureDepth] = {nullptr};
    const int n = backtrace(addresses, kCaptureDepth);
    if (n <= 0) return "";
    char** symbols = backtrace_symbols(addresses, n);
    if (!symbols) return "";
    std::string out;
    for (int i = 0; i < n; ++i) {
        out += demangle_frame_line(symbols[i]);
        out += '\n';
    }
    std::free(symbols);
    return out;
#else
    return ""; // musl/Windows: the record keeps the message line, zero frames
#endif
}

#if defined(__unix__) || defined(__APPLE__)
// --- async-signal-safe crash path -----------------------------------------
// Everything the signal handler touches is prepared here, in normal context.

constexpr std::size_t kPathMax = 4096;
char g_crash_path_buf[kPathMax] = {0};

// #114 frames decision: the signal path stays frame-less, on purpose.
// glibc's backtrace(3) is async-signal-safe, but backtrace_symbols() and
// abi::__cxa_demangle() both malloc, so rendering names inside the handler
// would need heap work in async context. The write(2)-only alternative —
// dumping raw frame addresses — carries no function names at all, and this
// client cannot symbolicate later (no addr2line pipeline, per RFC 0009), so
// the frames would fill $exception_list with uninformative hex. The
// terminate path runs in normal context and captures real demangled frames;
// signal reports remain type-only ("Signal:SIGSEGV"), which still groups.
constexpr std::size_t kSignalCount = 4; // SIGSEGV, SIGABRT, SIGILL, SIGBUS
struct SignalPayload {
    int sig;
    const char* bytes;
    std::size_t len;
};
// Filled at install(); the handler only reads these (async-signal-safe).
std::string g_payload_storage[kSignalCount];
SignalPayload g_payloads[kSignalCount];
std::size_t g_payload_count = 0;

const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGILL:
        return "SIGILL";
    case SIGBUS:
        return "SIGBUS";
    default:
        return "SIGNAL";
    }
}

// Minimal crash record for a signal: empty stack (native symbolication is out
// of scope, RFC 0009; see the #114 frames decision above) and empty tail
// (recovered from engine.log next launch).
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

    // #114: capture the real stack BEFORE the rethrow/catch dance below can
    // add its own frames on top. The frames represent the TERMINATE path:
    // for an uncaught exception the throw site is still on the stack (the
    // runtime never unwound it), so beneath this handler, __cxa_throw and
    // the runtime's terminate machinery sit the frames of whoever threw —
    // exactly what a crash report wants to name. A bare std::terminate()
    // call (no active exception) shows its caller instead.
    //
    // Exception safety (review r1): the capture does ~50 heap ops (string
    // appends, __cxa_demangle); a bad_alloc escaping here would skip the
    // crash-file write AND leave g_crash_written set, suppressing the
    // signal shim's fallback — no report at all, in exactly the OOM class
    // this handler exists for. Frames are best-effort: on ANY throw, keep
    // the message-only envelope and still write the file.
    std::string frames;
    try {
        frames = capture_backtrace_text();
    } catch (...) {
        frames.clear();
    }

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
            // #114: ex.ToString()-style message line ("<type>: <what>") first,
            // so the next launch can recover the message for
            // $exception_message; the captured frames follow it.
            env.stack_trace = env.exception_type + ": " + e.what();
        } catch (...) {
            env.exception_type = "unknown exception";
        }
    }
    if (!frames.empty()) {
        if (!env.stack_trace.empty()) env.stack_trace += '\n';
        env.stack_trace += frames;
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
    // #114: SIGBUS joins the set — bus errors (mmap'd file truncated under us,
    // alignment faults) are the same never-reaches-std::terminate crash class
    // as SIGSEGV and were the one signal named in the issue still missing.
    const int sigs[kSignalCount] = {SIGSEGV, SIGABRT, SIGILL, SIGBUS};
    g_payload_count = kSignalCount;
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

CrashEnvelope CrashReporter::make_envelope(const std::string& exception_type, const std::string& source) {
    CrashEnvelope env;
    env.exception_type = exception_type;
    env.source = source;
    env.app_version = DASHER_GTK_VERSION;
    env.os_version = os_version();
    return env;
}

} // namespace analytics
