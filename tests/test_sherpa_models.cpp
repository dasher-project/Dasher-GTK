// Unit tests for the Sherpa model catalogue, availability rules and
// the archive plumbing (tar reader, checksums, JSON). Everything here
// is offline: fixtures only, no network, no Rust library.
#include <doctest/doctest.h>

#include "Speech/Json.h"
#include "Speech/SherpaModels.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using Dasher::JsonValue;
using Dasher::SherpaModel;
using Dasher::sherpa_catalog_from_json;
namespace SherpaStorage = Dasher::SherpaStorage;

namespace {

std::string write_temp(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path.string();
}

const char* kTwoModels = R"json({
  "piper-en_GB-alan-medium": {
    "model_type": "vits", "engines": "sherpa-onnx", "developer": "piper",
    "name": "en_GB-alan-medium", "quality": "medium", "quantization": "fp32",
    "num_speakers": 1,
    "language": [{"lang_code": "en-GB", "language_name": "English", "country": "GB"}],
    "url": "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-piper-en_GB-alan-medium.tar.bz2",
    "filesize_mb": 64.11, "sha256": "a48d4017da0f77668b27",
    "license": "MIT", "readiness": "ready"
  },
  "mms_eng": {
    "model_type": "mms", "engines": "sherpa-onnx", "developer": "facebook",
    "name": "eng", "num_speakers": 1,
    "language": [{"lang_code": "en", "language_name": "English"}],
    "url": "https://huggingface.co/willwade/mms-tts-multilingual-models-onnx/resolve/main/eng",
    "filesize_mb": 0, "license": "CC-BY-NC", "readiness": "unverified"
  }
})json";

SherpaModel model_with(const std::string& overrides_json) {
    // Minimal valid model, then merge overrides.
    std::string base = R"json({
  "id": "x", "model_type": "vits", "engines": "sherpa-onnx",
  "developer": "piper", "name": "n", "num_speakers": 1,
  "language": [{"lang_code": "en", "language_name": "English"}],
  "url": "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-x.tar.bz2",
  "filesize_mb": 10, "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "license": "MIT", "readiness": "ready"
})json";
    // Parse both, copy fields from overrides onto base.
    JsonValue b = JsonValue::parse(base);
    JsonValue o = JsonValue::parse(overrides_json);
    std::string merged = "{";
    bool first = true;
    auto emit = [&](const std::string& k, const JsonValue& v) {
        if (!first) merged += ",";
        first = false;
        merged += "\"" + k + "\":";
        // Re-serialize the value (objects only in these tests).
        if (v.is_string()) merged += "\"" + v.as_string() + "\"";
        else if (v.is_number()) merged += std::to_string(v.as_number());
        else if (v.is_bool()) merged += v.as_bool() ? "true" : "false";
        else if (v.is_array()) {
            merged += "[";
            for (size_t i = 0; i < v.items().size(); i++) {
                if (i) merged += ",";
                const JsonValue& l = v.items()[i];
                merged += "{\"lang_code\":\"" + l.get_string("lang_code")
                    + "\",\"language_name\":\"" + l.get_string("language_name") + "\"}";
            }
            merged += "]";
        }
    };
    for (const auto& [k, v] : b.fields()) emit(k, v);
    for (const auto& [k, v] : o.fields()) emit(k, v);
    merged += "}";
    // The catalogue is keyed by id: honour an overridden id in the key.
    std::string key = "x";
    if (const JsonValue* id = o.find("id"); id && id->is_string()) key = id->as_string();
    auto models = sherpa_catalog_from_json("{\"" + key + "\":" + merged + "}");
    REQUIRE(models.size() == 1);
    return models[0];
}

} // namespace

TEST_CASE("JSON: scalars, escapes and errors") {
    JsonValue v = JsonValue::parse("{\"a\": [1, 2.5, \"x\\u00e9\", true, null]}");
    REQUIRE(v.is_object());
    REQUIRE(v.find("a")->items().size() == 5);
    CHECK(v.find("a")->items()[0].as_number() == 1);
    CHECK(v.find("a")->items()[1].as_number() == 2.5);
    CHECK(v.find("a")->items()[2].as_string() == "x\xc3\xa9");
    CHECK(v.find("a")->items()[3].as_bool());
    CHECK(v.find("a")->items()[4].is_null());

    std::string surrogate = "\"\\ud83d\\ude00\"";
    CHECK(JsonValue::parse(surrogate).as_string() == "\xF0\x9F\x98\x80");

    std::string err;
    JsonValue::parse("{\"a\":}", &err);
    CHECK(!err.empty());
    JsonValue::parse("{} trailing", &err);
    CHECK(!err.empty());
    CHECK(JsonValue::parse("").is_null());
}

TEST_CASE("catalogue: parses the registry shape") {
    auto models = sherpa_catalog_from_json(kTwoModels);
    REQUIRE(models.size() == 2);
    // Sorted by display name.
    CHECK(models[0].id == "mms_eng");
    CHECK(models[1].id == "piper-en_GB-alan-medium");

    const SherpaModel& piper = models[1];
    CHECK(piper.display_name() == "Piper en_GB-alan-medium medium");
    CHECK(piper.languages_label() == "English");
    CHECK(piper.size_label() == "64.1 MB");
    // isEnglishOnly is strict "en" (Dasher-Apple parity): en-GB is a
    // locale, not the English-only default.
    CHECK_FALSE(piper.is_english_only());
    // …and non-"en" piper voices without a g2p bundle are exactly the
    // ones Dasher cannot auto-lexicon.
    auto reason = SherpaStorage::download_unavailable_reason(piper);
    REQUIRE(reason.has_value());
    CHECK(reason->find("pronunciation dictionary") != std::string::npos);

    const SherpaModel& mms = models[0];
    CHECK(mms.model_type == "mms");
    // MMS from the hub is installable per-file.
    CHECK(!SherpaStorage::download_unavailable_reason(mms).has_value());

    // A plain en (US) piper voice is installable: the installer fetches
    // the pinned en-US lexicon for it.
    CHECK(!SherpaStorage::download_unavailable_reason(
        model_with("{\"language\": [{\"lang_code\": \"en\", \"language_name\": \"English\"}]}"))
        .has_value());
}

TEST_CASE("catalogue: search matches across fields") {
    auto models = sherpa_catalog_from_json(kTwoModels);
    const SherpaModel& piper = models[1];
    CHECK(piper.matches("alan"));
    CHECK(piper.matches("PIPER ALAN"));       // case-insensitive, all terms
    CHECK(piper.matches("en-gb"));            // lang code
    CHECK_FALSE(piper.matches("alan french")); // one term missing
    CHECK(piper.matches(""));                  // empty matches everything
}

TEST_CASE("availability: readiness blocks and engine gates") {
    CHECK(SherpaStorage::download_unavailable_reason(
              model_with("{\"readiness\": \"blocked:deprecated\"}"))
              .value_or("") == "This model is deprecated.");
    CHECK(SherpaStorage::download_unavailable_reason(
              model_with("{\"readiness\": \"blocked:no-bundle\"}"))
              .value_or("").find("pronunciation dictionary") != std::string::npos);
    // floravox-only engines are not installable here.
    CHECK(SherpaStorage::download_unavailable_reason(
              model_with("{\"engines\": \"floravox\"}"))
              .value_or("") == "Requires a different speech engine.");
    // No checksum → no verified download.
    CHECK(SherpaStorage::download_unavailable_reason(
              model_with("{\"sha256\": \"\"}"))
              .value_or("").find("no verified download") != std::string::npos);
    // The pinned replacement digests count as a checksum.
    CHECK_FALSE(SherpaStorage::download_unavailable_reason(
        model_with("{\"id\": \"icefall-en-ljspeech\", "
                   "\"url\": \"https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/icefall-en-ljspeech.tar.bz2\", "
                   "\"model_type\": \"matcha\", \"developer\": \"icefall\", \"sha256\": \"\"}"))
        .has_value());
    // Non-English piper without a g2p bundle needs a lexicon Dasher
    // cannot supply.
    CHECK(SherpaStorage::download_unavailable_reason(
              model_with("{\"language\": [{\"lang_code\": \"fr\", \"language_name\": \"French\"}]}"))
              .value_or("").find("pronunciation dictionary") != std::string::npos);
}

TEST_CASE("storage: models dir and install marker") {
    // Point HOME at a scratch dir.
    fs::path scratch = fs::temp_directory_path() / ("dasher-sherpa-test-" + std::to_string(::getpid()));
    fs::remove_all(scratch);
    fs::create_directories(scratch);
#ifdef _WIN32
    _putenv_s("HOME", scratch.string().c_str());
    _putenv_s("USERPROFILE", scratch.string().c_str());
#else
    setenv("HOME", scratch.string().c_str(), 1);
#endif
    CHECK(fs::path(SherpaStorage::models_dir()) == scratch / ".rust-tts-wrapper" / "sherpaonnx");

    fs::path dir = scratch / ".rust-tts-wrapper" / "sherpaonnx" / "test-model";
    fs::create_directories(dir);
    CHECK_FALSE(SherpaStorage::is_installed("test-model"));
    {
        std::ofstream marker(dir / ".dasher-verified-model", std::ios::trunc);
        marker << "test-model\n";
    }
    CHECK(SherpaStorage::is_installed("test-model"));
    CHECK(SherpaStorage::installed_ids().count("test-model") == 1);
    // Path traversal ids are rejected outright.
    CHECK_FALSE(SherpaStorage::is_installed("../escape"));
    fs::remove_all(scratch);
}
