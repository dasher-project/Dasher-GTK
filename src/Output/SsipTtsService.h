#pragma once

// Spike: a TtsService that talks SSIP to speech-dispatcher instead of linking
// rust-tts-wrapper through its C FFI. See dasher-project/Dasher-GTK#47.
//
// The public interface is deliberately identical to TtsService, so MainWindow
// and PreferencesWindow do not change at all: only what sits behind it does.
// Every call TtsService makes into the wrapper maps onto one SSIP command,
// which is the whole point of the experiment.

#include <string>
#include <vector>

#include "TtsService.h"  // TtsVoiceInfo, TtsEngineInfo

class SsipTtsService {
public:
    SsipTtsService();
    ~SsipTtsService();

    SsipTtsService(const SsipTtsService&) = delete;
    SsipTtsService& operator=(const SsipTtsService&) = delete;

    bool is_available() const { return m_fd >= 0; }

    void speak(const std::string& text);
    void speak_sync(const std::string& text);
    void stop();
    void pause();
    void resume();

    // SSIP calls these output modules. An "engine" in wrapper terms is a
    // module here, which is exactly the substitution the issue proposes.
    void set_engine(const std::string& engine_id, const std::string& credentials = "");
    std::string get_current_engine() const { return m_engine_id; }

    void set_voice(const std::string& voice_id);
    void set_rate(float rate);
    void set_pitch(float pitch);
    void set_volume(float volume);

    std::vector<TtsEngineInfo> get_engines() const;
    std::vector<TtsVoiceInfo> get_voices() const;

    // Diagnostics for the spike; not part of the TtsService interface.
    const std::string& last_error() const { return m_last_error; }
    const std::string& socket_path() const { return m_socket_path; }

private:
    // One SSIP request/response. Returns the reply's continuation lines with
    // their "NNN-" prefixes stripped; the final "NNN " line is consumed and
    // its code returned in `code`. Not reentrant, which is fine: everything
    // here runs on the GTK main thread.
    bool command(const std::string& line, std::vector<std::string>* out = nullptr,
                 int* code = nullptr) const;

    // Consume one complete reply. Split out from command() because SPEAK is
    // two exchanges - the command, then the message data - and both ends have
    // to be read or the next command is swallowed as message text.
    bool read_reply(std::vector<std::string>* out, int* code) const;
    bool write_all(const std::string& data) const;

    bool connect();
    void disconnect();

    // SSIP takes integers in [-100, 100] for rate/pitch/volume. The wrapper
    // took floats, so the existing callers keep their units and we map here.
    static int to_ssip_scale(float v);

    int m_fd = -1;
    std::string m_socket_path;
    std::string m_engine_id;
    mutable std::string m_last_error;
};
