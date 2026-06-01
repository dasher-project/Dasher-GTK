#pragma once

#include <string>

class DirectModeService {
public:
    DirectModeService();
    ~DirectModeService() = default;

    void inject_text(const std::string& text);
    void inject_delete(int count);

    bool is_available() const;
    static bool check_ydotool_installed();

private:
    bool m_available = false;
};
