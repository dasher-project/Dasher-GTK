#include "Output/ControlActions.h"

#include <algorithm>

// ── Parsing ────────────────────────────────────────────────────────────────

bool ControlActions::is_control_action(const std::string& name) {
    return name == "select" || name == "move" || name == "delete";
}

std::optional<std::pair<ControlDistance, ControlDirection>> ControlActions::parse(const std::string& dist,
                                                                                  const std::string& forward) {
    ControlDistance d;
    if (dist == "char")
        d = ControlDistance::Char;
    else if (dist == "word")
        d = ControlDistance::Word;
    else if (dist == "sentence")
        d = ControlDistance::Sentence;
    else if (dist == "paragraph")
        d = ControlDistance::Paragraph;
    else if (dist == "all")
        d = ControlDistance::All;
    else
        return std::nullopt;

    if (forward == "yes") return std::make_pair(d, ControlDirection::Forward);
    if (forward == "no") return std::make_pair(d, ControlDirection::Backward);
    return std::nullopt;
}

const char* ControlActions::distance_name(ControlDistance dist) {
    switch (dist) {
    case ControlDistance::Char:
        return "char";
    case ControlDistance::Word:
        return "word";
    case ControlDistance::Sentence:
        return "sentence";
    case ControlDistance::Paragraph:
        return "paragraph";
    case ControlDistance::All:
        return "all";
    }
    return "";
}

bool ControlActions::is_forward(ControlDirection dir) {
    return dir == ControlDirection::Forward;
}

// ── Keyboard (direct) mode: chord planning ─────────────────────────────────
//
// Mirrors Dasher-Windows' SelectActions.Keystroke / CaretKeystroke /
// DeleteKeystrokes table with Linux evdev codes (what ydotool injects):
//
//   select  char  Shift+Left/Right          move  char  Left/Right
//          word  Ctrl+Shift+Left/Right           word  Ctrl+Left/Right
//          para  Ctrl+Shift+Up/Down ⚠           para  Ctrl+Up/Down
//          all   Ctrl+A                         all   Ctrl+Home/End
//   delete  char  Backspace / Delete        sentence: no chord — degrades to
//          word  Ctrl+Backspace/Ctrl+Delete      the paragraph plan with
//          para  select-paragraph + Delete       sentence_fallback set
//          all   Ctrl+A + Delete
//
// ⚠ paragraph chords are paragraph-exact in paragraph-aware targets
// (GtkTextView-class widgets) but LINE-granular in line-class targets
// (browsers, plain entries) — the same documented failure envelope as
// Windows' Ctrl+Shift+Up/Down row in RFC 0021's platform table, accepted
// rather than papered over (GTK has no UIA/TextPattern equivalent in the
// injection path; AT-SPI text selection would be a project of its own).

std::vector<ControlActions::Chord> ControlActions::direct_plan(const std::string& action, ControlDistance dist,
                                                               ControlDirection dir, bool& sentence_fallback) {
    sentence_fallback = false;
    ControlDistance chord_dist = dist;
    if (dist == ControlDistance::Sentence) {
        // No universal Linux sentence keystroke. Windows degrades sentence
        // to paragraph in direct mode (logged + telemetry-tagged); we match
        // that exactly. Editor mode implements sentence natively.
        chord_dist = ControlDistance::Paragraph;
        sentence_fallback = true;
    }

    const bool fwd = is_forward(dir);
    const int horiz = fwd ? kKeyRight : kKeyLeft;
    const int vert = fwd ? kKeyDown : kKeyUp;

    if (action == "select") {
        switch (chord_dist) {
        case ControlDistance::Char:
            return {{/*mods=*/{kKeyLeftShift}, /*keys=*/{horiz}}};
        case ControlDistance::Word:
            return {{/*mods=*/{kKeyLeftCtrl, kKeyLeftShift}, /*keys=*/{horiz}}};
        case ControlDistance::Paragraph:
            return {{/*mods=*/{kKeyLeftCtrl, kKeyLeftShift}, /*keys=*/{vert}}};
        case ControlDistance::All:
            return {{/*mods=*/{kKeyLeftCtrl}, /*keys=*/{kKeyA}}};
        default:
            return {};
        }
    }

    if (action == "move") {
        switch (chord_dist) {
        case ControlDistance::Char:
            return {{/*mods=*/{}, /*keys=*/{horiz}}};
        case ControlDistance::Word:
            return {{/*mods=*/{kKeyLeftCtrl}, /*keys=*/{horiz}}};
        case ControlDistance::Paragraph:
            return {{/*mods=*/{kKeyLeftCtrl}, /*keys=*/{vert}}};
        case ControlDistance::All:
            // Ctrl+Home / Ctrl+End = document start/end (the bare Home/End
            // is line-granular in most targets).
            return {{/*mods=*/{kKeyLeftCtrl}, /*keys=*/{fwd ? kKeyEnd : kKeyHome}}};
        default:
            return {};
        }
    }

    if (action == "delete") {
        switch (chord_dist) {
        case ControlDistance::Char:
            // Backspace deletes behind the caret; Delete erases ahead.
            return {{/*mods=*/{}, /*keys=*/{fwd ? kKeyDelete : kKeyBackspace}}};
        case ControlDistance::Word:
            return {{/*mods=*/{kKeyLeftCtrl}, /*keys=*/{fwd ? kKeyDelete : kKeyBackspace}}};
        case ControlDistance::Paragraph:
            // No paragraph-delete keystroke anywhere: compose the select
            // chord, then the Delete key (two queued chords, Windows'
            // DeleteKeystrokes composition).
            return {{/*mods=*/{kKeyLeftCtrl, kKeyLeftShift}, /*keys=*/{vert}}, {/*mods=*/{}, /*keys=*/{kKeyDelete}}};
        case ControlDistance::All:
            return {{/*mods=*/{kKeyLeftCtrl}, /*keys=*/{kKeyA}}, {/*mods=*/{}, /*keys=*/{kKeyDelete}}};
        default:
            return {};
        }
    }

    return {};
}

std::string ControlActions::chord_sequence(const Chord& chord) {
    std::string seq;
    const auto press = [&seq](int code) {
        if (!seq.empty()) seq += ' ';
        seq += std::to_string(code);
        seq += ":1";
    };
    const auto release = [&seq](int code) {
        if (!seq.empty()) seq += ' ';
        seq += std::to_string(code);
        seq += ":0";
    };
    // Presses: modifiers first, then principals; releases exactly reversed.
    // One ydotool invocation carries the whole sequence, so the target sees
    // an atomic chord (the RFC 0021 injection contract: events within one
    // command are non-interleaved).
    for (int mod : chord.mods)
        press(mod);
    for (int key : chord.keys)
        press(key);
    for (auto it = chord.keys.rbegin(); it != chord.keys.rend(); ++it)
        release(*it);
    for (auto it = chord.mods.rbegin(); it != chord.mods.rend(); ++it)
        release(*it);
    return seq;
}

// ── Editor mode: native TextIter operations ────────────────────────────────

namespace {
// The caret position WITHOUT wrapping the insert mark: gtkmm's get_insert()
// wraps a GtkTextMark and that wrap needs full GTK initialisation (it is
// what breaks headless use — unit tests and any off-display call site).
// property_cursor_position → get_iter_at_offset is the same read MainWindow's
// editor-sync handlers use, needs no wrapper tables, and is one C property
// read + offset conversion.
Gtk::TextIter cursor_iter(const Glib::RefPtr<Gtk::TextBuffer>& buffer) {
    return buffer->get_iter_at_offset(buffer->property_cursor_position().get_value());
}
} // namespace

Gtk::TextIter ControlActions::editor_boundary(const Glib::RefPtr<Gtk::TextBuffer>& buffer, const Gtk::TextIter& from,
                                              ControlDistance dist, ControlDirection dir) {
    Gtk::TextIter it = from;
    const bool fwd = is_forward(dir);
    switch (dist) {
    case ControlDistance::Char:
        // Native forward_char/backward_char step one Unicode character and
        // never split a multi-byte sequence.
        if (fwd)
            it.forward_char();
        else
            it.backward_char();
        break;

    case ControlDistance::Word:
        if (fwd) {
            // GtkTextView's own Ctrl+Right algorithm (move_words): to the
            // end of the current word, then advance while not at a word
            // start — lands on the NEXT word's start, separators skipped.
            if (it.forward_word_end()) {
                while (!it.starts_word()) {
                    if (!it.forward_char()) break; // buffer end is a valid stop
                }
            }
        } else {
            it.backward_word_start();
        }
        break;

    case ControlDistance::Sentence:
        // Native Pango sentence boundaries — the one granularity GTK
        // measures exactly where Windows had to fall back (editor mode is
        // the precise path for sentences by design).
        if (fwd)
            it.forward_sentence_end();
        else
            it.backward_sentence_start();
        break;

    case ControlDistance::Paragraph:
        // A GtkTextBuffer paragraph IS a line (GTK normalises every line
        // separator on insert, so there is exactly one coordinate system —
        // no CRLF edge cases here).
        if (fwd) {
            // forward_to_line_end reaches the CURRENT line's end from
            // mid-line and, from a line end, chains to the NEXT line's end
            // (pinned by unit test) — so repeated paragraph moves step
            // paragraph by paragraph, empty lines included, matching
            // Dasher-Windows' ParagraphBound chaining.
            if (!it.forward_to_line_end()) {
                it = buffer->end();
            }
        } else {
            // Mid-line → this line's start; already at a line start → the
            // previous line's start (backward_line from starts_line is
            // unambiguous). Same chain behaviour, backwards.
            if (it.starts_line()) {
                if (!it.backward_line()) it = buffer->begin();
            } else {
                it.set_line_offset(0);
            }
        }
        break;

    case ControlDistance::All:
        it = fwd ? buffer->end() : buffer->begin();
        break;
    }
    return it;
}

bool ControlActions::editor_move(Glib::RefPtr<Gtk::TextBuffer> buffer, ControlDistance dist, ControlDirection dir) {
    Gtk::TextIter from = cursor_iter(buffer);
    const Gtk::TextIter target = editor_boundary(buffer, from, dist, dir);
    if (target == from) return false;
    // place_cursor moves the insert and drops any selection; the pane's
    // cursor-changed handler re-anchors the engine (RFC 0019 clause 3 —
    // the same path arrow-key caret moves take).
    buffer->place_cursor(target);
    return true;
}

void ControlActions::editor_select(Glib::RefPtr<Gtk::TextBuffer> buffer, ControlDistance dist, ControlDirection dir) {
    if (dist == ControlDistance::All) {
        // Select-all regardless of direction (the RFC table maps All to
        // SelectAll, not shift-to-boundary). Active end (cursor) at the
        // document end: a following forward select extends from there.
        buffer->select_range(buffer->end(), buffer->begin());
        return;
    }

    Gtk::TextIter sel_start, sel_end;
    const bool had_selection = buffer->get_selection_bounds(sel_start, sel_end);
    Gtk::TextIter insert = cursor_iter(buffer);

    // Anchor/active-end machine. With a selection: extending backward
    // anchors at the selection's END and extends its START (the opposite
    // end stays put); extending forward anchors at the START and extends
    // the END — so any sequence of selects only GROWS the range. Cold
    // start (no selection): both ends at the caret, so "forward selects
    // extend forward from the caret".
    Gtk::TextIter anchor, active;
    if (had_selection) {
        anchor = is_forward(dir) ? sel_start : sel_end;
        active = is_forward(dir) ? sel_end : sel_start;
    } else {
        anchor = active = insert;
    }

    const Gtk::TextIter target = editor_boundary(buffer, active, dist, dir);
    // select_range(cursor, bound): the CURSOR (active end) goes to the
    // measured target so a chained same-direction select keeps extending;
    // the selection bound (anchor) holds the opposite end.
    buffer->select_range(target, anchor);
}

int ControlActions::editor_delete(Glib::RefPtr<Gtk::TextBuffer> buffer, ControlDistance dist, ControlDirection dir) {
    if (dist == ControlDistance::All) {
        const int chars = buffer->get_char_count();
        if (chars == 0) return 0;
        buffer->erase(buffer->begin(), buffer->end());
        return chars;
    }

    Gtk::TextIter caret = cursor_iter(buffer);
    const Gtk::TextIter boundary = editor_boundary(buffer, caret, dist, dir);
    Gtk::TextIter start = caret, end = boundary;
    if (start.get_offset() > end.get_offset()) std::swap(start, end);
    if (start == end) return 0; // nothing to delete at a document edge

    const int bytes = end.get_offset() - start.get_offset(); // codepoints
    buffer->erase(start, end);
    // The caret lands at the erase start (both directions) — where the
    // deleted text used to begin. The changed signal feeds the engine
    // re-seed (user-edit path, RFC 0019 clause 4).
    return bytes;
}
