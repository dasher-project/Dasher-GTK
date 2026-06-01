#include "DirectModeService.h"
#include <cstdlib>
#include <cstring>

DirectModeService::DirectModeService() {
    m_available = check_ydotool_installed();
}

bool DirectModeService::is_available() const {
    return m_available;
}

bool DirectModeService::check_ydotool_installed() {
    return std::system("which ydotool >/dev/null 2>&1") == 0;
}

void DirectModeService::inject_text(const std::string& text) {
    if (!m_available || text.empty()) return;

    if (text == "\n" || text == "\r") {
        std::system("ydotool key 28:1 28:0 >/dev/null 2>&1");
        return;
    }

    if (text == "\t") {
        std::system("ydotool key 15:1 15:0 >/dev/null 2>&1");
        return;
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

    if (escaped.empty()) return;

    std::string cmd = "ydotool type -- '" + escaped + "' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

void DirectModeService::inject_delete(int count) {
    if (!m_available || count <= 0) return;

    for (int i = 0; i < count; i++) {
        std::system("ydotool key 14:1 14:0 >/dev/null 2>&1");
    }
}
