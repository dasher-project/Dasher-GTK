#pragma once

#include "Engine/DasherBridge.h"
#include <cairomm/context.h>
#include <string>
#include <vector>

class CommandRenderer {
public:
    CommandRenderer();

    void render(const DasherBridge::FrameResult& frame, const Cairo::RefPtr<Cairo::Context>& cr);

    void set_font_family(const std::string& family);
    void set_font_slant(Cairo::ToyFontFace::Slant slant);
    void set_font_weight(Cairo::ToyFontFace::Weight weight);

private:
    std::string m_font_family;
    Cairo::ToyFontFace::Slant m_font_slant;
    Cairo::ToyFontFace::Weight m_font_weight;
};
