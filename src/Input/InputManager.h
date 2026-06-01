#pragma once

#include "Engine/DasherBridge.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct JoystickDevice {
    SDL_JoystickID instance_id = 0;
    SDL_Joystick* handle = nullptr;
    SDL_Gamepad* gamepad = nullptr;
    std::string guid;
    std::string name;
    int num_axes = 0;
    int num_buttons = 0;
};

class InputManager {
public:
    explicit InputManager(std::shared_ptr<DasherBridge> bridge);
    ~InputManager();

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    void activate();
    void deactivate();

    void set_joystick_axis_mapping(const std::string& x_axis, const std::string& y_axis);
    void set_button_mapping(const std::string& mapping_string);
    std::string get_button_mapping() const;

    const std::vector<JoystickDevice>& get_joysticks() const;
    int get_joystick_count() const;

    void set_canvas_size(int width, int height);

private:
    void sdl_thread_func();
    void handle_axis_motion(const SDL_Event& event);
    void handle_button_down(const SDL_Event& event);
    void handle_button_up(const SDL_Event& event);
    void handle_joystick_added(SDL_JoystickID id);
    void handle_joystick_removed(SDL_JoystickID id);

    int map_button_to_virtual_key(const std::string& device_key) const;

    std::shared_ptr<DasherBridge> m_bridge;

    std::thread m_sdl_thread;
    std::atomic<bool> m_running{false};

    mutable std::mutex m_joysticks_mutex;
    std::vector<JoystickDevice> m_joysticks;

    std::string m_x_axis_id;
    std::string m_y_axis_id;

    std::unordered_map<std::string, int> m_button_map;

    std::atomic<int> m_canvas_width{500};
    std::atomic<int> m_canvas_height{500};

    static constexpr float DEADZONE = 0.15f;
    static constexpr int SDL_AXIS_RANGE = 32767;
};
