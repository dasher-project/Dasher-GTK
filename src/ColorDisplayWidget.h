#pragma once

#include "ColorPalette.h"
#include "gtkmm/drawingarea.h"
#include "gtkmm/enums.h"
#include <array>
#include <cmath>

class ColorDisplayWidget : public Gtk::DrawingArea
{
public:
    ColorDisplayWidget(){        
        set_vexpand(true);
        set_valign(Gtk::Align::FILL);

        set_draw_func([this](Glib::RefPtr<Cairo::Context> cr, int width, int height){
            if(width != height){
                set_size_request(std::max(width, height), -1);
                return;
            }

            const float gridSize = std::fmin(width,height)/10.0f;
            for(int i = 0; i < Colors.size(); i++){
                cr->arc((i%2*5.0f+2.5f)*gridSize, ((floorf(i/2.0f)*5.0f)+2.5f)*gridSize, gridSize*2, 0.0, 6.28318530718); //6.28 = 2*PI; Generate Coordinates on the fly 
                cr->set_source_rgba(Colors[i].Red/255.0, Colors[i].Green/255.0, Colors[i].Blue/255.0, Colors[i].Alpha/255.0);
                cr->fill();
            }
        });
    }

    void ReadColorsFromPalette(const Dasher::ColorPalette* palette){
        Colors = {
            palette->GetNodeColor("lowercase", 5, false),
            palette->GetNodeColor("lowercase", 10, false),
            palette->GetNodeColor("lowercase", 15, false),
            palette->GetNodeColor("lowercase", 20, false)
        };
        queue_draw();
    }

    std::array<Dasher::ColorPalette::Color, 4> Colors;
};