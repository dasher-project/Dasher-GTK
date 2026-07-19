#include "AnalyticsSettings.h"

#include <glib.h>

namespace analytics {

namespace {
constexpr char kGroup[] = "analytics";
} // namespace

std::string AnalyticsSettings::default_path() {
    char* dir = g_build_filename(g_get_user_config_dir(), "dasher", nullptr);
    char* path = g_build_filename(dir, "analytics.conf", nullptr);
    std::string result = path ? path : "";
    g_free(dir);
    g_free(path);
    return result;
}

AnalyticsSettings AnalyticsSettings::load(const std::string& path) {
    AnalyticsSettings s;
    s.m_path = path;

    GKeyFile* kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        s.m_opted_in = g_key_file_get_boolean(kf, kGroup, "opted_in", nullptr);
        s.m_prompt_shown = g_key_file_get_boolean(kf, kGroup, "prompt_shown", nullptr);
        char* id = g_key_file_get_string(kf, kGroup, "anonymous_id", nullptr);
        if (id) {
            s.m_anonymous_id = id;
            g_free(id);
        }
    }
    g_key_file_free(kf);
    return s;
}

void AnalyticsSettings::save() const {
    GKeyFile* kf = g_key_file_new();
    g_key_file_set_boolean(kf, kGroup, "opted_in", m_opted_in);
    g_key_file_set_boolean(kf, kGroup, "prompt_shown", m_prompt_shown);
    if (!m_anonymous_id.empty()) {
        g_key_file_set_string(kf, kGroup, "anonymous_id", m_anonymous_id.c_str());
    }

    // Ensure the parent directory exists before writing.
    char* dir = g_path_get_dirname(m_path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    g_key_file_save_to_file(kf, m_path.c_str(), nullptr);
    g_key_file_free(kf);
}

const std::string& AnalyticsSettings::anonymous_id() {
    if (m_anonymous_id.empty()) {
        char* uuid = g_uuid_string_random();
        m_anonymous_id = uuid ? uuid : "";
        g_free(uuid);
        save();
    }
    return m_anonymous_id;
}

void AnalyticsSettings::reset_anonymous_id() {
    char* uuid = g_uuid_string_random();
    m_anonymous_id = uuid ? uuid : "";
    g_free(uuid);
    save();
}

} // namespace analytics
