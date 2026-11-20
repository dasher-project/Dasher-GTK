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

    // X-SAMPA phoneme mode (parity with Dasher-Apple's SpeechService
    // .phonemeMode): when the active alphabet is phonetic, each spoken
    // token is wrapped as (tok)[xsampa:"tok"] SpeechMarkdown so
    // phoneme-capable engines pronounce it; sherpa (local, strips
    // <phoneme>) receives the raw text.
    void set_phoneme_mode(bool on);

    std::vector<TtsEngineInfo> get_engines() const;
    std::vector<TtsVoiceInfo> get_voices() const;

    // The wrapper's embedded Sherpa-ONNX model registry as JSON
    // (empty string when the feature is compiled out).
    std::string get_sherpa_models_json() const;

    // Fetch + sha-verify the voicegarden g2p bundle for a registry
    // model id (no-espeak phonemization for non-English voices).
    // True when the bundle is cached afterwards.
    bool provision_sherpa_g2p(const std::string& model_id) const;

    // User pronunciation overrides (rust-tts-wrapper #125/#131): a
    // plain-text file ("word = respell" / "word = /ipa/") applied at
    // the wrapper's text-normalization stage on every utterance.
    // The default file (~/.rust-tts-wrapper/pronunciations.txt) is
    // picked up with no configuration; these surface state for the
    // Speech page.
    static std::string pronunciations_default_path();
    static bool pronunciations_active();
    static int pronunciations_entry_count(); // -1 when no file loaded
    // Dry-run the text-form overrides: the text a plain-text engine
    // will actually speak (for editor previews and self-tests).
    static std::string apply_pronunciations(const std::string& text);

  private:
    void* m_ctx;
    bool m_available = false;
    std::string m_engine_id;
    bool m_phoneme_mode = false;

    // The payload actually sent to tts_speak: raw text, or each
    // space-separated token wrapped in SpeechMarkdown when phoneme
    // mode is on and the engine is not sherpa-onnx.
    std::string build_payload(const std::string& text) const;
};
