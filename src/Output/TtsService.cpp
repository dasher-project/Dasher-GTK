#include "TtsService.h"

#ifdef HAS_TTS
#include "tts_wrapper.h"
#include <cstring>
#include <iostream>
#endif

TtsService::TtsService() : m_ctx(nullptr) {
#ifdef HAS_TTS
    const char* try_engines[] = {"system", "avsynth", "sapi", nullptr};
    for (int i = 0; try_engines[i]; i++) {
        m_ctx = tts_create(try_engines[i], nullptr);
        if (m_ctx) {
            m_available = true;
            m_engine_id = try_engines[i];
            break;
        }
    }
    if (!m_ctx) {
        m_available = false;
    }
#endif
}

TtsService::~TtsService() {
#ifdef HAS_TTS
    if (m_ctx) {
        tts_stop(static_cast<tts_ctx*>(m_ctx));
        tts_destroy(static_cast<tts_ctx*>(m_ctx));
    }
#endif
}

bool TtsService::is_available() const {
    return m_available;
}

void TtsService::speak(const std::string& text) {
#ifdef HAS_TTS
    if (!m_ctx || text.empty()) return;
    const std::string payload = build_payload(text);
    tts_speak(static_cast<tts_ctx*>(m_ctx), payload.c_str());
#else
    (void)text;
#endif
}

void TtsService::speak_sync(const std::string& text) {
#ifdef HAS_TTS
    if (!m_ctx || text.empty()) return;
    const std::string payload = build_payload(text);
    tts_speak_sync(static_cast<tts_ctx*>(m_ctx), payload.c_str());
#else
    (void)text;
#endif
}

void TtsService::set_phoneme_mode(bool on) {
    m_phoneme_mode = on;
}

std::string TtsService::build_payload(const std::string& text) const {
#ifdef HAS_TTS
    // Parity with Dasher-Apple SpeechService.speak(_:phonemes:): the
    // wrap is skipped for sherpa (local engines strip <phoneme>), and
    // empty tokens (double spaces) pass through untouched rather than
    // producing "()[xsampa:\"\"]".
    if (!m_phoneme_mode || m_engine_id == "sherpaonnx") return text;
    std::string payload;
    payload.reserve(text.size() * 2);
    size_t start = 0;
    bool first = true;
    while (start <= text.size()) {
        size_t end = text.find(' ', start);
        std::string token = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!token.empty()) {
            if (!first) payload += ' ';
            payload += '(';
            payload += token;
            payload += ")[xsampa:\"" + token + "\"]";
            first = false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return payload;
#else
    return text;
#endif
}

void TtsService::stop() {
#ifdef HAS_TTS
    if (!m_ctx) return;
    tts_stop(static_cast<tts_ctx*>(m_ctx));
#endif
}

void TtsService::pause() {
#ifdef HAS_TTS
    if (!m_ctx) return;
    tts_pause(static_cast<tts_ctx*>(m_ctx));
#endif
}

void TtsService::resume() {
#ifdef HAS_TTS
    if (!m_ctx) return;
    tts_resume(static_cast<tts_ctx*>(m_ctx));
#endif
}

void TtsService::set_engine(const std::string& engine_id, const std::string& credentials) {
#ifdef HAS_TTS
    if (m_ctx) {
        tts_destroy(static_cast<tts_ctx*>(m_ctx));
    }
    m_ctx = tts_create(engine_id.c_str(), credentials.empty() ? nullptr : credentials.c_str());
    m_available = (m_ctx != nullptr);
    if (m_available) {
        m_engine_id = engine_id;
    } else {
        m_engine_id.clear();
        std::cerr << "TtsService: Failed to create engine: " << engine_id << std::endl;
        const char* err = tts_get_last_error(nullptr);
        if (err) std::cerr << "  Error: " << err << std::endl;
        const char* fallbacks[] = {"system", "avsynth", "sapi", nullptr};
        for (int i = 0; fallbacks[i]; i++) {
            m_ctx = tts_create(fallbacks[i], nullptr);
            if (m_ctx) {
                m_available = true;
                m_engine_id = fallbacks[i];
                break;
            }
        }
        if (!m_ctx) {
            m_available = false;
        }
    }
#else
    (void)engine_id;
    (void)credentials;
#endif
}

std::string TtsService::get_current_engine() const {
    return m_engine_id;
}

void TtsService::set_voice(const std::string& voice_id) {
#ifdef HAS_TTS
    if (!m_ctx) return;
    tts_set_voice(static_cast<tts_ctx*>(m_ctx), voice_id.c_str());
#else
    (void)voice_id;
#endif
}

void TtsService::set_rate(float rate) {
#ifdef HAS_TTS
    if (!m_ctx) return;
    tts_set_rate(static_cast<tts_ctx*>(m_ctx), rate);
#else
    (void)rate;
#endif
}

void TtsService::set_pitch(float pitch) {
#ifdef HAS_TTS
    if (!m_ctx) return;
    tts_set_pitch(static_cast<tts_ctx*>(m_ctx), pitch);
#else
    (void)pitch;
#endif
}

void TtsService::set_volume(float volume) {
#ifdef HAS_TTS
    if (!m_ctx) return;
    tts_set_volume(static_cast<tts_ctx*>(m_ctx), volume);
#else
    (void)volume;
#endif
}

std::vector<TtsEngineInfo> TtsService::get_engines() const {
    std::vector<TtsEngineInfo> result;
#ifdef HAS_TTS
    tts_engine_info* infos = nullptr;
    int32_t count = 0;
    if (tts_get_engines(&infos, &count) != 0 || !infos || count <= 0) return result;
    for (int32_t i = 0; i < count; i++) {
        TtsEngineInfo info;
        if (infos[i].id) info.id = infos[i].id;
        if (infos[i].name) info.name = infos[i].name;
        info.needs_credentials = infos[i].needs_credentials;
        if (infos[i].credential_keys_json) info.credential_keys_json = infos[i].credential_keys_json;
        result.push_back(std::move(info));
    }
    tts_free_engines(infos, count);
#endif
    return result;
}

std::string TtsService::get_sherpa_models_json() const {
#ifdef HAS_TTS
    char* json = tts_get_sherpa_models_json();
    if (!json) return {};
    std::string result(json);
    tts_free_string(json);
    return result;
#else
    return {};
#endif
}

bool TtsService::provision_sherpa_g2p(const std::string& model_id) const {
#ifdef HAS_TTS
    return tts_provision_sherpa_g2p_bundle(model_id.c_str());
#else
    (void)model_id;
    return false;
#endif
}

std::string TtsService::pronunciations_default_path() {
#ifdef HAS_TTS
    // Mirrors the wrapper's own resolution (pronunciation.rs
    // default_path): $HOME/.rust-tts-wrapper/pronunciations.txt. Kept
    // in sync so the "Open File" button edits the file the wrapper
    // actually reads.
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    if (!home || !*home) return std::string();
    return std::string(home) + "/.rust-tts-wrapper/pronunciations.txt";
#else
    return std::string();
#endif
}

bool TtsService::pronunciations_active() {
#ifdef HAS_TTS
    return tts_pronunciations_active() == 1;
#else
    return false;
#endif
}

int TtsService::pronunciations_entry_count() {
#ifdef HAS_TTS
    int32_t entries = 0, skipped = 0;
    if (tts_pronunciations_stats(&entries, &skipped) != 1) return -1;
    return static_cast<int>(entries);
#else
    return -1;
#endif
}

std::string TtsService::apply_pronunciations(const std::string& text) {
#ifdef HAS_TTS
    if (text.empty()) return text;
    char* result = tts_apply_pronunciations(text.c_str());
    if (!result) return text;
    std::string applied(result);
    tts_free_string(result);
    return applied;
#else
    return text;
#endif
}

std::vector<TtsVoiceInfo> TtsService::get_voices() const {
    std::vector<TtsVoiceInfo> result;
#ifdef HAS_TTS
    if (!m_ctx) return result;
    tts_voice* voices = nullptr;
    int32_t count = 0;
    if (tts_get_voices(static_cast<tts_ctx*>(m_ctx), &voices, &count) != 0) return result;
    for (int32_t i = 0; i < count; i++) {
        TtsVoiceInfo info;
        if (voices[i].id) info.id = voices[i].id;
        if (voices[i].name) info.name = voices[i].name;
        if (voices[i].language) info.language = voices[i].language;
        if (voices[i].gender) info.gender = voices[i].gender;
        result.push_back(std::move(info));
    }
    tts_free_voices(voices, count);
#endif
    return result;
}
