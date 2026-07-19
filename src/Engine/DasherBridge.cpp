#include "DasherBridge.h"
#include "Analytics/EngineLogRingBuffer.h"
#include <cstring>
#include <iostream>
#include <glib.h>

DasherBridge::DasherBridge(const std::string& data_dir, const std::string& user_dir) {
    m_start_time = std::chrono::steady_clock::now();

    char* error = nullptr;
    m_ctx = dasher_create(data_dir.c_str(), user_dir.empty() ? nullptr : user_dir.c_str(), &error);
    if (!m_ctx) {
        std::string err = error ? error : "Unknown error creating Dasher context";
        std::cerr << "DasherBridge: " << err << std::endl;
    }

    if (m_ctx) {
        // Route DasherCore's internal diagnostic logging to GLib so it integrates
        // with GTK's structured logging (stderr / journald), and mirror it into
        // the crash-report ring buffer. Capture from info (level 1) up so a crash
        // report has useful engine context (RFC 0009); GLib still filters what
        // actually reaches stderr by domain/level at display time.
        dasher_set_log_callback(m_ctx, log_callback_trampoline, nullptr, /*min_level=*/1);
    }
}

DasherBridge::~DasherBridge() {
    if (m_ctx) {
        dasher_destroy(m_ctx);
    }
}

void DasherBridge::set_screen_size(int width, int height) {
    if (m_ctx) dasher_set_screen_size(m_ctx, width, height);
}

void DasherBridge::mouse_move(float x, float y) {
    if (m_ctx) dasher_mouse_move(m_ctx, x, y);
}

void DasherBridge::mouse_down() {
    if (m_ctx) dasher_mouse_down(m_ctx);
}

void DasherBridge::mouse_up() {
    if (m_ctx) dasher_mouse_up(m_ctx);
}

void DasherBridge::key_event(int key, int pressed) {
    if (m_ctx) dasher_key_event(m_ctx, key, pressed);
}

DasherBridge::FrameResult DasherBridge::frame(int64_t time_ms) {
    FrameResult result;
    if (!m_ctx) return result;

    int* cmds = nullptr;
    int cmd_count = 0;
    char** strs = nullptr;
    int str_count = 0;

    dasher_frame(m_ctx, time_ms, &cmds, &cmd_count, &strs, &str_count);

    if (cmds && cmd_count > 0) {
        result.commands.assign(cmds, cmds + cmd_count);
    }

    if (strs && str_count > 0) {
        for (int i = 0; i < str_count; i++) {
            result.strings.emplace_back(strs[i] ? strs[i] : "");
        }
    }

    return result;
}

std::string DasherBridge::get_output_text() const {
    if (!m_ctx) return "";
    const char* text = dasher_get_output_text(m_ctx);
    return text ? text : "";
}

void DasherBridge::reset_output_text() {
    if (m_ctx) dasher_reset_output_text(m_ctx);
}

std::string DasherBridge::get_alphabet_id() const {
    if (!m_ctx) return "";
    const char* id = dasher_get_alphabet_id(m_ctx);
    return id ? id : "";
}

void DasherBridge::set_alphabet_id(const std::string& id) {
    if (m_ctx) dasher_set_alphabet_id(m_ctx, id.c_str());
}

int DasherBridge::get_alphabet_count() const {
    if (!m_ctx) return 0;
    return dasher_get_alphabet_count(m_ctx);
}

std::string DasherBridge::get_alphabet_name(int index) const {
    if (!m_ctx) return "";
    const char* name = dasher_get_alphabet_name(m_ctx, index);
    return name ? name : "";
}

int DasherBridge::get_language_model_id() const {
    if (!m_ctx) return 0;
    return dasher_get_language_model_id(m_ctx);
}

void DasherBridge::set_language_model_id(int model_id) {
    if (m_ctx) dasher_set_language_model_id(m_ctx, model_id);
}

int DasherBridge::get_speed_percent() const {
    if (!m_ctx) return 100;
    return dasher_get_speed_percent(m_ctx);
}

void DasherBridge::set_speed_percent(int percent) {
    if (m_ctx) dasher_set_speed_percent(m_ctx, percent);
}

double DasherBridge::get_cps() const {
    if (!m_ctx) return 0.0;
    return dasher_get_cps(m_ctx);
}

double DasherBridge::get_wpm() const {
    if (!m_ctx) return 0.0;
    return dasher_get_wpm(m_ctx);
}

bool DasherBridge::get_bool_parameter(int key) const {
    if (!m_ctx) return false;
    return dasher_get_bool_parameter(m_ctx, key) != 0;
}

void DasherBridge::set_bool_parameter(int key, bool value) {
    if (m_ctx) dasher_set_bool_parameter(m_ctx, key, value ? 1 : 0);
}

long DasherBridge::get_long_parameter(int key) const {
    if (!m_ctx) return 0;
    return dasher_get_long_parameter(m_ctx, key);
}

void DasherBridge::set_long_parameter(int key, long value) {
    if (m_ctx) dasher_set_long_parameter(m_ctx, key, value);
}

std::string DasherBridge::get_string_parameter(int key) const {
    if (!m_ctx) return "";
    const char* val = dasher_get_string_parameter(m_ctx, key);
    return val ? val : "";
}

void DasherBridge::set_string_parameter(int key, const std::string& value) {
    if (m_ctx) dasher_set_string_parameter(m_ctx, key, value.c_str());
}

int DasherBridge::find_parameter_key(const std::string& enum_key_name) const {
    // Resolve a parameter by its stable enum name (e.g. "LP_MAX_BITRATE") to its
    // current integer key. The numeric values of Dasher::Parameter are an
    // implementation detail of DasherCore and shift between releases, so callers
    // must never hardcode them. Returns -1 if the name is unknown.
    return dasher_find_parameter_key(enum_key_name.c_str());
}

int DasherBridge::get_parameter_count() const {
    return dasher_get_parameter_count();
}

DasherParameterInfo DasherBridge::get_parameter_info(int index) const {
    DasherParameterInfo info = {};
    ::dasher_parameter_info raw = {};
    if (dasher_get_parameter_info(index, &raw) == 0) {
        info.key = raw.key;
        info.name = raw.name ? raw.name : "";
        info.desc = raw.desc ? raw.desc : "";
        info.type = raw.type;
        info.ui_type = raw.ui_type;
        info.min_val = raw.min_val;
        info.max_val = raw.max_val;
        info.step = raw.step;
        info.advanced = raw.advanced != 0;
        info.group = raw.group ? raw.group : "";
        info.subgroup = raw.subgroup ? raw.subgroup : "";
    }
    return info;
}

int DasherBridge::get_parameter_enum_count(int key) const {
    return dasher_get_parameter_enum_count(key);
}

std::string DasherBridge::get_parameter_enum_name(int key, int index) const {
    const char* name = dasher_get_parameter_enum_name(key, index);
    return name ? name : "";
}

int DasherBridge::get_parameter_enum_value(int key, int index) const {
    return dasher_get_parameter_enum_value(key, index);
}

std::vector<std::string> DasherBridge::get_parameter_string_values(int key) const {
    std::vector<std::string> result;
    if (!m_ctx) return result;

    int count = dasher_get_parameter_string_values(m_ctx, key, nullptr, 0);
    if (count <= 0) return result;

    std::vector<const char*> ptrs(count);
    dasher_get_parameter_string_values(m_ctx, key, ptrs.data(), count);
    for (int i = 0; i < count; i++) {
        result.emplace_back(ptrs[i] ? ptrs[i] : "");
    }
    return result;
}

int DasherBridge::get_palette_count() const {
    if (!m_ctx) return 0;
    return dasher_get_palette_count(m_ctx);
}

std::string DasherBridge::get_palette_name(int index) const {
    if (!m_ctx) return "";
    const char* name = dasher_get_palette_name(m_ctx, index);
    return name ? name : "";
}

std::string DasherBridge::get_current_palette() const {
    if (!m_ctx) return "";
    const char* name = dasher_get_current_palette(m_ctx);
    return name ? name : "";
}

bool DasherBridge::get_palette_preview_colors(int index, int out_colors[4]) const {
    if (!m_ctx) return false;
    return dasher_get_palette_preview_colors(m_ctx, index, out_colors) == 0;
}

void DasherBridge::set_palette(const std::string& name) {
    if (m_ctx) dasher_set_palette(m_ctx, name.c_str());
}

void DasherBridge::save_settings() {
    if (m_ctx) dasher_save_settings(m_ctx);
}

int DasherBridge::set_locale(const std::string& locale) {
    if (!m_ctx) return -1;
    return dasher_set_locale(m_ctx, locale.c_str());
}

std::string DasherBridge::get_locale() const {
    if (!m_ctx) return "en";
    const char* loc = dasher_get_locale(m_ctx);
    return loc ? loc : "en";
}

std::vector<DasherBridge::LocaleInfo> DasherBridge::get_available_locales() const {
    return {
        {"af", "Afrikaans"}, {"ar", "\u0627\u0644\u0639\u0631\u0628\u064a\u0629"},
        {"bn", "\u09ac\u09be\u0982\u09b2\u09be"}, {"cs", "\u010ce\u0161tina"},
        {"da", "Dansk"}, {"de", "Deutsch"}, {"el", "\u0395\u03bb\u03bb\u03b7\u03bd\u03b9\u03ba\u03ac"},
        {"en", "English"}, {"es", "Espa\u00f1ol"}, {"fa", "\u0641\u0627\u0631\u0633\u06cc"},
        {"fi", "Suomi"}, {"fr", "Fran\u00e7ais"}, {"gu", "\u0a97\u0ac1\u0a9c\u0ab0\u0abe\u0aa4\u0ac0"},
        {"hi", "\u0939\u093f\u0928\u094d\u0926\u0940"}, {"hu", "Magyar"},
        {"it", "Italiano"}, {"kn", "\u0c95\u0ca8\u0ccd\u0ca8\u0ca1"},
        {"ml", "\u0d2e\u0d32\u0d2f\u0d3e\u0d33\u0d02"}, {"mr", "\u092e\u0930\u093e\u0920\u0940"},
        {"nl", "Nederlands"}, {"pa", "\u0a2a\u0a70\u0a1c\u0a3e\u0a2c\u0a40"},
        {"pl", "Polski"}, {"pt", "Portugu\u00eas (BR)"}, {"pt-PT", "Portugu\u00eas (PT)"},
        {"ru", "\u0420\u0443\u0441\u0441\u043a\u0438\u0439"}, {"sv", "Svenska"},
        {"sw", "Kiswahili"}, {"ta", "\u0ba4\u0bae\u0bbf\u0bb4\u0bcd"},
        {"te", "\u0c24\u0c46\u0c32\u0c41\u0c17\u0c41"}, {"th", "\u0e44\u0e17\u0e22"},
        {"ur", "\u0627\u0631\u062f\u0648"}, {"zh-CN", "\u7b80\u4f53\u4e2d\u6587"},
        {"zu", "isiZulu"},
    };
}

void DasherBridge::set_output_callback(std::function<void(int, const std::string&)> callback) {
    m_output_callback = std::move(callback);
    if (m_ctx) {
        dasher_set_output_callback(m_ctx, output_callback_trampoline, this);
    }
}

void DasherBridge::output_callback_trampoline(int event_type, const char* text, void* user_data) {
    auto* self = static_cast<DasherBridge*>(user_data);
    if (self && self->m_output_callback) {
        self->m_output_callback(event_type, text ? text : "");
    }
}

void DasherBridge::log_callback_trampoline(int level, const char* message, void* /*user_data*/) {
    // Map DasherCore log levels (0=debug, 1=info, 2=warning, 3=error) onto GLib
    // log levels under the "DasherCore" domain. Note: engine "error" maps to
    // G_LOG_LEVEL_CRITICAL rather than G_LOG_LEVEL_ERROR, because the latter is
    // fatal under GLib and would abort the process.
    GLogLevelFlags glib_level;
    switch (level) {
    case 0:
        glib_level = G_LOG_LEVEL_DEBUG;
        break;
    case 1:
        glib_level = G_LOG_LEVEL_INFO;
        break;
    case 2:
        glib_level = G_LOG_LEVEL_WARNING;
        break;
    case 3:
    default:
        glib_level = G_LOG_LEVEL_CRITICAL;
        break;
    }
    g_log("DasherCore", glib_level, "%s", message ? message : "");

    // Also retain the line in the crash-report ring buffer (and its on-disk
    // mirror) so a subsequent crash can attach recent engine context.
    analytics::engine_log_buffer().append(level, message ? message : "");
}

int64_t DasherBridge::get_current_time_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_start_time).count();
}
