#include "ColorPalette.h"
#include "gtkmm.h"

class PaletteProxy : public Glib::Object
{
    public:
        PaletteProxy(const Dasher::ColorPalette* p): p(p){}
        const Dasher::ColorPalette* p;
        static Glib::RefPtr<PaletteProxy> create(const Dasher::ColorPalette* p)
        {
            return Glib::make_refptr_for_instance<PaletteProxy>(new PaletteProxy(p));
        }
};