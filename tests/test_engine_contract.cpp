// Engine-contract tests against the real DasherCore via DasherBridge —
// GTK's counterpart of Dasher-Windows' EngineCApiTests (PR #37). They pin
// the dasher.h behaviours this frontend depends on, so engine regressions
// fail here rather than surfacing as UI bugs:
//
//   - dasher_create works on a FRESH user dir (DasherCore #59 regression:
//     the FileUtils glob regex choked on absolute paths and creation failed)
//   - probe-then-fetch permitted values (DasherCore #58 regression: the
//     probe returned 0 and every picker rendered blank)
//   - the text-size callback caches per label+size (DasherCore #56/v0.2.4:
//     2,520 callbacks per window on Windows before the cache contract was
//     honoured — steady-state frames must be callback-free)
//   - frame-delta clamping (this repo's jitter invariant, 1..50 ms)
//
// The prefs_rebuild_selftest binary already links the full app + engine; to
// keep this suite self-contained it links DasherBridge + the engine the same
// way and skips vacuously only when the Data tree is missing (CI always has
// it; see tests/CMakeLists.txt WORKING_DIRECTORY).

// On Windows, doctest's implementation drags in <windows.h>, whose macros
// (IN, OUT, ...) collide with cairomm's Operator enum and friends. Parse the
// GTK/Cairo headers first so they compile cleanly, then bring in doctest —
// same ordering as test_dwell_click_handler.cpp.
#include <glibmm/init.h>
#include "Engine/DasherBridge.h"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <fstream>
#include <thread>

namespace {

// Locate DasherCore/Data the way the app does at runtime: tests run with a
// WORKING_DIRECTORY that contains Data/ (see tests/CMakeLists.txt).
std::string find_data_dir() {
    // Two layouts carry the alphabet XMLs: the source submodule keeps them
    // under DasherCore/Data/alphabets/, and the build tree flattens
    // DasherCore/Data into build/Dasher/Data with alphabet XMLs at the top
    // level. Either shape serves; the engine scans recursively.
    const char* candidates[] = {"Data",           "Data/alphabets",  "Dasher/Data",
                                "../Dasher/Data", "DasherCore/Data", "DasherCore/Data/alphabets"};
    for (const char* candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_directory(candidate, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(candidate, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("alphabet.", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".xml") == 0) {
                // Return the Data root, not the alphabets subdir.
                std::string dir(candidate);
                if (dir.size() > 10 && dir.compare(dir.size() - 10, 10, "/alphabets") == 0) {
                    dir.resize(dir.size() - 10);
                }
                return dir;
            }
        }
    }
    return "";
}

std::string make_fresh_user_dir() {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path() /
               ("dasher-gtk-contract-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(dir); // deliberately empty: fresh user
    return dir.string();
}

} // namespace

TEST_CASE("frame delta clamp: the jitter invariant") {
    // Real smooth frames pass through untouched.
    CHECK(DasherBridge::clamp_frame_delta_ms(16) == 16);
    CHECK(DasherBridge::clamp_frame_delta_ms(17) == 17);
    // A stalled main loop (drag, modal, load) must cost the engine exactly
    // one bounded tick, never a multi-second jump.
    CHECK(DasherBridge::clamp_frame_delta_ms(5'000) == 50);
    CHECK(DasherBridge::clamp_frame_delta_ms(1'000'000) == 50);
    // Zero/negative deltas (clock oddities) still advance the engine.
    CHECK(DasherBridge::clamp_frame_delta_ms(0) == 1);
    CHECK(DasherBridge::clamp_frame_delta_ms(-7) == 1);
}

TEST_CASE("engine timeline is clamped across a real stall") {
    DasherBridge bridge(find_data_dir(), make_fresh_user_dir());
    const int64_t t0 = bridge.get_current_time_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // simulate a stall
    const int64_t t1 = bridge.get_current_time_ms();
    // 300 ms of wall time must compress to at most one 50 ms tick (+ the
    // first-call seed). Without the clamp this would be ~300.
    CHECK(t1 - t0 <= 100);
    CHECK(t1 - t0 >= 1);
}

TEST_CASE("dasher_create succeeds on a fresh user directory") {
    // DasherCore #59 regression: creation threw regex_error on brand-new
    // user dirs. If the data tree is missing here, fail — CI always has it.
    const std::string data = find_data_dir();
    REQUIRE_FALSE(data.empty());

    DasherBridge bridge(data, make_fresh_user_dir());
    bridge.set_screen_size(800, 600);
    CHECK(bridge.get_alphabet_count() > 0);
    CHECK_FALSE(bridge.get_alphabet_id().empty());
}

TEST_CASE("probe-then-fetch permitted values enumerate") {
    // DasherCore #58 regression: the count-only probe returned 0 before
    // querying, so every alphabet/palette picker rendered blank.
    const std::string data = find_data_dir();
    REQUIRE_FALSE(data.empty());

    DasherBridge bridge(data, make_fresh_user_dir());
    bridge.set_screen_size(800, 600);

    const int key = bridge.find_parameter_key("SP_ALPHABET_ID");
    REQUIRE(key >= 0);
    const std::vector<std::string> values = bridge.get_parameter_string_values(key);
    CHECK(values.size() > 100); // WorldAlphabets ships hundreds

    // The engine's current alphabet must be among the permitted values, or
    // the synced dropdown can't select it.
    const std::string current = bridge.get_string_parameter(key);
    CHECK(std::find(values.begin(), values.end(), current) != values.end());
}

TEST_CASE("training path resolves into the user dir at the engine layout") {
    // DasherCore#85/#86 contract the Preferences training UI depends on:
    // after realize, the engine reports the ONE file adaptive learning
    // appends to — inside this bridge's user dir, at the ROOT (not a
    // training/ subdirectory: the pre-#83 UIs derived paths the engine
    // never read or wrote, so exports missed all adaptive learning).
    const std::string data = find_data_dir();
    REQUIRE_FALSE(data.empty());
    REQUIRE(DasherBridge::capi_version() >= 1); // this build has the user-dir scan

    const std::string user = make_fresh_user_dir();
    DasherBridge bridge(data, user);
    CHECK(bridge.get_training_path().empty()); // unrealized: defined as empty
    bridge.set_screen_size(800, 600);

    const std::string path = bridge.get_training_path();
    CAPTURE(path);
    REQUIRE_FALSE(path.empty());
    const auto abs = std::filesystem::absolute(std::filesystem::path(path)).string();
    // Trailing separator: a sibling dir sharing a name prefix must not pass.
    const std::string user_prefix = std::filesystem::absolute(std::filesystem::path(user)).string() + "/";
    CHECK(abs.rfind(user_prefix, 0) == 0);
    const std::string name = std::filesystem::path(path).filename().string();
    CHECK(name.rfind("training_", 0) == 0);
    CHECK(name.compare(name.size() - 4, 4, ".txt") == 0);
    // Root layout: no path component may be a "training" directory.
    for (const auto& part : std::filesystem::path(path).parent_path())
        CHECK(part != "training");
}

TEST_CASE("editor contract: seed lands the caret on the byte offset; offset re-anchors; reset clears") {
    // RFC 0019 via the GTK bridge surface, mirroring Dasher-Windows#56's
    // engine-integration test. Clause 2 — seed text + caret (UTF-8 BYTES);
    // clause 3 — set_offset re-anchors; clause 5 — reset (New) clears buffer
    // AND offset.
    const std::string data = find_data_dir();
    REQUIRE_FALSE(data.empty());

    DasherBridge bridge(data, make_fresh_user_dir());
    bridge.set_screen_size(800, 600);

    // "héllo wörld": é/ö are 1 codepoint each but 2 UTF-8 bytes. GtkTextBuffer
    // offsets count codepoints; the engine counts bytes — the exact boundary
    // byte_offset_from_codepoints exists to cross.
    const std::string text = "héllo wörld";
    // codepoint 6 (after "héllo ") is byte 7: h=0 é=1,2 l=3 l=4 o=5 space=6.
    REQUIRE(DasherBridge::byte_offset_from_codepoints(text, 6) == 7);
    REQUIRE(bridge.seed_buffer(text, 7) == 0);
    CHECK(bridge.get_offset() == 7);

    // Pure caret move (clause 3): codepoint 7 (after "w", i.e. on the 2-byte
    // ö) is byte 8: h=0 é=1,2 l=3 l=4 o=5 space=6 w=7 ö=8,9.
    const int mid = DasherBridge::byte_offset_from_codepoints(text, 7);
    REQUIRE(mid == 8);
    REQUIRE(bridge.set_offset(mid) == 0);
    CHECK(bridge.get_offset() == 8);

    // New (clause 5): buffer and offset both reset.
    bridge.reset();
    CHECK(bridge.get_output_text().empty());
    CHECK(bridge.get_offset() == 0);
}

TEST_CASE("import training text reports success on a live engine") {
    const std::string data = find_data_dir();
    REQUIRE_FALSE(data.empty());

    DasherBridge bridge(data, make_fresh_user_dir());
    // The real measurement callback needs a Pango context (not initialised
    // in this bare test binary — same reason as the cached-label test).
    bridge.set_text_size_callback_for_tests([](const std::string& text, int font_size, int* w, int* h) {
        *w = static_cast<int>(text.size()) * font_size / 2;
        *h = font_size;
        return 0;
    });
    bridge.set_screen_size(800, 600);
    // Import trains the LIVE model only (persistence is the caller's job) —
    // the contract here is the rc plus "the engine stays healthy and keeps
    // rendering", not an observable model change (the load-at-startup
    // behaviour is pinned engine-side in DasherCore's training tests).
    CHECK(bridge.import_training_text("hello world this is a test") == 0);
    CHECK(bridge.import_training_text("") == 0);
    for (int i = 0; i < 3; i++)
        bridge.frame(i * 16);
    CHECK_FALSE(bridge.has_engine_error());
}

TEST_CASE("text-size callback is cached per label and size") {
    // DasherCore #56/v0.2.4: steady-state frames must issue zero measurement
    // callbacks (Windows measured 2,520 per window before the contract was
    // honoured). Render identical frames and count.
    const std::string data = find_data_dir();
    REQUIRE_FALSE(data.empty());

    DasherBridge bridge(data, make_fresh_user_dir());

    // One static pair-counter for the whole test: the cache contract is
    // that no (label, size) pair is measured twice - the tree may grow and
    // present new labels (measured once each), so total counts are not
    // stable across platforms.
    using Pair = std::pair<std::string, int>;
    static std::map<Pair, int> pair_counts;
    pair_counts.clear();
    bridge.set_text_size_callback_for_tests([](const std::string& text, int font_size, int* w, int* h) {
        pair_counts[{text, font_size}]++;
        *w = static_cast<int>(text.size()) * font_size / 2;
        *h = font_size;
        return 0;
    });
    bridge.set_screen_size(800, 600);

    // Warm-up + steady state: new labels may appear as the tree grows
    // (measured once each); no pair may ever be measured twice.
    for (int i = 0; i < 110; i++)
        bridge.frame(i * 16);
    int repeats = 0;
    for (const auto& [pair, n] : pair_counts) {
        if (n > 1) repeats++;
    }
    CHECK(repeats == 0);
    CHECK(pair_counts.size() > 10); // the cache engaged on a real tree
}
