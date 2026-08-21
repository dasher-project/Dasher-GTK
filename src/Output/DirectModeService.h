#pragma once

#include <atomic>
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
  // Re-run the ydotool availability check (e.g. after the user installs it via
  // the setup dialog, issue #38) and update the cached result.
  bool recheck();

  // Called from the worker thread the first time an injection fails;
  // receivers must marshal back to the UI thread (e.g. Glib::signal_idle).
  void set_failure_callback(FailureCallback callback);

  // Number of UTF-8 code points in `text` (lead-byte count). Injected
  // backspaces must match characters, or multibyte output over-deletes.
  static int utf8_length(const std::string& text);

private:
  struct Job {
      bool is_delete = false;
      std::string text;
  };

    // True when the ydotool binary exists AND the daemon answers. Arch-family
    // distros install the binary without enabling ydotoold, which previously
    // made is_available() lie — `which ydotool` alone is not enough.
    static bool probe_ydotool();

    bool run_job(const Job& job);
    void worker_loop();

    std::atomic<bool> m_available{false};
    std::queue<Job> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    bool m_stopping = false;
    FailureCallback m_failure_callback;
};
