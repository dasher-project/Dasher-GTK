#include "DirectModeService.h"
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

// Run a shell command quietly and report whether it exited 0. std::system
// returns a wait status on POSIX, not an exit code, so decode it (on Windows
// it already is the exit code).
bool run_command(const std::string& cmd) {
    const int status = std::system(cmd.c_str());
#ifdef _WIN32
    return status == 0;
#else
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

} // namespace

DirectModeService::DirectModeService() {
    m_available = probe_ydotool();
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

    if (text == "\n" || text == "\r") {
        return run_command("ydotool key 28:1 28:0 >/dev/null 2>&1");
    }

    if (text == "\t") {
        return run_command("ydotool key 15:1 15:0 >/dev/null 2>&1");
    }

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

    if (escaped.empty()) return true;

    // No `&` backgrounding: we need the exit status to tell the UI when
    // injection broke mid-session (daemon stopped, permissions lost).
    std::string cmd = "ydotool type -- '" + escaped + "' >/dev/null 2>&1";
    const bool ok = run_command(cmd);
    if (!ok) m_available = false;
    return ok;
}

bool DirectModeService::inject_delete(const std::string& deleted_text) {
    if (!m_available || deleted_text.empty()) return m_available;

    for (int i = 0; i < utf8_length(deleted_text); i++) {
        if (!run_command("ydotool key 14:1 14:0 >/dev/null 2>&1")) {
            m_available = false;
            return false;
        }
    }
    return true;
}

int DirectModeService::utf8_length(const std::string& text) {
    int count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) count++; // count UTF-8 lead bytes
    }
    return count;
}
