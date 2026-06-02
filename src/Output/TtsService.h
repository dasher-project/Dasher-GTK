#pragma once

#include <string>
#include <functional>
#include <vector>

struct TtsVoiceInfo {
    std::string id;
    std::string name;
    std::string language;
    std::string gender;
};

struct TtsEngineInfo {
    std::string id;
    std::string name;
    bool needs_credentials;
};

class TtsService {
public:
    TtsService();
    ~TtsService();

    TtsService(const TtsService&) = delete;
    TtsService& operator=(const TtsService&) = delete;

    bool is_available() const;

    void speak(const std::string& text);
    void speak_sync(const std::string& text);
    void stop();
    void pause();
    void resume();

    void set_engine(const std::string& engine_id, const std::string& credentials = "");
    std::string get_current_engine() const;

    void set_voice(const std::string& voice_id);
    void set_rate(float rate);
    void set_pitch(float pitch);
    void set_volume(float volume);

    std::vector<TtsEngineInfo> get_engines() const;
    std::vector<TtsVoiceInfo> get_voices() const;

private:
    void* m_ctx;
    bool m_available = false;
    std::string m_engine_id;
};
