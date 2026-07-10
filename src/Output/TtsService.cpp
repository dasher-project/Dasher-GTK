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
    tts_speak(static_cast<tts_ctx*>(m_ctx), text.c_str());
#endif
}

void TtsService::speak_sync(const std::string& text) {
#ifdef HAS_TTS
    if (!m_ctx || text.empty()) return;
    tts_speak_sync(static_cast<tts_ctx*>(m_ctx), text.c_str());
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
    m_ctx = tts_create(engine_id.c_str(),
                       credentials.empty() ? nullptr : credentials.c_str());
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

std::vector<TtsVoiceInfo> TtsService::get_voices() const {
    std::vector<TtsVoiceInfo> result;
#ifdef HAS_TTS
    if (!m_ctx) return result;
    tts_voice* voices = nullptr;
    int32_t count = 0;
    if (tts_get_voices(static_cast<tts_ctx*>(m_ctx), &voices, &count) != 0)
        return result;
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
