#pragma once

#include <cairomm/context.h>
#include <string>

#include "Engine/DasherBridge.h"

// Pango-backed canvas text with per-glyph font fallback — Dasher v5
// behaviour, restored for the v6 command renderer.
//
// Cairo's toy API (select_font_face + show_text) resolves ONE face and has
// no fallback: any glyph missing from it renders as tofu. Every non-Latin
// alphabet drew as squares on systems with perfectly good fonts installed.
// Pango itemizes the text and substitutes per run, so Arabic/Hebrew/Ethiopic
// labels render with whatever the system has.
//
// Both paths in this app go through here — CommandRenderer's opcode-5 draw
// AND DasherBridge's text-measurement callback — so the engine lays labels
// out with the widths this canvas actually draws (DasherCore v0.2.4
// text-size contract).
namespace CanvasText {

/** Draw `text` with the top-left of its ink box at (x, y). */
void draw(const Cairo::RefPtr<Cairo::Context>& cr, const DasherBridge::CanvasFont& font, const std::string& text,
          int font_size, double x, double y);

/** Ink-box extents of `text` — the same metrics the old Cairo-toy path
 *  measured (advance width, ink height). Returns false when unmeasurable. */
bool measure(const Cairo::RefPtr<Cairo::Context>& cr, const DasherBridge::CanvasFont& font, const std::string& text,
             int font_size, int& out_width, int& out_height);

} // namespace CanvasText
