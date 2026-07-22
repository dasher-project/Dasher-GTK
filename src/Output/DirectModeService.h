#pragma once

#include <string>

class DirectModeService {
public:
    DirectModeService();
    ~DirectModeService() = default;

    void inject_text(const std::string& text);
    void inject_delete(int count);

    bool is_available() const;
    // Re-run the ydotool availability check (e.g. after the user installs it via
    // the setup dialog, issue #38) and update the cached result.
    bool recheck();
    static bool check_ydotool_installed();

private:
    bool m_available = false;
};
