#include "UiSettings.h"
#include <glib.h>
#include <cmath>

namespace ui {

namespace {
constexpr const char* kGroup = "ui";
}

std::string UiSettings::default_path() {
    char* dir = g_build_filename(g_get_user_config_dir(), "dasher", nullptr);
    char* path = g_build_filename(dir, "ui.conf", nullptr);
    std::string result = path ? path : "";
    g_free(dir);
    g_free(path);
    return result;
}

UiSettings UiSettings::load(const std::string& path) {
    UiSettings s;
    s.m_path = path;

    GKeyFile* kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        double v = g_key_file_get_double(kf, kGroup, "keyboard_opacity", nullptr);
        if (v >= 0.2 && v <= 1.0) s.m_keyboard_opacity = v;
    }
    g_key_file_free(kf);
    return s;
}

void UiSettings::save() const {
    GKeyFile* kf = g_key_file_new();
    g_key_file_set_double(kf, kGroup, "keyboard_opacity", m_keyboard_opacity);
    g_key_file_save_to_file(kf, m_path.c_str(), nullptr);
    g_key_file_free(kf);
}

void UiSettings::set_keyboard_opacity(double v) {
    if (v < 0.2) v = 0.2;
    if (v > 1.0) v = 1.0;
    m_keyboard_opacity = v;
}

} // namespace ui
