#include "InputManager.h"
#include <cstring>
#include <iostream>
#include <cmath>

InputManager::InputManager(std::shared_ptr<DasherBridge> bridge)
    : m_bridge(std::move(bridge)) {
    m_button_map["MousePrimary"] = 100;
    m_button_map["MouseSecondary"] = 101;
}

InputManager::~InputManager() {
    deactivate();
}

void InputManager::activate() {
    if (m_running.exchange(true)) return;
    m_sdl_thread = std::thread(&InputManager::sdl_thread_func, this);
}

void InputManager::deactivate() {
    if (!m_running.exchange(false)) return;
    if (m_sdl_thread.joinable()) {
        m_sdl_thread.join();
    }
}

void InputManager::set_joystick_axis_mapping(const std::string& x_axis, const std::string& y_axis) {
    m_x_axis_id = x_axis;
    m_y_axis_id = y_axis;
}

void InputManager::set_button_mapping(const std::string& mapping_string) {
    std::lock_guard<std::mutex> lock(m_joysticks_mutex);
    m_button_map.clear();
    m_button_map["MousePrimary"] = 100;
    m_button_map["MouseSecondary"] = 101;

    if (mapping_string.empty()) return;

    std::string remaining = mapping_string;
    while (!remaining.empty()) {
        auto sep = remaining.find(';');
        std::string pair = (sep == std::string::npos) ? remaining : remaining.substr(0, sep);
        remaining = (sep == std::string::npos) ? "" : remaining.substr(sep + 1);

        auto colon = pair.find(':');
        if (colon == std::string::npos) continue;
        std::string device_key = pair.substr(0, colon);
        int vk = std::stoi(pair.substr(colon + 1));
        m_button_map[device_key] = vk;
    }
}

std::string InputManager::get_button_mapping() const {
    std::lock_guard<std::mutex> lock(m_joysticks_mutex);
    std::string result;
    for (const auto& [key, vk] : m_button_map) {
        if (vk == 100 && key == "MousePrimary") continue;
        if (vk == 101 && key == "MouseSecondary") continue;
        result += key + ":" + std::to_string(vk) + ";";
    }
    return result;
}

const std::vector<JoystickDevice>& InputManager::get_joysticks() const {
    return m_joysticks;
}

int InputManager::get_joystick_count() const {
    return static_cast<int>(m_joysticks.size());
}

void InputManager::set_canvas_size(int width, int height) {
    m_canvas_width.store(width);
    m_canvas_height.store(height);
}

void InputManager::sdl_thread_func() {
    if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        std::cerr << "InputManager: SDL joystick init failed: " << SDL_GetError() << std::endl;
        m_running = false;
        return;
    }

    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (ids && count > 0) {
        for (int i = 0; i < count; i++) {
            handle_joystick_added(ids[i]);
        }
    }
    SDL_free(ids);

    SDL_Event event;
    while (m_running) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                handle_axis_motion(event);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                handle_button_down(event);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                handle_button_up(event);
                break;
            case SDL_EVENT_JOYSTICK_ADDED:
                handle_joystick_added(event.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                handle_joystick_removed(event.jdevice.which);
                break;
            default:
                break;
            }
        }
        SDL_Delay(8);
    }

    {
        std::lock_guard<std::mutex> lock(m_joysticks_mutex);
        for (auto& dev : m_joysticks) {
            if (dev.gamepad) SDL_CloseGamepad(dev.gamepad);
            else if (dev.handle) SDL_CloseJoystick(dev.handle);
        }
        m_joysticks.clear();
    }

    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
}

void InputManager::handle_axis_motion(const SDL_Event& event) {
    float value = static_cast<float>(event.jaxis.value) / SDL_AXIS_RANGE;
    if (std::fabs(value) < DEADZONE) value = 0.0f;

    SDL_JoystickID joy_id = event.jaxis.which;
    int axis = event.jaxis.axis;

    std::lock_guard<std::mutex> lock(m_joysticks_mutex);
    for (const auto& dev : m_joysticks) {
        if (dev.instance_id != joy_id) continue;

        std::string axis_str = "JOY-" + dev.guid + "-" +
            (axis < 10 ? "0" : "") + std::to_string(axis);

        bool is_x = (!m_x_axis_id.empty() && axis_str == m_x_axis_id);
        bool is_y = (!m_y_axis_id.empty() && axis_str == m_y_axis_id);

        if (is_x || is_y) {
            float norm = value;
            if (!is_x && is_y) norm = -value;

            int w = m_canvas_width.load();
            int h = m_canvas_height.load();
            float px = is_x ? ((norm * 0.9f + 1.0f) * 0.5f * w) : (w * 0.5f);
            float py = is_y ? (((-norm) * 0.9f + 1.0f) * 0.5f * h) : (h * 0.5f);

            m_bridge->mouse_move(px, py);
        }
        break;
    }
}

static std::string button_key_for_joystick(const JoystickDevice& dev, int button) {
    return "JOY-" + dev.guid + "-" + (button < 10 ? "0" : "") + std::to_string(button);
}

void InputManager::handle_button_down(const SDL_Event& event) {
    std::lock_guard<std::mutex> lock(m_joysticks_mutex);
    for (const auto& dev : m_joysticks) {
        if (dev.instance_id != event.jbutton.which) continue;
        std::string key = button_key_for_joystick(dev, event.jbutton.button);
        int vk = map_button_to_virtual_key(key);
        m_bridge->key_event(vk, 1);
        break;
    }
}

void InputManager::handle_button_up(const SDL_Event& event) {
    std::lock_guard<std::mutex> lock(m_joysticks_mutex);
    for (const auto& dev : m_joysticks) {
        if (dev.instance_id != event.jbutton.which) continue;
        std::string key = button_key_for_joystick(dev, event.jbutton.button);
        int vk = map_button_to_virtual_key(key);
        m_bridge->key_event(vk, 0);
        break;
    }
}

void InputManager::handle_joystick_added(SDL_JoystickID id) {
    JoystickDevice dev;
    dev.instance_id = id;

    SDL_GUID sdl_guid = SDL_GetJoystickGUIDForID(id);
    char guid_buf[33] = {};
    SDL_GUIDToString(sdl_guid, guid_buf, sizeof(guid_buf));
    dev.guid = guid_buf;

    const char* name = SDL_GetJoystickNameForID(id);
    dev.name = name ? name : "Unknown";

    if (SDL_IsGamepad(id)) {
        dev.gamepad = SDL_OpenGamepad(id);
        if (dev.gamepad) {
            dev.handle = SDL_GetGamepadJoystick(dev.gamepad);
        }
    } else {
        dev.handle = SDL_OpenJoystick(id);
    }

    if (!dev.handle) {
        std::cerr << "InputManager: Failed to open joystick: " << SDL_GetError() << std::endl;
        return;
    }

    dev.num_axes = SDL_GetNumJoystickAxes(dev.handle);
    dev.num_buttons = SDL_GetNumJoystickButtons(dev.handle);

    std::lock_guard<std::mutex> lock(m_joysticks_mutex);
    m_joysticks.push_back(dev);
    std::cout << "InputManager: Joystick added: " << dev.name
              << " (" << dev.num_axes << " axes, " << dev.num_buttons << " buttons)" << std::endl;
}

void InputManager::handle_joystick_removed(SDL_JoystickID id) {
    std::lock_guard<std::mutex> lock(m_joysticks_mutex);
    for (auto it = m_joysticks.begin(); it != m_joysticks.end(); ++it) {
        if (it->instance_id == id) {
            std::cout << "InputManager: Joystick removed: " << it->name << std::endl;
            if (it->gamepad) SDL_CloseGamepad(it->gamepad);
            else if (it->handle) SDL_CloseJoystick(it->handle);
            m_joysticks.erase(it);
            break;
        }
    }
}

int InputManager::map_button_to_virtual_key(const std::string& device_key) const {
    auto it = m_button_map.find(device_key);
    if (it != m_button_map.end()) return it->second;
    return 0;
}
