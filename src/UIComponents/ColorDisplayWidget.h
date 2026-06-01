#pragma once

#include "Engine/DasherBridge.h"
#include "gtkmm/drawingarea.h"
#include <array>

class ColorDisplayWidget : public Gtk::DrawingArea {
public:
    ColorDisplayWidget() {
        set_vexpand(true);
        set_valign(Gtk::Align::FILL);

        set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
            if (width != height) {
                set_size_request(std::max(width, height), -1);
                return;
            }
            float gridSize = std::fmin(width, height) / 10.0f;
            for (int i = 0; i < 4; i++) {
                cr->arc((i % 2 * 5.0f + 2.5f) * gridSize,
                        (std::floor(i / 2.0f) * 5.0f + 2.5f) * gridSize,
                        gridSize * 2, 0.0, 6.28318530718);
                cr->set_source_rgba(colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
                cr->fill();
            }
        });
    }

    void set_colors_from_argb(int argb_colors[4]) {
        for (int i = 0; i < 4; i++) {
            int a = (argb_colors[i] >> 24) & 0xFF;
            int r = (argb_colors[i] >> 16) & 0xFF;
            int g = (argb_colors[i] >> 8) & 0xFF;
            int b = argb_colors[i] & 0xFF;
            colors[i] = {r / 255.0, g / 255.0, b / 255.0, a / 255.0};
        }
        queue_draw();
    }

private:
    std::array<std::array<double, 4>, 4> colors = {};
};
