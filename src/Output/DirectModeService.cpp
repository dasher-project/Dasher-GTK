#include "DirectModeService.h"
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// Seconds a single ydotool invocation may run before we kill it. Bounding
// every command bounds the worker's join() during shutdown too — a wedged
// daemon must never be able to stop Dasher from exiting.
constexpr int kCommandTimeoutSeconds = 10;

// Run a shell command quietly and report whether it exited 0 within
// timeout_seconds. On POSIX, fork/exec in its own process group and kill the
// whole group on timeout (std::system would block forever on a hung child);
// on Windows, where keyboard mode is unavailable anyway (no ydotool), the
// plain system() call fails fast and is fine.
bool run_command(const std::string& cmd, int timeout_seconds = kCommandTimeoutSeconds) {
#ifdef _WIN32
    (void)timeout_seconds;
    return std::system(cmd.c_str()) == 0;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Own process group so a timeout kill takes the shell AND the
        // ydotool child, not just the shell.
        setpgid(0, 0);
        if (!freopen("/dev/null", "r", stdin)) _exit(127);
        if (!freopen("/dev/null", "w", stdout)) _exit(127);
        if (!freopen("/dev/null", "w", stderr)) _exit(127);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127); // exec failed
    }

    int status = 0;
    const long deadline_us = static_cast<long>(timeout_seconds) * 1000000L;
    long waited_us = 0;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (waited_us >= deadline_us) {
            kill(-pid, SIGKILL); // the whole group
            kill(pid, SIGKILL);  // in case setpgid raced
            waitpid(pid, &status, 0);
            return false;
        }
        usleep(20 * 1000);
        waited_us += 20 * 1000;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

std::string shell_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() * 2);
    for (char c : text) {
        if (c == '\'' || c == '\\' || c == '"' || c == '$' || c == '`' || c == '!' || c == '\n') {
            escaped += '\\';
        }
        if (c != '\n' && c != '\r') {
            escaped += c;
        }
    }
    return escaped;
}

} // namespace

DirectModeService::DirectModeService() {
    m_available = probe_ydotool();
    m_worker = std::thread(&DirectModeService::worker_loop, this);
}

DirectModeService::~DirectModeService() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        // Never fire into a half-destroyed owner.
        m_failure_callback = nullptr;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

bool DirectModeService::is_available() const {
    return m_available;
}

bool DirectModeService::recheck() {
    // Probe outside the lock (it blocks on a process), then commit the verdict
    // with a generation bump: any in-flight job from before this recheck that
    // later reports failure is stale and must not clobber the new verdict.
    const bool available = probe_ydotool();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_available = available;
        ++m_generation;
    }
    return available;
}

bool DirectModeService::probe_ydotool() {
    // The binary alone isn't enough: ydotool talks to the ydotoold daemon over
    // a socket, and distros like Arch install the binary without enabling the
    // service. A relative move of (0, 0) exercises the whole path — socket,
    // daemon, uinput permissions — without moving the pointer. Tighter
    // timeout than jobs: recheck() runs on the UI thread (Retry button), so a
    // wedged daemon must not freeze the dialog for long.
    return run_command("which ydotool >/dev/null 2>&1", 2) && run_command("ydotool mousemove 0 0 >/dev/null 2>&1", 5);
}

bool DirectModeService::inject_text(const std::string& text) {
    if (!m_available || text.empty()) return m_available;

    DirectModeJob job;
    job.is_delete = false;
    job.text = text;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.generation = m_generation;
        m_queue.push(std::move(job));
    }
    m_cv.notify_one();
    return true;
}

bool DirectModeService::inject_delete(const std::string& deleted_text) {
    if (!m_available || deleted_text.empty()) return m_available;

    DirectModeJob job;
    job.is_delete = true;
    job.text = deleted_text;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.generation = m_generation;
        m_queue.push(std::move(job));
    }
    m_cv.notify_one();
    return true;
}

void DirectModeService::set_failure_callback(FailureCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failure_callback = std::move(callback);
}

bool DirectModeService::run_job(const DirectModeJob& job) {
    if (job.is_delete) {
        for (int i = 0; i < utf8_length(job.text); i++) {
            if (!run_command("ydotool key 14:1 14:0 >/dev/null 2>&1")) return false;
        }
        return true;
    }

    if (job.text == "\n" || job.text == "\r") {
        return run_command("ydotool key 28:1 28:0 >/dev/null 2>&1");
    }
    if (job.text == "\t") {
        return run_command("ydotool key 15:1 15:0 >/dev/null 2>&1");
    }

    const std::string escaped = shell_escape(job.text);
    if (escaped.empty()) return true;
    return run_command("ydotool type -- '" + escaped + "' >/dev/null 2>&1");
}

void DirectModeService::worker_loop() {
    while (true) {
        DirectModeJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return m_stopping || !m_queue.empty(); });
            // On shutdown, abandon whatever is still queued: draining it would
            // block the joining GTK main thread behind every remaining ydotool
            // process. The join is then bounded by the one in-flight command
            // (run_command kills it after kCommandTimeoutSeconds).
            if (m_stopping) return;
            job = std::move(m_queue.front());
            m_queue.pop();
        }

        if (!run_job(job)) {
            // A failure only counts if the job was queued under the current
            // availability verdict: jobs queued before a successful recheck()
            // (Retry) carry an older generation and must not disable the
            // recovered service or re-fire the failure callback.
            FailureCallback callback;
            bool stale = true;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                stale = job.generation != m_generation;
                if (!stale) {
                    m_available = false;
                    // Drop any queued jobs (they'd fail against a dead daemon).
                    std::queue<DirectModeJob> empty;
                    std::swap(m_queue, empty);
                    callback = m_failure_callback;
                }
            }
            if (!stale && callback) callback();
            // Do NOT exit: the worker must survive so a later recheck()
            // (setup dialog Retry) can resume injection. With m_available
            // false, inject_* stop queueing, so the loop parks on the
            // condition variable until then.
            continue;
        }
    }
}

int DirectModeService::utf8_length(const std::string& text) {
    int count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) count++; // count UTF-8 lead bytes
    }
    return count;
}
