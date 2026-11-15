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
    std::string credential_keys_json;
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

    // The wrapper's embedded Sherpa-ONNX model registry as JSON
    // (empty string when the feature is compiled out).
    std::string get_sherpa_models_json() const;

    // Fetch + sha-verify the voicegarden g2p bundle for a registry
    // model id (no-espeak phonemization for non-English voices).
    // True when the bundle is cached afterwards.
    bool provision_sherpa_g2p(const std::string& model_id) const;

  private:
    void* m_ctx;
    bool m_available = false;
    std::string m_engine_id;
};
