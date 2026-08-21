#include "DirectModeService.h"
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

// Run a shell command quietly and report whether it exited 0. std::system
// returns a wait status on POSIX, not an exit code, so decode it (on Windows
// it already is the exit code). Blocking — only call from the worker thread
// (or once at construction for the availability probe).
bool run_command(const std::string& cmd) {
    const int status = std::system(cmd.c_str());
#ifdef _WIN32
    return status == 0;
#else
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
    m_available = probe_ydotool();
    return m_available;
}

bool DirectModeService::probe_ydotool() {
    // The binary alone isn't enough: ydotool talks to the ydotoold daemon over
    // a socket, and distros like Arch install the binary without enabling the
    // service. A relative move of (0, 0) exercises the whole path — socket,
    // daemon, uinput permissions — without moving the pointer.
    return run_command("which ydotool >/dev/null 2>&1") && run_command("ydotool mousemove 0 0 >/dev/null 2>&1");
}

bool DirectModeService::inject_text(const std::string& text) {
    if (!m_available || text.empty()) return m_available;

    Job job;
    job.is_delete = false;
    job.text = text;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(job));
    }
    m_cv.notify_one();
    return true;
}

bool DirectModeService::inject_delete(const std::string& deleted_text) {
    if (!m_available || deleted_text.empty()) return m_available;

    Job job;
    job.is_delete = true;
    job.text = deleted_text;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(job));
    }
    m_cv.notify_one();
    return true;
}

void DirectModeService::set_failure_callback(FailureCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failure_callback = std::move(callback);
}

bool DirectModeService::run_job(const Job& job) {
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
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return m_stopping || !m_queue.empty(); });
            // On shutdown, abandon whatever is still queued: draining it would
            // block the joining GTK main thread behind every remaining ydotool
            // process. The join is then bounded by the one in-flight command.
            if (m_stopping) return;
            job = std::move(m_queue.front());
            m_queue.pop();
        }

        if (!run_job(job)) {
            m_available = false;
            // Drop any queued jobs: they'd fail against a dead daemon anyway.
            std::unique_lock<std::mutex> lock(m_mutex);
            std::queue<Job> empty;
            std::swap(m_queue, empty);
            FailureCallback callback = m_failure_callback;
            lock.unlock();
            if (callback) callback();
            return;
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
