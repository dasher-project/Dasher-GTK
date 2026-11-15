// Sherpa-ONNX model catalogue: typed view over the wrapper's embedded
// registry (tts_get_sherpa_models_json), plus the local-installation
// bookkeeping that mirrors Dasher-Apple's SpeechModelStorage — same
// verification-file convention (.dasher-verified-model), same model
// directory layout (~/.rust-tts-wrapper/sherpaonnx, the wrapper's
// default model dir), same download-availability rules.
//
// FFI-free on purpose: unit tests compile these translation units
// without the Rust library.
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Dasher {

struct SherpaLanguage {
    std::string lang_code;
    std::string language_name;
    std::string country;
};

struct SherpaModel {
    std::string id;
    std::string model_type;
    std::string engines; // comma-separated ("sherpa-onnx,floravox")
    std::string developer;
    std::string name;
    std::vector<SherpaLanguage> language;
    std::string quality;
    std::string quantization;
    long num_speakers = 0;
    std::string url;
    double filesize_mb = 0.0;
    std::string sha256;
    std::string license;
    std::string license_url;
    bool deprecated = false;
    std::string readiness;

    // "Developer Name quality quant" like Dasher-Apple's displayName;
    // the default English model keeps its friendly name.
    std::string display_name() const;
    std::string languages_label() const; // "English, French"
    std::string size_label() const;      // "21.4 MB" (empty when unknown)
    bool is_english_only() const;
    bool uses_g2p_bundle() const; // readiness == "needs_bundle"

    // Case-insensitive all-terms match over display name, id, model
    // type, language names and language codes.
    bool matches(const std::string& search) const;
};

// Parse the registry JSON (an object keyed by model id). Invalid
// entries are skipped rather than fatal. Result is sorted with the
// default English model first, then display name (case-insensitive).
std::vector<SherpaModel> sherpa_catalog_from_json(const std::string& json);

// Local installation state + availability rules.
namespace SherpaStorage {

// The wrapper's default model dir: $HOME/.rust-tts-wrapper/sherpaonnx.
// Models installed here are found by the engine without a modelPath
// credential.
std::string models_dir();

// The model the Speech page offers as the friendly default download.
extern const char* const kEnglishModelId; // "inflect-en-nano-v2"

// Pinned replacement sha256 for archives the upstream release
// replaced after registry 1.1.0 (empty when the registry digest
// applies). The installer verifies against this first.
std::string archive_checksum_override(const std::string& model_id);

bool is_installed(const std::string& model_id);
std::set<std::string> installed_ids();

// Why Dasher cannot install this model (empty = installable):
// missing verified checksum, non-github archive, blocked readiness,
// piper voices that need a lexicon Dasher cannot supply, or engines
// this build does not have. Mirrors SpeechModelStorage
// .downloadUnavailableReason(for:) from Dasher-Apple.
std::optional<std::string> download_unavailable_reason(const SherpaModel& model);

} // namespace SherpaStorage

} // namespace Dasher
