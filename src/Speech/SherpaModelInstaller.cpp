#include "SherpaModelInstaller.h"
#include "Json.h"

// The installer needs curl + bzip2 and only backs the Sherpa model
// browser (HAS_TTS_DOWNLOAD: Linux builds with the sherpaonnx engine).
// Other platforms compile this translation unit empty rather than
// requiring bzlib.h — Windows/macOS GTK builds use their system/cloud
// engines and never see the model browser.
#ifdef HAS_TTS_DOWNLOAD

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <bzlib.h>
#include <curl/curl.h>
#include <glib.h>

namespace fs = std::filesystem;

namespace Dasher {

namespace {

constexpr size_t kMiB = 1024 * 1024;
// Matches Dasher-Apple's decompression guard.
constexpr long long kMaxTarBytes = 2LL * 1024 * kMiB;
// Plain (non-LFS) HF files larger than this are refused — same guard
// as SpeechModelStorage.downloadMMS.
constexpr long long kMaxGitBlobBytes = 16 * kMiB;

const char* const kGithubAssets = "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/";
const char* const kVocoderRelease = "https://github.com/k2-fsa/sherpa-onnx/releases/download/vocoder-models/";
const char* const kLexiconEnUs = "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/lexicon-en-us.txt";
const char* const kLexiconEnUsSha256 = "8b67ae549a465eee78f92ecb74644ad90c3c58ce65f19c68baf85878a10ef73a";

const char* const kHub = "https://huggingface.co";
const char* const kMmsRepository = "willwade/mms-tts-multilingual-models-onnx";
const char* const kMmsRevision = "4f49fc254f11ddb0e35fbf25b6e48b04463a6779";

// ── small helpers ──────────────────────────────────────────────────

struct InstallError {
    std::string message;
};

[[noreturn]] void fail(const std::string& message) {
    throw InstallError{message};
}

bool file_nonempty(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 0 && !ec;
}

bool dir_contains_nonempty_file(const fs::path& dir, const std::string& name) {
    return file_nonempty(dir / name);
}

bool dir_has_lexicon(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string fn = entry.path().filename().string();
        if (fn.rfind("lexicon", 0) == 0 && entry.path().extension() == ".txt" && file_nonempty(entry.path())) {
            return true;
        }
    }
    return false;
}

bool dir_has_onnx(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() == ".onnx" && file_nonempty(entry.path())) return true;
    }
    return false;
}

std::vector<fs::path> dir_entries(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            out.push_back(entry.path());
        }
    }
    return out;
}

// ── checksums (GLib: SHA-256 for archives/LFS, SHA-1 for git blobs) ─

enum class ChecksumKind { None, Sha256, GitBlobSha1 };

struct Checksum {
    ChecksumKind kind = ChecksumKind::None;
    std::string hex;    // lowercase expected digest
    long long size = 0; // git-blob only

    static Checksum sha256(const std::string& h) { return {ChecksumKind::Sha256, h, 0}; }
    static Checksum git_blob(const std::string& h, long long bytes) { return {ChecksumKind::GitBlobSha1, h, bytes}; }
    bool valid() const {
        if (kind == ChecksumKind::None) return false;
        if (hex.size() != 64 && kind == ChecksumKind::Sha256) return false;
        if (hex.size() != 40 && kind == ChecksumKind::GitBlobSha1) return false;
        for (char c : hex) {
            bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!ok) return false;
        }
        return true;
    }
};

bool file_matches_checksum(const fs::path& file, const Checksum& expected) {
    if (expected.kind == ChecksumKind::None) return false;
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return false;
    GChecksumType type = expected.kind == ChecksumKind::Sha256 ? G_CHECKSUM_SHA256 : G_CHECKSUM_SHA1;
    GChecksum* cs = g_checksum_new(type);
    if (expected.kind == ChecksumKind::GitBlobSha1) {
        // A git blob digest hashes the literal "blob <size>\0" header
        // before the content — the NUL must survive string append.
        std::string header = "blob " + std::to_string(expected.size);
        header += '\0';
        g_checksum_update(cs, reinterpret_cast<const guchar*>(header.data()), static_cast<gsize>(header.size()));
    }
    std::vector<char> buffer(kMiB);
    size_t n;
    bool size_ok = true;
    long long total = 0;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), f)) > 0) {
        g_checksum_update(cs, reinterpret_cast<const guchar*>(buffer.data()), static_cast<gsize>(n));
        total += static_cast<long long>(n);
        if (expected.kind == ChecksumKind::GitBlobSha1 && total > expected.size) {
            size_ok = false;
            break;
        }
    }
    std::fclose(f);
    if (!size_ok) return false;
    if (expected.kind == ChecksumKind::GitBlobSha1 && total != expected.size) return false;
    const char* digest = g_checksum_get_string(cs);
    std::string actual = digest ? digest : "";
    g_checksum_free(cs);
    std::string want = expected.hex;
    for (auto& c : want)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return actual == want;
}

// ── HTTP fetch (libcurl) ───────────────────────────────────────────

struct FetchCtx {
    const SherpaModelInstaller::CancelFn* cancelled = nullptr;
    const SherpaModelInstaller::ProgressFn* progress = nullptr;
    const char* stage = "";
    std::string error;
};

size_t fetch_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return out->good() ? size * nmemb : 0;
}

int fetch_progress_cb(void* userdata, curl_off_t /*dltotal*/, curl_off_t dlnow, curl_off_t /*ultotal*/,
                      curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<FetchCtx*>(userdata);
    if (ctx->cancelled && (*ctx->cancelled)()) {
        ctx->error = "cancelled";
        return 1; // aborts the transfer
    }
    if (ctx->progress) (*ctx->progress)(-1.0, ctx->stage);
    return 0;
}

void http_get_to_file(const std::string& url, const fs::path& destination, const Checksum& checksum, FetchCtx& ctx) {
    fs::create_directories(destination.parent_path());
    fs::path partial = destination;
    partial += ".part";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) fail("Cannot write to the models directory.");

    CURL* curl = curl_easy_init();
    if (!curl) fail("Cannot start the downloader.");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // GitHub release redirects
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, fetch_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Dasher-GTK");
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    out.close();
    if (rc == CURLE_ABORTED_BY_CALLBACK || ctx.error == "cancelled") {
        std::error_code ec;
        fs::remove(partial, ec);
        fail("Download cancelled.");
    }
    if (rc != CURLE_OK || status != 200) {
        std::error_code ec;
        fs::remove(partial, ec);
        fail("Could not download the speech model.");
    }
    if (!file_matches_checksum(partial, checksum)) {
        std::error_code ec;
        fs::remove(partial, ec);
        fail("The downloaded model failed its checksum verification.");
    }
    fs::rename(partial, destination);
}

std::string http_get_to_string(const std::string& url, FetchCtx& ctx) {
    std::ostringstream ss;
    struct Sink {
        std::ostringstream* out;
        static size_t write(char* ptr, size_t size, size_t nmemb, void* userdata) {
            auto* self = static_cast<Sink*>(userdata);
            self->out->write(ptr, static_cast<std::streamsize>(size * nmemb));
            return size * nmemb;
        }
    } sink{&ss};
    CURL* curl = curl_easy_init();
    if (!curl) fail("Cannot start the downloader.");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Sink::write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, fetch_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Dasher-GTK");
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc == CURLE_ABORTED_BY_CALLBACK || ctx.error == "cancelled") fail("Download cancelled.");
    if (rc != CURLE_OK) fail("Could not reach the model host.");
    return ss.str();
}

// ── bzip2 + tar ────────────────────────────────────────────────────

void decompress_bz2(const fs::path& archive, const fs::path& tar, const SherpaModelInstaller::CancelFn& cancelled) {
    FILE* in = std::fopen(archive.c_str(), "rb");
    if (!in) fail("Cannot read the downloaded archive.");
    int bz_error = BZ_OK;
    BZFILE* reader = BZ2_bzReadOpen(&bz_error, in, 0, 0, nullptr, 0);
    if (!reader || bz_error != BZ_OK) {
        std::fclose(in);
        fail("The downloaded archive is not a valid bzip2 stream.");
    }
    std::ofstream out(tar, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        BZ2_bzReadClose(&bz_error, reader);
        std::fclose(in);
        fail("Cannot write to the models directory.");
    }
    std::vector<char> buffer(kMiB);
    long long expanded = 0;
    bool done = false;
    while (!done) {
        if (cancelled()) {
            out.close();
            BZ2_bzReadClose(&bz_error, reader);
            std::fclose(in);
            fail("Download cancelled.");
        }
        int count = BZ2_bzRead(&bz_error, reader, buffer.data(), static_cast<int>(buffer.size()));
        if (bz_error == BZ_OK || bz_error == BZ_STREAM_END) {
            if (count > 0) {
                expanded += count;
                if (expanded > kMaxTarBytes) {
                    out.close();
                    BZ2_bzReadClose(&bz_error, reader);
                    std::fclose(in);
                    fail("The downloaded archive expands beyond the 2 GB safety limit.");
                }
                out.write(buffer.data(), count);
            }
            if (bz_error == BZ_STREAM_END) done = true;
        } else {
            out.close();
            BZ2_bzReadClose(&bz_error, reader);
            std::fclose(in);
            fail("The downloaded archive is corrupted (bzip2 stream, error " + std::to_string(bz_error) + ").");
        }
    }
    out.close();
    BZ2_bzReadClose(&bz_error, reader);
    std::fclose(in);
}

// Minimal ustar reader. Mirrors Dasher-Apple's TarReader semantics:
// regular files only, espeak-ng-data skipped (no-espeak policy),
// path-traversal components refused, no overwriting.
void extract_tar(const fs::path& tar, const fs::path& dest, const SherpaModelInstaller::CancelFn& cancelled) {
    std::ifstream in(tar, std::ios::binary);
    if (!in.is_open()) fail("Cannot read the extracted archive.");
    std::array<char, 512> header{};
    while (in.read(header.data(), header.size()) || in.gcount() == 512) {
        if (in.gcount() != 512) break;
        if (header[0] == '\0') break; // end-of-archive marker block
        if (cancelled()) fail("Download cancelled.");

        // Numeric fields are NUL/space-padded octal.
        auto octal = [&](size_t offset, size_t len) -> long long {
            long long value = 0;
            for (size_t i = offset; i < offset + len && i < header.size(); i++) {
                char c = header[i];
                if (c == '\0' || c == ' ') {
                    if (i == offset) continue; // leading padding
                    break;
                }
                if (c < '0' || c > '7') return -1;
                value = value * 8 + (c - '0');
            }
            return value;
        };

        long long size = octal(124, 12);
        if (size < 0) fail("The downloaded archive is corrupted (tar size field).");
        char type = header[156];

        std::string name(header.data(), &header[100] - header.data());
        name = std::string(name.c_str()); // stop at NUL
        if (header[257] == 'u' && header[258] == 's' && header[259] == 't' && header[260] == 'a' &&
            header[261] == 'r') {
            std::string prefix(&header[345], &header[500] - &header[345]);
            prefix = std::string(prefix.c_str());
            if (!prefix.empty()) name = prefix + "/" + name;
        }

        long long blocks = (size + 511) / 512;
        bool data_consumed = false;
        if (type == '0' || type == '\0') { // regular file
            // Reject absolute paths and traversal components.
            std::vector<std::string> parts;
            std::string segment;
            std::stringstream path(name);
            while (std::getline(path, segment, '/'))
                parts.push_back(segment);
            bool safe = !parts.empty();
            for (const auto& p : parts) {
                if (p.empty() || p == "." || p == "..") safe = false;
            }
            if (!safe) fail("The downloaded archive contains an unsafe path.");

            bool is_espeak = false;
            for (const auto& p : parts) {
                if (p == "espeak-ng-data") {
                    is_espeak = true;
                    break;
                }
            }

            fs::path target = dest;
            for (const auto& p : parts)
                target /= p;
            if (!is_espeak) {
                fs::create_directories(target.parent_path());
                if (fs::exists(target)) fail("The downloaded archive overwrites files.");
                std::ofstream f(target, std::ios::binary | std::ios::trunc);
                long long remaining = size;
                std::vector<char> filebuf(kMiB);
                while (remaining > 0) {
                    long long chunk = std::min<long long>(remaining, static_cast<long long>(filebuf.size()));
                    in.read(filebuf.data(), chunk);
                    if (in.gcount() != chunk) fail("The downloaded archive is truncated.");
                    f.write(filebuf.data(), chunk);
                    remaining -= chunk;
                }
                data_consumed = true; // data was read inline
            }
        }
        // Skip the data blocks of non-regular entries and of skipped
        // files. For files written inline, only the block padding
        // after `size` bytes remains to be skipped.
        if (data_consumed) {
            long long padding = blocks * 512 - size;
            if (padding > 0) in.seekg(padding, std::ios::cur);
        } else {
            in.seekg(blocks * 512, std::ios::cur);
        }
        if (in.eof()) break;
    }
}

// ── post-download steps ────────────────────────────────────────────

void download_english_lexicon(const fs::path& folder, FetchCtx& ctx) {
    fs::path target = folder / "lexicon.txt";
    std::error_code ec;
    fs::remove(target, ec);
    http_get_to_file(kLexiconEnUs, target, Checksum::sha256(kLexiconEnUsSha256), ctx);
}

void download_vocoder_if_needed(const SherpaModel& model, const fs::path& folder, FetchCtx& ctx) {
    std::string name, digest;
    if (model.model_type == "matcha" && model.id == "icefall-zh_en-zh-en") {
        name = "vocos-16khz-univ.onnx";
        digest = "b599142a1fb8ff03de3e84ac35ff537c619e56f4267a6fe894851a42844acf9e";
    } else if (model.model_type == "matcha") {
        name = "vocos-22khz-univ.onnx";
        digest = "0574a135aa1db2de6e181050db2ec528496cacd4a4701fc5d7faf9f9804c0081";
    } else if (model.model_type == "zipvoice") {
        name = "vocos_24khz.onnx";
        digest = "bcb3b970e384161c4d634f0bb9e999ff1c471b34c9bc0b1049a5014065ed3cc0";
    } else {
        return;
    }
    fs::path target = folder / "vocoder.onnx";
    std::error_code ec;
    fs::remove(target, ec);
    http_get_to_file(std::string(kVocoderRelease) + name, target, Checksum::sha256(digest), ctx);
}

// MMS voices live on Hugging Face, one file each. The tree API lists
// paths with either an LFS sha256 (big files) or a git-blob sha1.
void download_mms(const std::string& language, const fs::path& folder, FetchCtx& ctx) {
    std::string tree_url =
        std::string(kHub) + "/api/models/" + kMmsRepository + "/tree/" + kMmsRevision + "/" + language;
    std::string body = http_get_to_string(tree_url, ctx);
    std::string parse_error;
    JsonValue files = JsonValue::parse(body, &parse_error);
    if (!files.is_array()) fail("Could not list the MMS model files.");

    struct Wanted {
        std::string path;
        Checksum sum;
    };
    std::vector<Wanted> wanted;
    for (const std::string& name : {"model.onnx", "tokens.txt", "lexicon.txt"}) {
        std::string path = language + "/" + name;
        const JsonValue* file = nullptr;
        for (const JsonValue& f : files.items()) {
            if (f.get_string("path") == path) {
                file = &f;
                break;
            }
        }
        if (!file) {
            if (name == "lexicon.txt") continue; // optional
            fail("Could not list the MMS model files.");
        }
        double size = file->get_number("size");
        std::string oid = file->get_string("oid");
        Checksum sum;
        if (const JsonValue* lfs = file->find("lfs"); lfs && lfs->is_object()) {
            sum = Checksum::sha256(lfs->get_string("oid"));
        } else if (size > 0 && size <= static_cast<double>(kMaxGitBlobBytes)) {
            sum = Checksum::git_blob(oid, static_cast<long long>(size));
        } else {
            fail("Could not verify the MMS model files.");
        }
        if (!sum.valid()) fail("Could not verify the MMS model files.");
        wanted.push_back({path, sum});
    }
    for (const auto& w : wanted) {
        http_get_to_file(std::string(kHub) + "/" + kMmsRepository + "/resolve/" + kMmsRevision + "/" + w.path,
                         folder / fs::path(w.path).filename(), w.sum, ctx);
    }
}

// For MMS registry entries: the per-language HF directory the model's
// url points at ("…/resolve/main/<lang>"), when the id matches
// mms_<lang>. Mirrors SpeechModelStorage.mmsDirectory(for:).
std::optional<std::string> mms_directory(const SherpaModel& model) {
    if (model.model_type != "mms") return std::nullopt;
    const std::string prefix = std::string(kHub) + "/" + kMmsRepository + "/resolve/main/";
    if (model.url.rfind(prefix, 0) != 0) return std::nullopt;
    std::string lang = model.url.substr(prefix.size());
    if (lang.empty() || lang.find('/') != std::string::npos) return std::nullopt;
    if (model.id != "mms_" + lang) return std::nullopt;
    return lang;
}

fs::path find_model_folder(const fs::path& root) {
    if (dir_has_onnx(root)) return root;
    std::vector<fs::path> candidates;
    for (const auto& entry : dir_entries(root)) {
        std::error_code ec;
        if (fs::is_directory(entry, ec) && dir_has_onnx(entry)) candidates.push_back(entry);
    }
    if (candidates.size() == 1) return candidates[0];
    fail("The downloaded model is missing files required by Sherpa-ONNX.");
}

// Per-type required files. Each group needs at least one present,
// mirroring SpeechModelStorage.validate(_:in:).
void validate_model_files(const SherpaModel& model, const fs::path& folder) {
    std::vector<std::vector<std::string>> required;
    if (model.model_type == "vits") {
        if (!(model.uses_g2p_bundle() || dir_has_lexicon(folder)) && dir_has_onnx(folder)) {
            fail("This model needs a pronunciation dictionary that Dasher cannot install yet.");
        }
        required = {{"tokens.txt"}};
    } else if (model.model_type == "mms") {
        required = {{"model.onnx"}, {"tokens.txt"}};
    } else if (model.model_type == "kokoro" || model.model_type == "kitten") {
        // needs_bundle models (catalog v2) phonemize through the
        // voicegarden G2P bundle — their tarballs ship no lexicon.txt
        // (kokoro-en-v0_19 et al.). The lexicon is only required when
        // no bundle backs the model, mirroring the vits branch above.
        if (!model.uses_g2p_bundle() && !dir_has_lexicon(folder)) {
            fail("This model needs a pronunciation dictionary that Dasher cannot install yet.");
        }
        required = {{"model.onnx", "model.int8.onnx"}, {"tokens.txt"}, {"voices.bin"}};
    } else if (model.model_type == "supertonic") {
        required = {{"duration_predictor.onnx", "duration_predictor.int8.onnx"},
                    {"text_encoder.onnx", "text_encoder.int8.onnx"},
                    {"vector_estimator.onnx", "vector_estimator.int8.onnx"},
                    {"vocoder.onnx", "vocoder.int8.onnx"},
                    {"tts.json"},
                    {"unicode_indexer.bin"},
                    {"voice.bin"}};
    } else if (model.model_type == "matcha") {
        required = {{"acoustic-model.onnx", "model-steps-3.onnx", "model-steps-1000.onnx", "model.onnx"},
                    {"tokens.txt"},
                    {"lexicon.txt"},
                    {"vocoder.onnx"}};
    } else if (model.model_type == "zipvoice") {
        required = {{"encoder.onnx", "encoder.int8.onnx"},
                    {"decoder.onnx", "decoder.int8.onnx"},
                    {"tokens.txt"},
                    {"lexicon.txt"},
                    {"vocoder.onnx"},
                    {"test_wavs/leijun-1.wav"}};
    } else if (model.model_type == "pocket") {
        bool has_reference = false;
        fs::path wavs = folder / "test_wavs";
        for (const auto& p : dir_entries(wavs)) {
            if (p.extension() == ".wav" && file_nonempty(p)) has_reference = true;
        }
        for (const auto& p : dir_entries(folder)) {
            if (p.extension() == ".wav" && file_nonempty(p)) has_reference = true;
        }
        if (!has_reference) fail("The downloaded model is missing files required by Sherpa-ONNX.");
        required = {{"lm_flow.onnx", "lm_flow.int8.onnx"},
                    {"lm_main.onnx", "lm_main.int8.onnx"},
                    {"encoder.onnx", "encoder.int8.onnx"},
                    {"decoder.onnx", "decoder.int8.onnx"},
                    {"text_conditioner.onnx", "text_conditioner.int8.onnx"},
                    {"vocab.json"},
                    {"token_scores.json"}};
    } else {
        fail("The downloaded model is missing files required by Sherpa-ONNX.");
    }
    for (const auto& group : required) {
        bool ok = false;
        for (const auto& name : group) {
            // Allow nested paths (test_wavs/…) relative to the folder.
            if (dir_contains_nonempty_file(folder, name)) {
                ok = true;
                break;
            }
        }
        if (!ok) fail("The downloaded model is missing files required by Sherpa-ONNX.");
    }
}

std::string staging_name() {
    std::random_device rd;
    std::ostringstream id;
    id << ".staging-" << std::hex << rd() << rd();
    return id.str();
}

} // namespace

SherpaModelInstaller::SherpaModelInstaller(G2pProvisioner provision_g2p) : m_provision_g2p(std::move(provision_g2p)) {}

bool SherpaModelInstaller::install(const SherpaModel& model, const CancelFn& cancelled, const ProgressFn& progress,
                                   std::string& error) {
    fs::path staging;
    try {
        if (auto reason = SherpaStorage::download_unavailable_reason(model)) {
            fail(*reason);
        }

        fs::path models = SherpaStorage::models_dir();
        staging = models / staging_name();
        std::error_code ec;
        fs::create_directories(models);
        fs::remove_all(staging, ec);
        fs::create_directories(staging / "model");

        FetchCtx ctx{&cancelled, &progress, "Downloading model…"};

        fs::path model_folder;
        if (auto language = mms_directory(model)) {
            progress(0.0, "Downloading model…");
            download_mms(*language, staging / "model", ctx);
            model_folder = staging / "model";
        } else {
            std::string digest = SherpaStorage::archive_checksum_override(model.id);
            if (digest.empty()) digest = model.sha256;
            if (digest.empty()) fail("This model has no verified download that Dasher can install.");
            fs::path archive = staging / "model.tar.bz2";
            http_get_to_file(model.url, archive, Checksum::sha256(digest), ctx);
            progress(-1.0, "Extracting…");
            fs::path tar = staging / "model.tar";
            decompress_bz2(archive, tar, cancelled);
            extract_tar(tar, staging / "model", cancelled);
            model_folder = find_model_folder(staging / "model");

            // espeak-orphaned English voices get the pinned lexicon.
            static const char* const kLexiconTypes[] = {"vits", "kokoro", "kitten", "matcha", "zipvoice"};
            bool lexicon_type = false;
            for (const char* t : kLexiconTypes) {
                if (model.model_type == t) lexicon_type = true;
            }
            if (lexicon_type && !model.uses_g2p_bundle() && !dir_has_lexicon(model_folder)) {
                if (!model.is_english_only()) {
                    fail("This model needs a pronunciation dictionary that Dasher cannot install yet.");
                }
                progress(-1.0, "Downloading pronunciation dictionary…");
                download_english_lexicon(model_folder, ctx);
            }
            if (model.model_type == "kitten" && !dir_contains_nonempty_file(model_folder, "model.onnx")) {
                for (const char* alt : {"model.int8.onnx", "model.fp16.onnx"}) {
                    if (dir_contains_nonempty_file(model_folder, alt)) {
                        fs::rename(model_folder / alt, model_folder / "model.onnx");
                        break;
                    }
                }
            }
            progress(-1.0, "Downloading vocoder…");
            download_vocoder_if_needed(model, model_folder, ctx);
        }

        progress(-1.0, "Verifying…");
        validate_model_files(model, model_folder);

        if (model.uses_g2p_bundle()) {
            progress(-1.0, "Downloading pronunciation dictionary…");
            if (!m_provision_g2p || !m_provision_g2p(model.id)) {
                fail("Could not download or verify the pronunciation dictionary.");
            }
        }

        // Marker first, then move into place (replace any previous).
        {
            std::ofstream marker(model_folder / ".dasher-verified-model", std::ios::trunc);
            marker << model.id << "\n";
        }
        progress(-1.0, "Installing…");
        fs::path destination = models / model.id;
        fs::remove_all(destination, ec);
        fs::rename(model_folder, destination);
        fs::remove_all(staging, ec);
        progress(1.0, "Installed");
        return true;
    } catch (const InstallError& e) {
        std::error_code ec;
        if (!staging.empty()) fs::remove_all(staging, ec);
        error = e.message;
        return false;
    } catch (const std::exception&) {
        std::error_code ec;
        if (!staging.empty()) fs::remove_all(staging, ec);
        error = "Could not download or verify the speech model.";
        return false;
    }
}

} // namespace Dasher

#endif // HAS_TTS_DOWNLOAD
