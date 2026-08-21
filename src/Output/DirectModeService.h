#pragma once

#include <string>

// Types Dasher's output into whichever window currently has keyboard focus,
// via ydotool/uinput. Mirrors what Dasher-Windows does with SendInput and
// Dasher-Apple with CGEvent posting.
class DirectModeService {
public:
    DirectModeService();
    ~DirectModeService() = default;

    // Attempt to type `text` into the focused window. Returns false when the
    // injection failed (most commonly: the ydotoold daemon isn't running, so
    // the binary exists but can't reach /dev/uinput). Callers must surface the
    // failure — keyboard mode used to fail silently, which read as "the button
    // does nothing".
    bool inject_text(const std::string& text);

    // Press backspace once per character (not byte) of `deleted_text`.
    bool inject_delete(const std::string& deleted_text);

    bool is_available() const;
    // Re-run the ydotool availability check (e.g. after the user installs it via
    // the setup dialog, issue #38) and update the cached result.
    bool recheck();

    // Number of UTF-8 code points in `text` (lead-byte count). Injected
    // backspaces must match characters, or multibyte output over-deletes.
    static int utf8_length(const std::string& text);

  private:
    bool m_available = false;
    // True when the ydotool binary exists AND the daemon answers. Arch-family
    // distros install the binary without enabling ydotoold, which previously
    // made is_available() lie — `which ydotool` alone is not enough.
    static bool probe_ydotool();
};
