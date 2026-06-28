#include "CommandRenderer.h"
#include <dasher.h>

CommandRenderer::CommandRenderer()
    : m_font_family("Sans")
    , m_font_slant(Cairo::ToyFontFace::Slant::NORMAL)
    , m_font_weight(Cairo::ToyFontFace::Weight::NORMAL)
{
}

void CommandRenderer::set_font_family(const std::string& family) {
    m_font_family = family;
}

void CommandRenderer::set_font_slant(Cairo::ToyFontFace::Slant slant) {
    m_font_slant = slant;
}

void CommandRenderer::set_font_weight(Cairo::ToyFontFace::Weight weight) {
    m_font_weight = weight;
}

void CommandRenderer::render(const DasherBridge::FrameResult& frame, const Cairo::RefPtr<Cairo::Context>& cr) {
    const auto& cmds = frame.commands;
    const auto& strs = frame.strings;

    const int CMD_SIZE = 6;
    int count = static_cast<int>(cmds.size()) / CMD_SIZE;

    for (int i = 0; i < count; i++) {
        int base = i * CMD_SIZE;
        int op = cmds[base + 0];
        int a = cmds[base + 1];
        int b = cmds[base + 2];
        int c = cmds[base + 3];
        int d = cmds[base + 4];
        int argb = cmds[base + 5];

        double red = dasher_color_get_red(argb) / 255.0;
        double green = dasher_color_get_green(argb) / 255.0;
        double blue = dasher_color_get_blue(argb) / 255.0;
        double alpha = dasher_color_get_alpha(argb) / 255.0;

        cr->set_source_rgba(red, green, blue, alpha);

        switch (op) {
        case 0:
            cr->set_operator(Cairo::Context::Operator::SOURCE);
            cr->rectangle(0, 0, a, b);
            cr->fill();
            cr->set_operator(Cairo::Context::Operator::OVER);
            break;

        case 1: {
            cr->begin_new_path();
            cr->arc(a, b, c, 0.0, 6.28318530718);
            if (d) {
                cr->fill();
            } else {
                cr->stroke();
            }
            break;
        }

        case 2:
            cr->begin_new_path();
            cr->move_to(a, b);
            cr->line_to(c, d);
            cr->stroke();
            break;

        case 3:
            cr->begin_new_path();
            cr->rectangle(a, b, c - a, d - b);
            cr->stroke();
            break;

        case 4:
            cr->begin_new_path();
            cr->rectangle(a, b, c - a, d - b);
            cr->fill();
            break;

        case 5: {
            if (d < 0 || d >= static_cast<int>(strs.size())) break;
            const std::string& text = strs[d];
            if (text.empty()) break;

            cr->select_font_face(m_font_family, m_font_slant, m_font_weight);
            cr->set_font_size(c);

            Cairo::TextExtents te;
            cr->get_text_extents(text, te);
            cr->begin_new_path();
            cr->move_to(a - te.x_bearing, b - te.y_bearing);
            cr->show_text(text);
            break;
        }

        default:
            break;
        }
    }
}
