#pragma once

// RFC 0021 control-mode custom actions — select / move / delete (GTK).
//
// The engine grows the control tree and passes each node's XML attributes
// (forward = "yes"|"no", dist = "char"|"word"|"sentence"|"paragraph"|"all")
// VERBATIM to the frontend's registered handler; it validates nothing and
// executes nothing (DasherCore docs/CUSTOM_ACTIONS.md). This module is the
// GTK semantics layer, split the way the reference implementations split it
// (Dasher-Windows SelectActions.cs / Dasher-Apple MoveDeleteActions.swift):
//
//   - parse():            pure attribute -> enum mapping; unknown values
//                         return nullopt (log + no-op, never crash).
//   - direct_plan():      pure keyboard-mode chord planning (ydotool evdev
//                         keycodes). Sentence has no universal Linux chord:
//                         like Windows, it degrades to the paragraph chord
//                         and the plan reports the fallback so telemetry can
//                         flag it (fallback=paragraph).
//   - editor_*():         editor-mode operations on GtkTextBuffer using
//                         gtkmm's NATIVE TextIter boundary primitives
//                         (forward_word_end/starts_word, forward_sentence_end,
//                         line iterators) — the RFC's philosophy: frontends
//                         select natively with platform primitives, never a
//                         hand-rolled copy of the engine's separators.
//
// Everything except the editor_* functions is pure and unit-tested; the
// editor_* functions are exercised against real headless GtkTextBuffers in
// the same suite (a TextBuffer is a data model and needs no display).

#include <gtkmm/textbuffer.h>
#include <optional>
#include <string>
#include <vector>

enum class ControlDistance { Char, Word, Sentence, Paragraph, All };
enum class ControlDirection { Backward, Forward };

class ControlActions {
  public:
    // True when the action name is one of the three RFC 0021 names.
    static bool is_control_action(const std::string& name);

    // Parse the callback's attribute values. The engine validates no
    // attributes for custom actions — a typo'd dist must not crash the
    // frontend, callers log and no-op (null nullopt).
    static std::optional<std::pair<ControlDistance, ControlDirection>> parse(const std::string& dist,
                                                                             const std::string& forward);

    static const char* distance_name(ControlDistance dist);
    static bool is_forward(ControlDirection dir);

    // ── Keyboard (direct) mode: chord planning ───────────────────────────
    //
    // One chord = a set of modifier evdev keycodes plus an ordered list of
    // principal keycodes. Sent as ONE ydotool invocation (presses in order,
    // releases in reverse — the atomicity contract from RFC 0021's injection
    // section; DirectModeService builds the command from chord_sequence()).
    struct Chord {
        std::vector<int> mods; // e.g. {29} Ctrl, {42} Shift
        std::vector<int> keys; // principal keys, pressed in order
    };

    // Ordered plan for the action. Empty plan = no viable chord (caller
    // no-ops with a log line rather than injecting something wrong).
    // sentence_fallback is set when a sentence granularity was degraded to
    // the paragraph chord (direct mode has no sentence keystroke) so
    // telemetry can append fallback=paragraph — field data must never read
    // sentence-accurate when the applied granularity was paragraph.
    static std::vector<Chord> direct_plan(const std::string& action, ControlDistance dist, ControlDirection dir,
                                          bool& sentence_fallback);

    // Render a chord as a ydotool key argument ("29:1 42:1 106:1 106:0
    // 42:0 29:0" — presses in order, releases in reverse). Pure; validates
    // that codes are positive so nothing but digits/colons/spaces can reach
    // the shell.
    static std::string chord_sequence(const Chord& chord);

    // ── Editor mode: native TextIter operations ──────────────────────────
    //
    // Move: place the cursor at the measured boundary. Word moves use GTK's
    // own move_words algorithm (forward_word_end + advance to starts_word —
    // exactly what GtkTextView's Ctrl+Right runs); sentence/paragraph/line
    // use the native boundary iterators. Returns true if the cursor moved.
    static bool editor_move(Glib::RefPtr<Gtk::TextBuffer> buffer, ControlDistance dist, ControlDirection dir);

    // Select: the active-end state machine (RFC 0021 §directional
    // semantics, mirroring Dasher-Windows' EditorBounds). STATELESS
    // grow-only model: the anchor is the end OPPOSITE the extension
    // direction, the active end is the one being extended; the cursor
    // (active end) lands on the measured target so a chained select in the
    // same direction keeps extending. A forward-then-backward sequence
    // GROWS the selection rather than collapsing it — the deliberate AAC
    // divergence from native Shift+Arrow documented in the RFC's platform
    // table (monotonic selection is safer for eye-gaze users who cannot
    // easily undo an accidental shrink). All = whole buffer.
    static void editor_select(Glib::RefPtr<Gtk::TextBuffer> buffer, ControlDistance dist, ControlDirection dir);

    // Delete: erase the range between the caret and the measured boundary
    // (backward deletes end AT the caret, forward deletes start AT it).
    // Deliberately measured from the CARET, not an existing selection —
    // replacing the selection is the plain Delete key's job; the
    // control-tree delete is a measured-distance operation (same
    // divergence as Dasher-Windows' DeleteBounds). The buffer's changed
    // signal feeds the engine re-seed — the same user-edit path typing
    // takes (RFC 0019 clause 4). Returns the erased byte count (0 = no-op
    // at a document edge).
    static int editor_delete(Glib::RefPtr<Gtk::TextBuffer> buffer, ControlDistance dist, ControlDirection dir);

    // The measured target boundary for a move (also the selection extension
    // target). Public for tests; uses native iterators only.
    static Gtk::TextIter editor_boundary(const Glib::RefPtr<Gtk::TextBuffer>& buffer, const Gtk::TextIter& from,
                                         ControlDistance dist, ControlDirection dir);

    // Evdev keycodes used by the plans (named for readability; values are
    // the fixed linux/input-event-codes.h assignments ydotool speaks).
    static constexpr int kKeyLeftCtrl = 29;
    static constexpr int kKeyLeftShift = 42;
    static constexpr int kKeyLeft = 105;
    static constexpr int kKeyRight = 106;
    static constexpr int kKeyUp = 103;
    static constexpr int kKeyDown = 108;
    static constexpr int kKeyBackspace = 14;
    static constexpr int kKeyDelete = 111;
    static constexpr int kKeyA = 30;
    static constexpr int kKeyHome = 102;
    static constexpr int kKeyEnd = 107;
};
