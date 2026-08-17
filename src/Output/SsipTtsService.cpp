#include "SsipTtsService.h"

// See the header: POSIX only, and the whole file compiles away on Windows so
// the globbed MSVC build does not trip over <sys/socket.h>.
#ifndef _WIN32

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

// SSIP is a line protocol over a unix socket. Replies are a 3-digit code
// followed by '-' on continuation lines and ' ' on the final line, so the
// end of a reply is unambiguous without knowing the command.
constexpr char CRLF[] = "\r\n";

std::string runtime_socket_path() {
    // speech-dispatcher 0.12 puts the per-session socket here. SPEECHD_SOCKET
    // overrides it, which is also how a Flatpak build would be pointed at a
    // socket bind-mounted into the sandbox.
    if (const char* explicit_path = std::getenv("SPEECHD_SOCKET")) return explicit_path;
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"))
        return std::string(runtime) + "/speech-dispatcher/speechd.sock";
    return {};
}

// A line consisting of a single '.' terminates message data, so a literal '.'
// at the start of a line has to be doubled. Dasher emits arbitrary user text,
// so this is not hypothetical.
std::string escape_message(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    bool at_line_start = true;
    for (char c : text) {
        if (at_line_start && c == '.') out.push_back('.');
        out.push_back(c);
        at_line_start = (c == '\n');
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : line) {
        if (c == '\t') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

} // namespace

SsipTtsService::SsipTtsService() {
    connect();
}

SsipTtsService::~SsipTtsService() {
    if (m_fd >= 0) command("QUIT");
    disconnect();
}

bool SsipTtsService::connect() {
    m_socket_path = runtime_socket_path();
    if (m_socket_path.empty()) {
        m_last_error = "no XDG_RUNTIME_DIR and no SPEECHD_SOCKET";
        return false;
    }

    m_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) {
        m_last_error = std::string("socket(): ") + std::strerror(errno);
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socket_path.size() >= sizeof(addr.sun_path)) {
        m_last_error = "socket path too long: " + m_socket_path;
        disconnect();
        return false;
    }
    std::memcpy(addr.sun_path, m_socket_path.c_str(), m_socket_path.size());

    if (::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        // No daemon. Unlike the FFI build this is a normal, recoverable state:
        // the app runs, the speech switch is just insensitive.
        m_last_error = "connect(" + m_socket_path + "): " + std::strerror(errno);
        disconnect();
        return false;
    }

    // SSIP requires the client to identify itself before anything else.
    int code = 0;
    if (!command("SET self CLIENT_NAME owen:dasher:main", nullptr, &code) || code != 208) {
        m_last_error = "CLIENT_NAME rejected (code " + std::to_string(code) + ")";
        disconnect();
        return false;
    }
    return true;
}

void SsipTtsService::disconnect() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool SsipTtsService::write_all(const std::string& data) const {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(m_fd, data.data() + written, data.size() - written);
        if (n <= 0) {
            m_last_error = std::string("write(): ") + std::strerror(errno);
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

bool SsipTtsService::command(const std::string& line, std::vector<std::string>* out, int* code) const {
    if (m_fd < 0) return false;
    if (!write_all(line + CRLF)) return false;
    return read_reply(out, code);
}

bool SsipTtsService::read_reply(std::vector<std::string>* out, int* code) const {
    // Read until a final line, i.e. one whose 4th character is a space rather
    // than '-'. Replies are small; a byte-at-a-time read keeps the framing
    // simple and avoids buffering state between calls.
    std::string buffer;
    for (;;) {
        char c = 0;
        const ssize_t n = ::read(m_fd, &c, 1);
        if (n <= 0) {
            m_last_error = "connection closed mid-reply";
            return false;
        }
        if (c == '\r') continue;
        if (c != '\n') {
            buffer.push_back(c);
            continue;
        }

        if (buffer.size() < 4) {
            m_last_error = "short reply line: " + buffer;
            return false;
        }
        const int this_code = std::atoi(buffer.substr(0, 3).c_str());
        const bool final_line = (buffer[3] == ' ');
        if (!final_line && out) out->push_back(buffer.substr(4));
        if (final_line) {
            if (code) *code = this_code;
            // 2xx/1xx are success; 3xx/4xx/5xx are errors.
            return this_code < 300;
        }
        buffer.clear();
    }
}

int SsipTtsService::to_ssip_scale(float v) {
    // Callers pass roughly -1..1 (the wrapper's range); SSIP wants -100..100.
    const int scaled = static_cast<int>(v * 100.0f);
    return std::max(-100, std::min(100, scaled));
}

void SsipTtsService::speak(const std::string& text) {
    if (m_fd < 0 || text.empty()) return;

    // Note 230, not the 202 the *module* protocol uses for the same step. The
    // client and module sides of speech-dispatcher have separate code spaces
    // and it is an easy hour to lose: getting this wrong leaves the daemon in
    // data-receiving mode, and the next command is silently eaten as message
    // text rather than answered.
    int code = 0;
    if (!command("SPEAK", nullptr, &code) || code != 230) {
        m_last_error = "SPEAK refused (code " + std::to_string(code) + ")";
        return;
    }

    if (!write_all(escape_message(text) + CRLF + "." + CRLF)) return;

    // "225-<message id>" then "225 OK MESSAGE QUEUED".
    std::vector<std::string> lines;
    if (!read_reply(&lines, &code)) m_last_error = "message rejected (code " + std::to_string(code) + ")";
}

void SsipTtsService::speak_sync(const std::string& text) {
    // Dasher only ever calls speak() on the UI thread; speak_sync exists for
    // interface parity. Blocking would need the event notifications
    // (701 BEGIN / 702 END), which nothing in Dasher currently consumes.
    speak(text);
}

void SsipTtsService::stop() {
    command("STOP self");
}

void SsipTtsService::pause() {
    command("PAUSE self");
}

void SsipTtsService::resume() {
    command("RESUME self");
}

void SsipTtsService::set_engine(const std::string& engine_id, const std::string& credentials) {
    // Credentials have no SSIP equivalent on purpose: under this model the
    // API keys live in the module's own configuration, not in every client.
    (void)credentials;
    if (command("SET self OUTPUT_MODULE " + engine_id)) m_engine_id = engine_id;
}

void SsipTtsService::set_voice(const std::string& voice_id) {
    command("SET self SYNTHESIS_VOICE " + voice_id);
}

void SsipTtsService::set_rate(float rate) {
    command("SET self RATE " + std::to_string(to_ssip_scale(rate)));
}

void SsipTtsService::set_pitch(float pitch) {
    command("SET self PITCH " + std::to_string(to_ssip_scale(pitch)));
}

void SsipTtsService::set_volume(float volume) {
    command("SET self VOLUME " + std::to_string(to_ssip_scale(volume)));
}

std::vector<TtsEngineInfo> SsipTtsService::get_engines() const {
    std::vector<TtsEngineInfo> engines;
    std::vector<std::string> lines;
    if (!command("LIST OUTPUT_MODULES", &lines)) return engines;

    for (const auto& line : lines) {
        if (line.empty()) continue;
        TtsEngineInfo info;
        info.id = line;
        info.name = line;
        // Modules carry their own credentials, so the Speech page has no keys
        // to collect: this is the UI simplification the issue is really about.
        info.needs_credentials = false;
        info.credential_keys_json = "[]";
        engines.push_back(info);
    }
    return engines;
}

std::vector<TtsVoiceInfo> SsipTtsService::get_voices() const {
    std::vector<TtsVoiceInfo> voices;
    std::vector<std::string> lines;
    if (!command("LIST SYNTHESIS_VOICES", &lines)) return voices;

    for (const auto& line : lines) {
        if (line.empty()) continue;
        // NAME<TAB>LANGUAGE<TAB>VARIANT
        const auto parts = split_tabs(line);
        TtsVoiceInfo info;
        info.id = parts[0];
        info.name = parts[0];
        info.language = parts.size() > 1 ? parts[1] : "";
        // SSIP's "variant" is male1/female1/..., which is the closest thing
        // the protocol has to the wrapper's gender field.
        info.gender = parts.size() > 2 ? parts[2] : "";
        voices.push_back(info);
    }
    return voices;
}

#endif // !_WIN32
