#include "SherpaModels.h"
#include "Json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace fs = std::filesystem;

namespace Dasher {

namespace {

std::string ascii_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    return ascii_lower(haystack).find(ascii_lower(needle)) != std::string::npos;
}

// Model ids must stay inside the models directory: alphanumeric start
// then [A-Za-z0-9_.-]. Mirrors SpeechModelStorage.isModelID.
bool is_model_id(const std::string& id) {
    if (id.empty()) return false;
    if (!std::isalnum(static_cast<unsigned char>(id[0]))) return false;
    for (char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

// "willwade" → "Willwade" (word-capitalise, mirroring Swift's
// .capitalized for the registry's single-word developers).
std::string capitalized_words(const std::string& s) {
    std::string out = s;
    bool at_word_start = true;
    for (char& c : out) {
        bool alnum = std::isalnum(static_cast<unsigned char>(c)) != 0;
        if (at_word_start && alnum) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        at_word_start = !alnum;
    }
    return out;
}

SherpaModel model_from_json(const std::string& id, const JsonValue& v) {
    SherpaModel m;
    m.id = id;
    m.model_type = v.get_string("model_type");
    m.engines = v.get_string("engines");
    m.developer = v.get_string("developer");
    m.name = v.get_string("name");
    m.quality = v.get_string("quality");
    m.quantization = v.get_string("quantization");
    m.num_speakers = static_cast<long>(v.get_number("num_speakers"));
    m.url = v.get_string("url");
    m.filesize_mb = v.get_number("filesize_mb");
    m.sha256 = v.get_string("sha256");
    m.license = v.get_string("license");
    m.license_url = v.get_string("license_url");
    m.deprecated = v.get_bool("deprecated", false);
    m.readiness = v.get_string("readiness");
    if (const JsonValue* langs = v.find("language"); langs && langs->is_array()) {
        for (const JsonValue& l : langs->items()) {
            SherpaLanguage lang;
            lang.lang_code = l.get_string("lang_code");
            lang.language_name = l.get_string("language_name");
            lang.country = l.get_string("country");
            m.language.push_back(std::move(lang));
        }
    }
    return m;
}

} // namespace

std::string SherpaModel::display_name() const {
    if (id == SherpaStorage::kEnglishModelId) return "Inflect Nano v2";
    std::string developer = capitalized_words(this->developer);
    std::vector<std::string> parts;
    if (!developer.empty()) parts.push_back(developer);
    if (!name.empty()) parts.push_back(name);
    if (!quality.empty() && quality != "unknown") parts.push_back(quality);
    if (!quantization.empty() && quantization != "fp32") parts.push_back(quantization);
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += ' ';
        out += parts[i];
    }
    return out;
}

std::string SherpaModel::languages_label() const {
    // Deduplicated language names, sorted.
    std::set<std::string> names;
    for (const auto& l : language) {
        if (!l.language_name.empty()) names.insert(l.language_name);
    }
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

std::string SherpaModel::size_label() const {
    if (filesize_mb <= 0) return "";
    double mb = filesize_mb;
    char buf[32];
    if (mb >= 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f GB", mb / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    }
    return buf;
}

bool SherpaModel::is_english_only() const {
    return language.size() == 1 && language[0].lang_code == "en";
}

bool SherpaModel::uses_g2p_bundle() const {
    return readiness == "needs_bundle";
}

bool SherpaModel::matches(const std::string& search) const {
    // Whitespace-separated terms; every term must match somewhere.
    std::string codes;
    for (const auto& l : language) {
        if (!codes.empty()) codes += ' ';
        codes += l.lang_code;
    }
    std::string haystacks[5] = {display_name(), id, model_type, languages_label(), codes};

    std::string lowered = ascii_lower(search);
    size_t start = 0;
    while (start < lowered.size()) {
        size_t end = lowered.find_first_of(" \t", start);
        if (end == std::string::npos) end = lowered.size();
        if (end > start) {
            std::string term = lowered.substr(start, end - start);
            bool any = false;
            for (const auto& h : haystacks) {
                if (contains_ci(h, term)) {
                    any = true;
                    break;
                }
            }
            if (!any) return false;
        }
        start = end + 1;
    }
    return true;
}

std::vector<SherpaModel> sherpa_catalog_from_json(const std::string& json) {
    std::vector<SherpaModel> models;
    std::string error;
    JsonValue root = JsonValue::parse(json, &error);
    if (!root.is_object()) return models;
    for (const auto& [id, v] : root.fields()) {
        if (!v.is_object()) continue;
        SherpaModel m = model_from_json(id, v);
        if (m.id.empty() || m.model_type.empty() || m.url.empty()) continue;
        models.push_back(std::move(m));
    }
    std::sort(models.begin(), models.end(), [](const SherpaModel& a, const SherpaModel& b) {
        if (a.id == SherpaStorage::kEnglishModelId) return b.id != SherpaStorage::kEnglishModelId;
        if (b.id == SherpaStorage::kEnglishModelId) return false;
        std::string da = ascii_lower(a.display_name()), db = ascii_lower(b.display_name());
        if (da != db) return da < db;
        return a.id < b.id;
    });
    return models;
}

namespace SherpaStorage {

const char* const kEnglishModelId = "inflect-en-nano-v2";

namespace {
const char* const kVerificationFile = ".dasher-verified-model";
const char* const kGithubAssets = "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/";
} // namespace

std::string models_dir() {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    fs::path dir = home ? fs::path(home) : fs::path(".");
    return (dir / ".rust-tts-wrapper" / "sherpaonnx").string();
}

// The upstream release replaced these files after registry 1.1.0 —
// pinned replacement digests until the registry includes them
// (AACTools/sherpa-onnx-tts-models#3). Mirrors SpeechModelStorage
// .archiveChecksum(for:) from Dasher-Apple.
std::string archive_checksum_override(const std::string& model_id) {
    static const std::map<std::string, std::string> kUpdated = {
        {"kokoro-zh_en-int8", "259c9c8bc906d6078e4e0570421885dabb68b539c2f970fcbb890a58eeaf3e9c"},
        {"kokoro-zh_en-multi-lang", "b78e1ac42b852e839cceaafbe88f66806bf5860992d1d7b9a3f2ad1198427aed"},
        {"icefall-en-ljspeech", "cff849fee69d7d3b22e0b0267aa6b863e32d05039a1eca963765a314a73450c0"},
        {"icefall-en-ljspeech-low", "e5ae18c97b566099a2a748f243d4d44ea7cd188acc34e3533a2adc2565c9f845"},
        {"icefall-en-ljspeech-medium", "93efae74fe0c72199c51b11ca7acb20b4966c2dc8b728109910266784f39f720"},
        {"icefall-zh-baker", "ae70096b19c6e283ea351a26b8a718ce6427c8bde8c1da22eb46a5478d39a69b"},
    };
    auto it = kUpdated.find(model_id);
    return it == kUpdated.end() ? std::string() : it->second;
}

namespace {

bool contains_nonempty_file(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    fs::path p = dir / name;
    return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 0 && !ec;
}

bool has_lexicon(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string fn = entry.path().filename().string();
        if (fn.rfind("lexicon", 0) == 0 && entry.path().extension() == ".txt" && contains_nonempty_file(dir, fn)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool is_installed(const std::string& model_id) {
    if (!is_model_id(model_id)) return false;
    fs::path folder = fs::path(models_dir()) / model_id;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return false;
    if (model_id == kEnglishModelId) {
        return contains_nonempty_file(folder, "model.onnx") && contains_nonempty_file(folder, "tokens.txt") &&
               contains_nonempty_file(folder, "lexicon.txt");
    }
    std::ifstream marker(folder / kVerificationFile);
    std::string content((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
    // Trim the trailing newline the installer writes.
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
        content.pop_back();
    }
    return content == model_id;
}

std::set<std::string> installed_ids() {
    std::set<std::string> ids;
    std::error_code ec;
    fs::path dir = models_dir();
    if (!fs::is_directory(dir, ec)) return ids;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::string id = entry.path().filename().string();
        if (is_installed(id)) ids.insert(id);
    }
    return ids;
}

std::optional<std::string> download_unavailable_reason(const SherpaModel& model) {
    // Engines this build offers: sherpa-onnx only (no floravox).
    bool sherpa_engine = false;
    {
        std::string engines = model.engines;
        size_t start = 0;
        while (start <= engines.size()) {
            size_t end = engines.find(',', start);
            std::string one = engines.substr(start, end == std::string::npos ? engines.size() - start : end - start);
            // trim
            size_t b = one.find_first_not_of(" \t");
            size_t e = one.find_last_not_of(" \t");
            one = (b == std::string::npos) ? std::string() : one.substr(b, e - b + 1);
            if (one == "sherpa-onnx") {
                sherpa_engine = true;
                break;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        // An absent engines field means the registry did not tag the
        // model — treat like Dasher-Apple (nil engines passes).
        if (model.engines.empty()) sherpa_engine = true;
    }
    if (!sherpa_engine) return std::string("Requires a different speech engine.");

    if (model.readiness.rfind("blocked:", 0) == 0) {
        if (model.readiness == "blocked:no-bundle" || model.readiness == "blocked:no-language")
            return std::string("This model needs a pronunciation dictionary that Dasher cannot install yet.");
        if (model.readiness == "blocked:bundle-not-validated")
            return std::string("This model's pronunciation dictionary has not been validated yet.");
        if (model.readiness == "blocked:g2p-unavailable")
            return std::string("This build cannot use the pronunciation dictionary this model needs.");
        if (model.readiness == "blocked:deprecated") return std::string("This model is deprecated.");
        return std::string("This model has no verified download that Dasher can install.");
    }

    // The GitHub tarball path needs a verified checksum — either in the
    // registry or among the post-registry release-replacement digests
    // (SherpaModelInstaller applies the same map when verifying).
    bool has_checksum = !model.sha256.empty() || !archive_checksum_override(model.id).empty();

    std::string path = model.url;
    size_t last_slash = path.find_last_of('/');
    std::string filename = last_slash == std::string::npos ? path : path.substr(last_slash + 1);

    bool github_tarball = model.url.rfind(kGithubAssets, 0) == 0 && filename.size() > 8 &&
                          filename.compare(filename.size() - 8, 8, ".tar.bz2") == 0;

    // MMS models fetched per-file from Hugging Face are installable
    // without a tarball checksum (each file carries its own).
    bool mms_from_hub = model.model_type == "mms" && model.url.rfind("https://huggingface.co/", 0) == 0;
    if (!mms_from_hub && (!is_model_id(model.id) || !github_tarball || !has_checksum)) {
        return std::string("This model has no verified download that Dasher can install.");
    }

    if (model.model_type == "vits" && model.developer == "piper" && !model.is_english_only() &&
        !model.uses_g2p_bundle()) {
        return std::string("This model needs a pronunciation dictionary that Dasher cannot install yet.");
    }

    // Everything else with a verified download is offered; the
    // installer's per-type validation catches incomplete archives
    // (mirrors Dasher-Apple, whose switch falls through to nil).
    return std::nullopt;
}

} // namespace SherpaStorage

} // namespace Dasher
