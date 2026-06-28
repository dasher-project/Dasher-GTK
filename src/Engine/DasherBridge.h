#pragma once

#include "dasher.h"
#include <functional>
#include <string>
#include <vector>
#include <chrono>

struct DasherParameterInfo {
    int key;
    std::string name;
    std::string desc;
    int type;
    int ui_type;
    long min_val;
    long max_val;
    long step;
    bool advanced;
    std::string group;
    std::string subgroup;
};

class DasherBridge {
public:
    DasherBridge(const std::string& data_dir, const std::string& user_dir);
    ~DasherBridge();

    DasherBridge(const DasherBridge&) = delete;
    DasherBridge& operator=(const DasherBridge&) = delete;

    void set_screen_size(int width, int height);
    void mouse_move(float x, float y);
    void mouse_down();
    void mouse_up();
    void key_event(int key, int pressed);

    struct FrameResult {
        std::vector<int> commands;
        std::vector<std::string> strings;
    };

    FrameResult frame(int64_t time_ms);

    std::string get_output_text() const;
    void reset_output_text();

    std::string get_alphabet_id() const;
    void set_alphabet_id(const std::string& id);
    int get_alphabet_count() const;
    std::string get_alphabet_name(int index) const;

    int get_language_model_id() const;
    void set_language_model_id(int model_id);

    int get_speed_percent() const;
    void set_speed_percent(int percent);

    bool get_bool_parameter(int key) const;
    void set_bool_parameter(int key, bool value);
    long get_long_parameter(int key) const;
    void set_long_parameter(int key, long value);
    std::string get_string_parameter(int key) const;
    void set_string_parameter(int key, const std::string& value);

    int find_parameter_key(const std::string& enum_key_name) const;

    int get_parameter_count() const;
    DasherParameterInfo get_parameter_info(int index) const;
    int get_parameter_enum_count(int key) const;
    std::string get_parameter_enum_name(int key, int index) const;
    int get_parameter_enum_value(int key, int index) const;
    std::vector<std::string> get_parameter_string_values(int key) const;

    int get_palette_count() const;
    std::string get_palette_name(int index) const;
    std::string get_current_palette() const;
    bool get_palette_preview_colors(int index, int out_colors[4]) const;
    void set_palette(const std::string& name);

    void save_settings();

    int set_locale(const std::string& locale);
    std::string get_locale() const;

    struct LocaleInfo {
        std::string code;
        std::string display_name;
    };
    std::vector<LocaleInfo> get_available_locales() const;

    void set_output_callback(std::function<void(int, const std::string&)> callback);

    int64_t get_current_time_ms() const;

private:
    dasher_ctx* m_ctx = nullptr;
    std::function<void(int, const std::string&)> m_output_callback;

    static void output_callback_trampoline(int event_type, const char* text, void* user_data);
    static void log_callback_trampoline(int level, const char* message, void* user_data);

    std::chrono::time_point<std::chrono::steady_clock> m_start_time;
};
