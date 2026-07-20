#pragma once

#include <cstddef>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>

namespace analytics {

// A fixed-capacity ring buffer of the most recent engine log lines, fed from
// DasherCore's log callback (see DasherBridge::log_callback_trampoline). Per
// RFC 0009 it keeps the last N lines in memory for the std::terminate crash
// path and mirrors them to an on-disk file so a hard SIGSEGV/SIGABRT (whose
// handler cannot safely lock or allocate) can still recover the tail on the
// next launch.
//
// The class is parameterised (capacity + mirror path) so it can be unit-tested
// against a temporary file; the running app uses engine_log_buffer() below.
class EngineLogRingBuffer {
  public:
    static constexpr std::size_t kDefaultMaxLines = 64;
    static constexpr std::size_t kDefaultMaxBytes = 8 * 1024;

    // mirror_path may be empty to disable the on-disk mirror (used in tests
    // that only exercise the in-memory buffer). A non-empty path is truncated
    // on construction so each session starts fresh.
    EngineLogRingBuffer(std::string mirror_path, std::size_t max_lines = kDefaultMaxLines,
                        std::size_t max_bytes = kDefaultMaxBytes);

    // level maps DasherCore's 0=debug/1=info/2=warning/3=error to a single-char
    // prefix ([D]/[I]/[W]/[E], [X] for anything else). Thread-safe.
    void append(int level, const std::string& message);

    // The buffered lines joined by '\n', oldest first. Thread-safe.
    std::string snapshot() const;

  private:
    void evict_locked();

    mutable std::mutex m_mutex;
    std::deque<std::string> m_lines;
    std::size_t m_bytes = 0;
    const std::size_t m_max_lines;
    const std::size_t m_max_bytes;
    std::string m_mirror_path;
    std::ofstream m_mirror;
};

// Process-wide singleton used by the app, mirroring to
// <XDG_DATA_HOME>/dasher/engine.log. Constructed on first use (which happens
// after CrashReporter::flush_pending has already read the previous session's
// mirror file, so truncation here does not race that recovery).
EngineLogRingBuffer& engine_log_buffer();

// Absolute path of the singleton's mirror file. Safe to call before the buffer
// exists (CrashReporter reads it to recover a signal-path crash's log tail).
std::string engine_log_mirror_path();

} // namespace analytics
