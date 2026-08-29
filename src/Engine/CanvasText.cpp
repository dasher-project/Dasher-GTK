#include "Engine/CanvasText.h"

#include <pangomm/layout.h>

namespace {

Pango::FontDescription make_font_description(const DasherBridge::CanvasFont& font, int font_size) {
    Pango::FontDescription desc(font.family.empty() ? "Sans" : font.family);

    switch (font.slant) {
    case Cairo::ToyFontFace::Slant::OBLIQUE:
    case Cairo::ToyFontFace::Slant::ITALIC:
        desc.set_style(Pango::Style::ITALIC);
        break;
    default:
        desc.set_style(Pango::Style::NORMAL);
        break;
    }

    // Cairo's toy API distinguishes only NORMAL/BOLD; anything heavier maps
    // to bold, lighter to normal — same resolution the toy backend makes.
    if (static_cast<int>(font.weight) >= static_cast<int>(Cairo::ToyFontFace::Weight::BOLD))
        desc.set_weight(Pango::Weight::BOLD);
    else
        desc.set_weight(Pango::Weight::NORMAL);

    // Absolute size in px (device units) — matches the old
    // cr->set_font_size(font_size) semantics on an identity-transform
    // canvas, where user-space units are pixels.
    desc.set_absolute_size(static_cast<double>(font_size) * PANGO_SCALE);
    return desc;
}

Glib::RefPtr<Pango::Layout> make_layout(const Cairo::RefPtr<Cairo::Context>& cr,
                                        const DasherBridge::CanvasFont& font, const std::string& text,
                                        int font_size) {
    auto layout = Pango::Layout::create(cr);
    layout->set_font_description(make_font_description(font, font_size));
    layout->set_text(text);
    return layout;
}

} // namespace

namespace CanvasText {

void draw(const Cairo::RefPtr<Cairo::Context>& cr, const DasherBridge::CanvasFont& font,
          const std::string& text, int font_size, double x, double y) {
    if (!cr || text.empty()) return;

    auto layout = make_layout(cr, font, text, font_size);

    // Position by the ink box, mirroring the old renderer's
    // x_bearing/y_bearing placement so vertical alignment of labels is
    // unchanged for Latin text.
    const Pango::Rectangle ink = layout->get_ink_extents();
    cr->begin_new_path();
    cr->move_to(x - ink.get_x() / PANGO_SCALE, y - ink.get_y() / PANGO_SCALE);
    layout->show_in_cairo_context(cr);
}

bool measure(const Cairo::RefPtr<Cairo::Context>& cr, const DasherBridge::CanvasFont& font,
             const std::string& text, int font_size, int& out_width, int& out_height) {
    if (!cr || text.empty() || font_size <= 0) return false;

    auto layout = make_layout(cr, font, text, font_size);

    // Width from LOGICAL extents (advance), height from INK — exactly the
    // old Cairo-toy contract (x_advance for width, extents.height for
    // height). The engine lays labels out by advance; ink width would
    // under-report strings whose bearings differ from their advance (e.g.
    // trailing spaces, marks) and cause overlaps at deep zoom.
    const Pango::Rectangle logical = layout->get_logical_extents();
    const Pango::Rectangle ink = layout->get_ink_extents();
    out_width = (logical.get_width() + PANGO_SCALE / 2) / PANGO_SCALE;
    out_height = (ink.get_height() + PANGO_SCALE / 2) / PANGO_SCALE;
    return true;
}

} // namespace CanvasText
