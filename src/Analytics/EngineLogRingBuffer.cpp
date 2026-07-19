#include "EngineLogRingBuffer.h"

#include <glib.h>

namespace analytics {

namespace {

char level_prefix(int level) {
    switch (level) {
    case 0:
        return 'D';
    case 1:
        return 'I';
    case 2:
        return 'W';
    case 3:
        return 'E';
    default:
        return 'X';
    }
}

} // namespace

EngineLogRingBuffer::EngineLogRingBuffer(std::string mirror_path, std::size_t max_lines, std::size_t max_bytes)
    : m_max_lines(max_lines ? max_lines : 1), m_max_bytes(max_bytes ? max_bytes : 1),
      m_mirror_path(std::move(mirror_path)) {
    if (!m_mirror_path.empty()) {
        // Start each session with a fresh mirror file. std::ios::trunc so a
        // crash tail from a previous run isn't confused with this run's.
        m_mirror.open(m_mirror_path, std::ios::out | std::ios::trunc);
    }
}

void EngineLogRingBuffer::evict_locked() {
    while (!m_lines.empty() && (m_lines.size() > m_max_lines || m_bytes > m_max_bytes)) {
        m_bytes -= m_lines.front().size() + 1; // +1 for the joining newline
        m_lines.pop_front();
    }
}

void EngineLogRingBuffer::append(int level, const std::string& message) {
    std::string line = "[";
    line += level_prefix(level);
    line += "] ";
    line += message;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_bytes += line.size() + 1;
    m_lines.push_back(std::move(line));
    evict_locked();

    if (m_mirror.is_open()) {
        m_mirror << m_lines.back() << '\n';
        m_mirror.flush(); // survive a hard crash between now and the next line
    }
}

std::string EngineLogRingBuffer::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string out;
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        if (i) out += '\n';
        out += m_lines[i];
    }
    return out;
}

std::string engine_log_mirror_path() {
    // <XDG_DATA_HOME>/dasher/engine.log (g_get_user_data_dir honours the env).
    char* dir = g_build_filename(g_get_user_data_dir(), "dasher", nullptr);
    g_mkdir_with_parents(dir, 0700);
    char* path = g_build_filename(dir, "engine.log", nullptr);
    std::string result = path ? path : "";
    g_free(dir);
    g_free(path);
    return result;
}

EngineLogRingBuffer& engine_log_buffer() {
    static EngineLogRingBuffer instance(engine_log_mirror_path());
    return instance;
}

} // namespace analytics
