#include "UiSettings.h"
#include <glib.h>
#include <cmath>

namespace ui {
namespace {

constexpr const char* kGroup = "ui";

// GKeyFile has no has_int; read through the error to tell "missing" from 0.
int get_int_or(GKeyFile* kf, const char* key, bool& found) {
    GError* err = nullptr;
    const gint v = g_key_file_get_integer(kf, kGroup, key, &err);
    if (err != nullptr) {
        g_clear_error(&err);
        found = false;
        return 0;
    }
    found = true;
    return v;
}

// Valid sizes only: reject the degenerate (0/-1) values a failed or partial
// save would produce.
WindowGeometry load_geometry(GKeyFile* kf, const char* px, const char* py, const char* pw,
                             const char* ph, const char* pmax, bool with_maximized) {
    WindowGeometry g;
    bool found = false;
    const int w = get_int_or(kf, pw, found);
    if (!found) return g;
    const int h = get_int_or(kf, ph, found);
    if (!found || w < 50 || h < 50) return g; // degenerate / corrupt
    g.w = w;
    g.h = h;
    g.valid = true;

    const int x = get_int_or(kf, px, found);
    if (found) {
        const int y = get_int_or(kf, py, found);
        if (found) {
            g.x = x;
            g.y = y;
            g.has_position = true;
        }
    }
    if (with_maximized) {
        GError* err = nullptr;
        const gboolean max = g_key_file_get_boolean(kf, kGroup, pmax, &err);
        if (err == nullptr) g.maximized = (max == TRUE);
        g_clear_error(&err);
    }
    return g;
}

void save_geometry(GKeyFile* kf, const WindowGeometry& g, const char* px, const char* py,
                   const char* pw, const char* ph, const char* pmax, bool with_maximized) {
    if (!g.valid) return;
    g_key_file_set_integer(kf, kGroup, pw, g.w);
    g_key_file_set_integer(kf, kGroup, ph, g.h);
    if (g.has_position) {
        g_key_file_set_integer(kf, kGroup, px, g.x);
        g_key_file_set_integer(kf, kGroup, py, g.y);
    }
    if (with_maximized) g_key_file_set_boolean(kf, kGroup, pmax, g.maximized);
}

} // namespace

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
        // Use the return value directly — g_key_file_get_boolean returns
        // FALSE both for a valid `false` and for a missing key. Only
        // fall back to the default when the key genuinely doesn't exist.
        GError* err = nullptr;
        const gboolean enabled = g_key_file_get_boolean(kf, kGroup, "update_checks_enabled", &err);
        if (err == nullptr) {
            s.m_update_checks_enabled = (enabled == TRUE);
        }
        // Key missing or unreadable — keep the default (true)
        g_clear_error(&err);

        s.m_window_geometry = load_geometry(kf, "window_x", "window_y", "window_w", "window_h",
                                            "window_maximized", /*with_maximized=*/true);
        s.m_keyboard_geometry = load_geometry(kf, "kb_x", "kb_y", "kb_w", "kb_h", nullptr,
                                              /*with_maximized=*/false);
    }
    g_key_file_free(kf);
    return s;
}

void UiSettings::save() const {
    GKeyFile* kf = g_key_file_new();
    g_key_file_set_double(kf, kGroup, "keyboard_opacity", m_keyboard_opacity);
    g_key_file_set_boolean(kf, kGroup, "update_checks_enabled", m_update_checks_enabled);
    save_geometry(kf, m_window_geometry, "window_x", "window_y", "window_w", "window_h",
                  "window_maximized", /*with_maximized=*/true);
    save_geometry(kf, m_keyboard_geometry, "kb_x", "kb_y", "kb_w", "kb_h", nullptr,
                  /*with_maximized=*/false);
    // First run writes before anything else has created the config dir, and
    // g_key_file_save_to_file fails silently without it.
    char* dir = g_path_get_dirname(m_path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_key_file_save_to_file(kf, m_path.c_str(), nullptr);
    g_key_file_free(kf);
}

void UiSettings::set_keyboard_opacity(double v) {
    if (v < 0.2) v = 0.2;
    if (v > 1.0) v = 1.0;
    m_keyboard_opacity = v;
}

void UiSettings::set_update_checks_enabled(bool v) {
    m_update_checks_enabled = v;
}

} // namespace ui
