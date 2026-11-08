// Unit tests for the RFC 0021 control-action semantics layer (GTK): the
// pure attribute parser, the direct-mode chord planner, sequence rendering
// and validation, and the editor-mode operations against REAL headless
// GtkTextBuffers (a TextBuffer is a data model — no display needed, unlike
// the prefs_rebuild_selftest which maps windows under xvfb).
//
// The buffer cases pin the NATIVE TextIter boundary behaviour the
// implementation leans on (RFC 0021's philosophy: frontends measure with
// platform primitives, never a hand-rolled copy of the engine's
// separators) — most importantly the chaining contracts for paragraph
// moves, which the Windows implementation had to fix twice.
//
// Note: the first TextBuffer::create() without gtk_init() logs a harmless
// GLib-GObject-CRITICAL (quark initialisation); it does not affect any
// behaviour under test.
#include "Output/ControlActions.h"

#include <doctest/doctest.h>

using CD = ControlDistance;
using CDir = ControlDirection;

namespace {
struct Buf {
    Glib::RefPtr<Gtk::TextBuffer> buf = Gtk::TextBuffer::create();
    explicit Buf(const std::string& text) { buf->set_text(text); }
    int cursor() const { return buf->property_cursor_position().get_value(); }
    void cursor(int offset) { buf->place_cursor(buf->get_iter_at_offset(offset)); }
    std::string text() const { return buf->get_text(); }
    std::pair<int, int> selection() {
        Gtk::TextIter s, e;
        if (!buf->get_selection_bounds(s, e)) return {-1, -1};
        return {s.get_offset(), e.get_offset()};
    }
};
} // namespace

// ── Parsing ────────────────────────────────────────────────────────────────

TEST_CASE("parse accepts the RFC 0021 attribute vocabulary") {
    for (const char* d : {"char", "word", "sentence", "paragraph", "all"}) {
        for (const char* f : {"yes", "no"}) {
            const auto parsed = ControlActions::parse(d, f);
            REQUIRE(parsed.has_value());
            CHECK(ControlActions::distance_name(parsed->first) == std::string(d));
            CHECK(ControlActions::is_forward(parsed->second) == (std::string(f) == "yes"));
        }
    }
}

TEST_CASE("parse rejects unknown or missing values instead of guessing") {
    // The engine validates NO attributes for custom actions — a typo'd dist
    // must parse to nothing (the dispatcher logs and no-ops, never crashes).
    CHECK_FALSE(ControlActions::parse("letter", "yes").has_value());
    CHECK_FALSE(ControlActions::parse("char", "maybe").has_value());
    CHECK_FALSE(ControlActions::parse("", "yes").has_value());
    CHECK_FALSE(ControlActions::parse("char", "").has_value());
    CHECK_FALSE(ControlActions::parse("Char", "Yes").has_value()); // case-sensitive, verbatim
}

TEST_CASE("is_control_action names exactly the three registered actions") {
    CHECK(ControlActions::is_control_action("select"));
    CHECK(ControlActions::is_control_action("move"));
    CHECK(ControlActions::is_control_action("delete"));
    CHECK_FALSE(ControlActions::is_control_action("stop"));
    CHECK_FALSE(ControlActions::is_control_action(""));
}

// ── Direct-mode chord planning ─────────────────────────────────────────────

TEST_CASE("select chords mirror the RFC 0021 platform table") {
    bool fb = true;
    auto seq = [](std::vector<ControlActions::Chord> plan) {
        REQUIRE(plan.size() == 1);
        return ControlActions::chord_sequence(plan.front());
    };
    using CA = ControlActions;
    CHECK(seq(CA::direct_plan("select", CD::Char, CDir::Forward, fb)) == "42:1 106:1 106:0 42:0");
    CHECK(seq(CA::direct_plan("select", CD::Char, CDir::Backward, fb)) == "42:1 105:1 105:0 42:0");
    CHECK(seq(CA::direct_plan("select", CD::Word, CDir::Forward, fb)) == "29:1 42:1 106:1 106:0 42:0 29:0");
    CHECK(seq(CA::direct_plan("select", CD::Paragraph, CDir::Backward, fb)) == "29:1 42:1 103:1 103:0 42:0 29:0");
    CHECK(seq(CA::direct_plan("select", CD::All, CDir::Forward, fb)) == "29:1 30:1 30:0 29:0"); // Ctrl+A
    CHECK_FALSE(fb); // none of the above degrade
}

TEST_CASE("move chords carry no Shift (movement, not selection)") {
    bool fb = true;
    using CA = ControlActions;
    auto plan = CA::direct_plan("move", CD::Word, CDir::Backward, fb);
    REQUIRE(plan.size() == 1);
    CHECK(ControlActions::chord_sequence(plan.front()) == "29:1 105:1 105:0 29:0"); // Ctrl+Left

    plan = CA::direct_plan("move", CD::Char, CDir::Forward, fb);
    REQUIRE(plan.size() == 1);
    CHECK(ControlActions::chord_sequence(plan.front()) == "106:1 106:0"); // bare Right

    // All = document start/end via Ctrl+Home/Ctrl+End (bare Home/End is
    // line-granular in most targets).
    plan = CA::direct_plan("move", CD::All, CDir::Backward, fb);
    REQUIRE(plan.size() == 1);
    CHECK(ControlActions::chord_sequence(plan.front()) == "29:1 102:1 102:0 29:0");
    plan = CA::direct_plan("move", CD::All, CDir::Forward, fb);
    REQUIRE(plan.size() == 1);
    CHECK(ControlActions::chord_sequence(plan.front()) == "29:1 107:1 107:0 29:0");
}

TEST_CASE("delete chords: single key for char/word, composition for paragraph/all") {
    bool fb = true;
    using CA = ControlActions;
    // backward char = Backspace; forward char = Delete
    auto plan = CA::direct_plan("delete", CD::Char, CDir::Backward, fb);
    REQUIRE(plan.size() == 1);
    CHECK(ControlActions::chord_sequence(plan.front()) == "14:1 14:0");
    plan = CA::direct_plan("delete", CD::Char, CDir::Forward, fb);
    REQUIRE(plan.size() == 1);
    CHECK(ControlActions::chord_sequence(plan.front()) == "111:1 111:0");
    // word: Ctrl+Backspace / Ctrl+Delete
    plan = CA::direct_plan("delete", CD::Word, CDir::Backward, fb);
    REQUIRE(plan.size() == 1);
    CHECK(ControlActions::chord_sequence(plan.front()) == "29:1 14:1 14:0 29:0");
    // paragraph: select-paragraph then Delete (two queued chords, Windows'
    // DeleteKeystrokes composition)
    plan = CA::direct_plan("delete", CD::Paragraph, CDir::Forward, fb);
    REQUIRE(plan.size() == 2);
    CHECK(ControlActions::chord_sequence(plan[0]) == "29:1 42:1 108:1 108:0 42:0 29:0");
    CHECK(ControlActions::chord_sequence(plan[1]) == "111:1 111:0");
    // all: Ctrl+A then Delete
    plan = CA::direct_plan("delete", CD::All, CDir::Backward, fb);
    REQUIRE(plan.size() == 2);
    CHECK(ControlActions::chord_sequence(plan[0]) == "29:1 30:1 30:0 29:0");
    CHECK(ControlActions::chord_sequence(plan[1]) == "111:1 111:0");
}

TEST_CASE("sentence degrades to the paragraph chord with the fallback flag") {
    // Direct mode has no sentence keystroke (the RFC 0021 honesty note):
    // every action degrades to paragraph and REPORTS it, so telemetry can
    // tag fallback=paragraph and field data never reads sentence-accurate.
    for (const char* action : {"select", "move", "delete"}) {
        bool fb = false;
        const auto plan = ControlActions::direct_plan(action, CD::Sentence, CDir::Forward, fb);
        CHECK(fb);
        bool fb_para = false;
        const auto para_plan = ControlActions::direct_plan(action, CD::Paragraph, CDir::Forward, fb_para);
        CHECK(plan.size() == para_plan.size());
        if (plan.size() == para_plan.size() && !plan.empty()) {
            CHECK(ControlActions::chord_sequence(plan.front()) == ControlActions::chord_sequence(para_plan.front()));
        }
    }
}

TEST_CASE("unknown actions plan nothing (no-op, never a wrong injection)") {
    bool fb = true;
    CHECK(ControlActions::direct_plan("explode", CD::Char, CDir::Forward, fb).empty());
    CHECK_FALSE(fb);
}

TEST_CASE("chord_sequence releases in reverse press order") {
    const ControlActions::Chord chord{{29, 42}, {106}};
    CHECK(ControlActions::chord_sequence(chord) == "29:1 42:1 106:1 106:0 42:0 29:0");
}

// ── Editor mode: native TextIter measurement ───────────────────────────────

TEST_CASE("editor move: char steps one codepoint, never splitting a multibyte char") {
    // h é l l o space 😀 space e n d — the emoji is 4 UTF-8 bytes but ONE
    // buffer position (GTK counts codepoints).
    Buf b("h\xC3\xA9llo \xF0\x9F\x98\x80 end"); // "héllo 😀 end"
    b.cursor(6);                                // just before the emoji
    CHECK(ControlActions::editor_move(b.buf, CD::Char, CDir::Forward));
    CHECK(b.cursor() == 7); // skipped the whole emoji, not one byte of it
    CHECK(ControlActions::editor_move(b.buf, CD::Char, CDir::Backward));
    CHECK(b.cursor() == 6);
    // Document edges: no move, no crash.
    b.cursor(0);
    CHECK_FALSE(ControlActions::editor_move(b.buf, CD::Char, CDir::Backward));
    CHECK(b.cursor() == 0);
    b.cursor(static_cast<int>(b.text().size()));
    CHECK_FALSE(ControlActions::editor_move(b.buf, CD::Char, CDir::Forward));
}

TEST_CASE("editor move: word steps between word starts (GtkTextView Ctrl+arrow semantics)") {
    Buf b("one two three four");
    b.cursor(0);
    CHECK(ControlActions::editor_move(b.buf, CD::Word, CDir::Forward));
    CHECK(b.cursor() == 4); // start of "two" (separator skipped)
    CHECK(ControlActions::editor_move(b.buf, CD::Word, CDir::Forward));
    CHECK(b.cursor() == 8); // start of "three"
    CHECK(ControlActions::editor_move(b.buf, CD::Word, CDir::Backward));
    CHECK(b.cursor() == 4); // back to start of "two"
    // Mid-word backward lands on the CURRENT word's start.
    b.cursor(6);
    CHECK(ControlActions::editor_move(b.buf, CD::Word, CDir::Backward));
    CHECK(b.cursor() == 4);
}

TEST_CASE("editor move: sentence uses native Pango boundaries and chains") {
    const std::string text = "First one. Second one! Third one? Final tail";
    const int second_start = static_cast<int>(text.find("Second")); // 11
    Buf b(text);
    b.cursor(0);
    CHECK(ControlActions::editor_move(b.buf, CD::Sentence, CDir::Forward));
    const int first_end = b.cursor();
    // Forward stops just past the terminator (Pango's sentence-END
    // boundary); backward stops at the sentence-START boundary (after the
    // trailing space) — asymmetric but consistently so, which is what the
    // chaining contract needs.
    CHECK(first_end == 10);
    CHECK(ControlActions::editor_move(b.buf, CD::Sentence, CDir::Forward));
    CHECK(b.cursor() > first_end); // chains — repeated moves keep stepping
    CHECK(ControlActions::editor_move(b.buf, CD::Sentence, CDir::Backward));
    CHECK(b.cursor() == second_start); // back at the middle sentence's start
    CHECK(ControlActions::editor_move(b.buf, CD::Sentence, CDir::Backward));
    CHECK(b.cursor() == 0); // and on to the document start
}

TEST_CASE("editor move: paragraph chains through empty lines in both directions") {
    Buf b("para one\n\npara two\npara three");
    //          ^8 ^9        ^18
    b.cursor(3); // mid "para one"
    CHECK(ControlActions::editor_move(b.buf, CD::Paragraph, CDir::Forward));
    CHECK(b.cursor() == 8); // end of line 0 (the separator position)
    // Chaining: from a line end, the next forward reaches the NEXT line's
    // end — the empty line's, then line 2's (the Windows ParagraphBound
    // behaviour, natively).
    CHECK(ControlActions::editor_move(b.buf, CD::Paragraph, CDir::Forward));
    CHECK(b.cursor() == 9);
    CHECK(ControlActions::editor_move(b.buf, CD::Paragraph, CDir::Forward));
    CHECK(b.cursor() == 18);
    // Backward: mid-line → this line's start; at a line start → the
    // previous line's start (the empty line's start, then line 0's).
    b.cursor(15);
    CHECK(ControlActions::editor_move(b.buf, CD::Paragraph, CDir::Backward));
    CHECK(b.cursor() == 10);
    CHECK(ControlActions::editor_move(b.buf, CD::Paragraph, CDir::Backward));
    CHECK(b.cursor() == 9);
    CHECK(ControlActions::editor_move(b.buf, CD::Paragraph, CDir::Backward));
    CHECK(b.cursor() == 0);
    CHECK_FALSE(ControlActions::editor_move(b.buf, CD::Paragraph, CDir::Backward)); // buffer start
}

TEST_CASE("editor move: all jumps to document start/end") {
    Buf b("some text here");
    b.cursor(5);
    CHECK(ControlActions::editor_move(b.buf, CD::All, CDir::Forward));
    CHECK(b.cursor() == 14);
    CHECK(ControlActions::editor_move(b.buf, CD::All, CDir::Backward));
    CHECK(b.cursor() == 0);
}

TEST_CASE("editor select: cold start extends forward from the caret") {
    Buf b("abcdef");
    b.cursor(3);
    ControlActions::editor_select(b.buf, CD::Char, CDir::Forward);
    CHECK(b.selection() == std::pair<int, int>{3, 4});
    CHECK(b.cursor() == 4); // active end (cursor) on the measured target
    ControlActions::editor_select(b.buf, CD::Char, CDir::Backward);
    // Grow-only anchor machine: backward anchors at the selection END (4)
    // and extends its start past the caret — the selection grows rather
    // than collapsing (the deliberate AAC divergence, RFC 0021 platform
    // table).
    CHECK(b.selection() == std::pair<int, int>{2, 4});
}

TEST_CASE("editor select: chained forward selects grow monotonically") {
    Buf b("one two three four");
    b.cursor(0);
    ControlActions::editor_select(b.buf, CD::Word, CDir::Forward);
    CHECK(b.selection() == std::pair<int, int>{0, 4});
    ControlActions::editor_select(b.buf, CD::Word, CDir::Forward);
    CHECK(b.selection() == std::pair<int, int>{0, 8});
    // Direction flip on a mid-document selection: anchor moves to the far
    // end, the near end extends BACKWARD — the selection GROWS rather than
    // collapsing (the deliberate AAC divergence from native Shift+Arrow).
    b.cursor(4);
    ControlActions::editor_select(b.buf, CD::Word, CDir::Forward);
    CHECK(b.selection() == std::pair<int, int>{4, 8});
    ControlActions::editor_select(b.buf, CD::Word, CDir::Backward);
    CHECK(b.selection() == std::pair<int, int>{0, 8});
}

TEST_CASE("editor select: all selects the whole buffer regardless of direction") {
    Buf b("hello world");
    b.cursor(5);
    ControlActions::editor_select(b.buf, CD::All, CDir::Backward);
    CHECK(b.selection() == std::pair<int, int>{0, 11});
}

TEST_CASE("editor delete: measured from the caret, both directions") {
    Buf b("one two three");
    b.cursor(8); // start of "three"
    // Backward word: removes "two " (to the previous word start).
    CHECK(ControlActions::editor_delete(b.buf, CD::Word, CDir::Backward) == 4);
    CHECK(b.text() == "one three");
    CHECK(b.cursor() == 4);
    // Forward char: removes ONE character ahead (the t of "three").
    CHECK(ControlActions::editor_delete(b.buf, CD::Char, CDir::Forward) == 1);
    CHECK(b.text() == "one hree");
}

TEST_CASE("editor delete: paragraph erases to the measured line boundary") {
    Buf b("para one\npara two");
    b.cursor(12); // inside line 1 ("para two"), after "par"
    // Backward paragraph: measured to the LINE start (9), not the
    // buffer's — erases exactly "par".
    CHECK(ControlActions::editor_delete(b.buf, CD::Paragraph, CDir::Backward) == 3);
    CHECK(b.text() == "para one\na two");
}

TEST_CASE("editor delete: all clears the document; edges no-op") {
    Buf b("text");
    b.cursor(2);
    CHECK(ControlActions::editor_delete(b.buf, CD::All, CDir::Backward) == 4);
    CHECK(b.text().empty());
    CHECK(b.cursor() == 0);
    CHECK(ControlActions::editor_delete(b.buf, CD::Char, CDir::Backward) == 0); // no-op at the edge
    CHECK(ControlActions::editor_delete(b.buf, CD::Word, CDir::Forward) == 0);
}

TEST_CASE("editor delete: multibyte characters erase as ONE unit") {
    Buf b("a\xC3\xA9"
          "b");  // "aéb" — é is 2 UTF-8 bytes, 1 codepoint
    b.cursor(2); // after é
    CHECK(ControlActions::editor_delete(b.buf, CD::Char, CDir::Backward) == 1);
    CHECK(b.text() == "ab");
}
