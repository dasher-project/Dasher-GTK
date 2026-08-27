#pragma once

#include <string>

// RFC 0017: passive in-app update check for self-managed builds (GitHub
// Releases). Runs on a background thread after the window is up, at most
// once per week. On a newer release, the caller shows a non-modal
// notification with a link — never a modal dialog, never a download.
//
// Flatpak builds skip the check (the system's `flatpak update` owns updates).
// The "Check for updates" opt-out lives in Preferences → Privacy.
class UpdateChecker {
  public:
    struct UpdateInfo {
        bool available = false;
        std::string latest_tag; // e.g. "v0.3.0"
        std::string current_version;
        std::string release_url;  // link to the GitHub release page
        std::string release_name; // human-readable title
    };

    // Compare two version strings semantically (ignoring leading "v").
    // Returns true if latest > current.
    static bool is_newer(const std::string& latest, const std::string& current);

    // Fetch the latest release from the GitHub API. Blocking (call from a
    // worker thread). Returns UpdateInfo with available=false on any error
    // or if the check should be skipped (Flatpak, rate-limited, offline).
    static UpdateInfo check(const std::string& current_version);

    // True if we should skip the check entirely (Flatpak sandbox).
    static bool is_managed_build();

    // Persistent state: last-check timestamp and skip-version (both stored in
    // the same config file as the analytics opt-in).
    static bool should_check();
    static void record_check(const std::string& skipped_version = "");

  private:
    static constexpr const char* kRepoOwner = "dasher-project";
    static constexpr const char* kRepoName = "Dasher-GTK";
    static constexpr int kCheckIntervalHours = 168; // 7 days
    static constexpr int kHttpTimeoutSeconds = 10;
};
