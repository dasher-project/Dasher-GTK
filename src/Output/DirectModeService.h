#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// Types Dasher's output into whichever window currently has keyboard focus,
// via ydotool/uinput. Mirrors what Dasher-Windows does with SendInput and
// Dasher-Apple with CGEvent posting.
//
// Commands run on a single worker thread: spawning a ydotool process and
// waiting on the ydotoold daemon round-trip must never happen on the GTK
// frame loop (a slow daemon would hitch rendering on every character), and a
// single queue keeps insert/delete ordering stable.
// One queued keyboard-mode command: either text to type or text that was
// deleted (one backspace per character). Namespace scope rather than a
// nested private struct so clang-format versions agree on the layout.
struct DirectModeJob {
    bool is_delete = false;
    std::string text;
};

class DirectModeService {
  public:
    using FailureCallback = std::function<void()>;

    DirectModeService();
    ~DirectModeService();

    DirectModeService(const DirectModeService&) = delete;
    DirectModeService& operator=(const DirectModeService&) = delete;

    // Queue `text` for injection into the focused window. Returns false when
    // the service already knows injection is broken; a failure discovered
    // later (mid-session daemon death) arrives via the failure callback.
    bool inject_text(const std::string& text);

    // Queue one backspace per character (not byte) of `deleted_text`.
    bool inject_delete(const std::string& deleted_text);

    bool is_available() const;
    // Re-run the ydotool availability check (e.g. after the user installs it
    // via the setup dialog, issue #38) and update the cached result. The
    // worker survives injection failures precisely so this can resume the
    // service after a Retry — recheck() alone puts it back in business.
    bool recheck();

    // Called from the worker thread whenever an injection fails mid-session
    // (daemon died, permissions lost). Receivers must marshal back to the UI
    // thread (e.g. Glib::signal_idle) and be prepared for the call to race
    // teardown — see MainWindow's alive-flag pattern.
    void set_failure_callback(FailureCallback callback);

    // Number of UTF-8 code points in `text` (lead-byte count). Injected
    // backspaces must match characters, or multibyte output over-deletes.
    static int utf8_length(const std::string& text);

  private:
    // True when the ydotool binary exists AND the daemon answers. Arch-family
    // distros install the binary without enabling ydotoold, which previously
    // made is_available() lie — `which ydotool` alone is not enough.
    static bool probe_ydotool();

    bool run_job(const DirectModeJob& job);
    void worker_loop();

    // Injected while the worker is mid-job. Recheck() bumps the generation so
    // a failure result from an older attempt (e.g. one still completing when
    // the user pressed Retry) can't clobber a newer availability verdict.
    // Guarded by m_mutex.
    uint64_t m_generation = 0;

    std::atomic<bool> m_available{false};
    std::queue<DirectModeJob> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    bool m_stopping = false;
    FailureCallback m_failure_callback;
};
