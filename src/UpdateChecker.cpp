#include "UpdateChecker.h"
#include "UiSettings.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef HAS_ANALYTICS_CURL
#include <curl/curl.h>
#endif

namespace {

std::string get_state_path() {
    char* dir = g_build_filename(g_get_user_config_dir(), "dasher", nullptr);
    char* path = g_build_filename(dir, "update-check.conf", nullptr);
    std::string result = path ? path : "";
    g_free(dir);
    g_free(path);
    return result;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (f.is_open()) f << content;
}

// Very small JSON field extractor: returns the string value for a simple
// "tag_name": "v0.3.0" or "html_url": "https://..." pattern. Not a parser —
// just enough for the GitHub Releases API's flat response.
std::string extract_json_string(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    pos++; // skip opening quote
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++; // skip escaped char
        }
        result += json[pos++];
    }
    return result;
}

} // namespace

bool UpdateChecker::is_newer(const std::string& latest, const std::string& current) {
    // Strip leading 'v'
    std::string l = latest.substr(latest[0] == 'v' ? 1 : 0);
    std::string c = current.substr(current[0] == 'v' ? 1 : 0);

    // Split on '.' and compare numerically
    int lMajor = 0, lMinor = 0, lPatch = 0;
    int cMajor = 0, cMinor = 0, cPatch = 0;
    std::sscanf(l.c_str(), "%d.%d.%d", &lMajor, &lMinor, &lPatch);
    std::sscanf(c.c_str(), "%d.%d.%d", &cMajor, &cMinor, &cPatch);

    if (lMajor != cMajor) return lMajor > cMajor;
    if (lMinor != cMinor) return lMinor > cMinor;
    return lPatch > cPatch;
}

bool UpdateChecker::is_managed_build() {
    // Flatpak: the sandbox has FLATPAK_ID or /.flatpak-info
    if (std::getenv("FLATPAK_ID")) return true;
    std::error_code ec;
    return std::filesystem::exists("/.flatpak-info", ec);
}

bool UpdateChecker::should_check() {
    if (is_managed_build()) return false;

    const std::string state = read_file(get_state_path());
    if (state.empty()) return true; // never checked

    // State file format: "last_check=<epoch>\nskip=<version>\nenabled=<true|false>"
    std::string skip_version;
    long last_check = 0;
    bool enabled = true;
    std::istringstream iss(state);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("last_check=", 0) == 0) {
            last_check = std::atol(line.substr(11).c_str());
        } else if (line.rfind("skip=", 0) == 0) {
            skip_version = line.substr(5);
        } else if (line.rfind("enabled=", 0) == 0) {
            enabled = line.substr(8) == "true";
        }
    }

    if (!enabled) return false;

    // Check interval
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const long now_s = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
    if (now_s - last_check < kCheckIntervalHours * 3600) {
        return false; // checked recently
    }

    // If the user skipped this specific version, don't nag (but still
    // allow a future version to trigger the notification)
    // (skip_version check happens in the caller against the fetched tag)

    return true;
}

void UpdateChecker::record_check(const std::string& skipped_version) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const long now_s = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
    std::ostringstream ss;
    ss << "last_check=" << now_s << "\n";
    if (!skipped_version.empty()) {
        ss << "skip=" << skipped_version << "\n";
    }
    write_file(get_state_path(), ss.str());
}

UpdateChecker::UpdateInfo UpdateChecker::check(const std::string& current_version) {
    UpdateInfo info;
    info.current_version = current_version;
    info.available = false;

    if (is_managed_build()) return info;

#ifdef HAS_ANALYTICS_CURL
    const std::string url = "https://api.github.com/repos/" + std::string(kRepoOwner) + "/" +
                             std::string(kRepoName) + "/releases?per_page=1";
    const std::string release_page = "https://github.com/" + std::string(kRepoOwner) + "/" +
                                      std::string(kRepoName) + "/releases/latest";

    CURL* curl = curl_easy_init();
    if (!curl) return info;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kHttpTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Dasher-GTK/" DASHER_GTK_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* buf = static_cast<std::string*>(userdata);
        buf->append(static_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || http_code != 200) return info;

    info.latest_tag = extract_json_string(response, "tag_name");
    info.release_url = extract_json_string(response, "html_url");
    info.release_name = extract_json_string(response, "name");
    if (info.release_url.empty()) info.release_url = release_page;

    if (!info.latest_tag.empty() && is_newer(info.latest_tag, current_version)) {
        info.available = true;
    }
#endif

    return info;
}
