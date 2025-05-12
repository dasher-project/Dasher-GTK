#include "MainWindow.h"
#include "ColorPalette.h"
#include "Parameters.h"
#include "RenderingCanvas.h"
#include "cairomm/fontface.h"
#include "gdkmm/display.h"
#include "giomm/liststore.h"
#include "glibmm/object.h"
#include "glibmm/refptr.h"
#include "gtk/gtk.h"
#include "gtkmm/enums.h"
#include "gtkmm.h"
#include "pangomm/fontdescription.h"
#include <memory>

Cairo::ToyFontFace::Slant getSlantFromPango(Pango::Style s){
    if(s == Pango::Style::NORMAL) return Cairo::ToyFontFace::Slant::NORMAL;
    if(s == Pango::Style::ITALIC) return Cairo::ToyFontFace::Slant::ITALIC;
    if(s == Pango::Style::OBLIQUE) return Cairo::ToyFontFace::Slant::OBLIQUE;
    return Cairo::ToyFontFace::Slant::NORMAL;
}

#pragma clang optimize off
MainWindow::MainWindow() :
    m_main_vertical_box(Gtk::Orientation::VERTICAL),
    m_side_panel(Gtk::Orientation::VERTICAL),
    m_main_pane(Gtk::Orientation::HORIZONTAL)
{
    g_setenv("GTK_CSD", "0", false);

    Glib::RefPtr<Gtk::CssProvider> css = Gtk::CssProvider::create();
    css->load_from_path("./UIStyle.css");
    get_style_context()->add_provider_for_display(Gdk::Display::get_default(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    set_title("Dasher v6");

    set_child(m_main_vertical_box);
    m_main_vertical_box.append(m_header_bar);
    m_main_vertical_box.append(m_main_pane);
    m_main_vertical_box.append(m_footer_bar);

    m_message_overlay.set_child(m_canvas);
    m_message_overlay.ConnectToDasher(m_canvas.dasherController);

    // Pack Header Bar
    m_header_bar.pack_start(m_new_button);
    m_header_bar.pack_start(m_open_button);
    m_header_bar.pack_start(m_save_button);
    m_header_bar.pack_start(m_separator_1);
    m_header_bar.pack_start(m_play_button);
    m_header_bar.pack_start(m_separator_2);
    m_header_bar.pack_start(m_layout_chooser);
    m_header_bar.pack_start(m_separator_3);
    m_header_bar.pack_start(m_pref_button);

    // Pack Footer Bar
    m_footer_bar.pack_start(m_alphabet_chooser);
    m_footer_bar.pack_start(m_separator_4);
    m_footer_bar.pack_start(m_speed_adjustment);
    m_footer_bar.pack_start(m_learning_label);
    m_footer_bar.pack_start(m_learning_switch);
    m_footer_bar.pack_start(m_separator_5);
    m_footer_bar.pack_start(m_color_chooser);
    m_footer_bar.pack_start(m_font_chooser);
    m_footer_bar.pack_start(m_separator_6);
    m_footer_bar.pack_start(m_speech_enable_label);
    m_footer_bar.pack_start(m_speech_enable_switch);
    m_speech_enable_switch.set_valign(Gtk::Align::CENTER);
    m_learning_switch.set_valign(Gtk::Align::CENTER);

    auto color_factory = Gtk::SignalListItemFactory::create();
    color_factory->signal_setup().connect([](const std::shared_ptr<Gtk::ListItem> item) {
        auto widget = Gtk::make_managed<ColorDisplayWidget>();
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
        auto label = Gtk::make_managed<Gtk::Label>("Test");
        box->append(*widget);
        box->append(*label);
        item->set_child(*box);
    });
    color_factory->signal_bind().connect([](const std::shared_ptr<Gtk::ListItem> item){
        std::shared_ptr<PaletteProxy> palette = std::dynamic_pointer_cast<PaletteProxy>(item->get_item());
        Gtk::Box* box = dynamic_cast<Gtk::Box*>(item->get_child());
        ColorDisplayWidget* widget = dynamic_cast<ColorDisplayWidget*>(box->get_first_child());
        Gtk::Label* label = dynamic_cast<Gtk::Label*>(box->get_last_child());
        widget->ReadColorsFromPalette(palette->p);
        label->set_label(palette->p->PaletteName);
    });
    m_color_chooser.set_list_factory(color_factory);

    auto color_factory_header = Gtk::SignalListItemFactory::create();
    color_factory_header->signal_setup().connect([](const std::shared_ptr<Gtk::ListItem> item) {
        auto widget = Gtk::make_managed<ColorDisplayWidget>();
        item->set_child(*widget);
    });
    color_factory_header->signal_bind().connect([](const std::shared_ptr<Gtk::ListItem> item){
        std::shared_ptr<PaletteProxy> palette = std::dynamic_pointer_cast<PaletteProxy>(item->get_item());
        ColorDisplayWidget* widget = dynamic_cast<ColorDisplayWidget*>(item->get_child());
        widget->ReadColorsFromPalette(palette->p);
    });
    m_color_chooser.set_factory(color_factory_header);

    const std::map<std::string, Dasher::ColorPalette*>& palettes = *m_canvas.dasherController->GetColorIO()->GetKnownPalettes();
    for (const auto &[key, value] : palettes) {
        colorPaletteList->append(PaletteProxy::create(value));
    }
    m_color_chooser.set_model(colorPaletteList);
    
    std::string selected_color = m_canvas.dasherController->GetStringParameter(Dasher::Parameter::SP_COLOUR_ID);
    for(guint i = 0; i < colorPaletteList->get_n_items(); i++){
        if(colorPaletteList->get_item(i)->p->PaletteName.compare(selected_color) == 0){
            m_color_chooser.set_selected(i);
        }
    }
    
    m_color_chooser.property_selected_item().signal_changed().connect([this](){
        std::shared_ptr<PaletteProxy> palette = std::dynamic_pointer_cast<PaletteProxy>(m_color_chooser.get_selected_item());
        m_canvas.dasherController->SetStringParameter(Dasher::Parameter::SP_COLOUR_ID, palette->p->PaletteName);
    });

    std::vector<std::string> Alphabets;
    m_canvas.dasherController->GetPermittedValues(Dasher::Parameter::SP_ALPHABET_ID, Alphabets);
    std::vector<Glib::ustring> AlphaUString(Alphabets.begin(), Alphabets.end());
    m_alphabet_chooser.set_model(Gtk::StringList::create(AlphaUString));
    std::string selected_alphabet = m_canvas.dasherController->GetStringParameter(Dasher::Parameter::SP_ALPHABET_ID);
    m_alphabet_chooser.set_selected(std::find(AlphaUString.begin(), AlphaUString.end(), selected_alphabet) - AlphaUString.begin());
    m_alphabet_chooser.property_selected_item().signal_changed().connect([this](){
        Glib::ustring s = std::dynamic_pointer_cast<Gtk::StringObject>(m_alphabet_chooser.get_selected_item())->get_string();
        m_canvas.dasherController->SetStringParameter(Dasher::Parameter::SP_ALPHABET_ID, s);
    });


    m_font_chooser.property_font_desc().signal_changed().connect([this](){
        Pango::FontDescription newFont = m_font_chooser.get_font_desc();
        m_canvas.renderingBackend->select_font_face(newFont.get_family(),
            getSlantFromPango(newFont.get_style()),
            (newFont.get_weight() == Pango::Weight::NORMAL) ? Cairo::ToyFontFace::Weight::NORMAL : Cairo::ToyFontFace::Weight::BOLD);
        m_canvas.dasherController->SetLongParameter(Dasher::Parameter::LP_DASHER_FONTSIZE, newFont.get_size() / Pango::SCALE);
    });

    m_speed_adjustment.set_adjustment(Gtk::Adjustment::create(m_canvas.dasherController->GetLongParameter(Dasher::Parameter::LP_MAX_BITRATE), 50, 1000));
    m_speed_adjustment.property_value().signal_changed().connect([this](){
        m_canvas.dasherController->SetLongParameter(Dasher::Parameter::LP_MAX_BITRATE, m_speed_adjustment.get_value_as_int());
    });

    m_learning_switch.set_active(m_canvas.dasherController->GetBoolParameter(Dasher::Parameter::BP_LM_ADAPTIVE));
    m_learning_switch.property_active().signal_changed().connect([this](){
        m_canvas.dasherController->SetBoolParameter(Dasher::Parameter::BP_LM_ADAPTIVE, m_learning_switch.get_active());
    });

    m_main_pane.set_start_child(m_message_overlay);
    m_main_pane.set_resize_start_child(true);
    m_main_pane.set_end_child(m_side_panel);

    m_side_panel.set_size_request(150,-1);
    m_side_panel.set_vexpand(true);
    m_side_panel.set_valign(Gtk::Align::FILL);
}
#pragma clang optimize on