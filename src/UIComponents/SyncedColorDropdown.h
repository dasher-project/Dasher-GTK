#pragma once

#include "Engine/DasherBridge.h"
#include "ColorDisplayWidget.h"
#include "gtkmm/box.h"
#include "gtkmm/dropdown.h"
#include "gtkmm/label.h"
#include "gtkmm/signallistitemfactory.h"
#include "giomm/liststore.h"
#include <memory>
#include <string>

class PaletteProxy : public Glib::Object {
public:
    std::string name;
    int preview_colors[4] = {};
    static Glib::RefPtr<PaletteProxy> create(const std::string& name) {
        return Glib::make_refptr_for_instance<PaletteProxy>(new PaletteProxy(name));
    }
protected:
    PaletteProxy(const std::string& n) : name(n) {}
};

class SyncedColorDropdown : public Gtk::DropDown {
public:
    SyncedColorDropdown(std::shared_ptr<DasherBridge> bridge)
        : m_bridge(bridge)
    {
        int count = m_bridge->get_palette_count();
        for (int i = 0; i < count; i++) {
            auto proxy = PaletteProxy::create(m_bridge->get_palette_name(i));
            m_bridge->get_palette_preview_colors(i, proxy->preview_colors);
            palette_list->append(proxy);
        }

        set_list_factory(list_factory);
        set_factory(header_factory);
        set_model(palette_list);

        header_factory->signal_setup().connect([](const std::shared_ptr<Gtk::ListItem> item) {
            item->set_child(*Gtk::make_managed<ColorDisplayWidget>());
        });
        header_factory->signal_bind().connect([](const std::shared_ptr<Gtk::ListItem> item) {
            auto proxy = std::dynamic_pointer_cast<PaletteProxy>(item->get_item());
            auto* widget = dynamic_cast<ColorDisplayWidget*>(item->get_child());
            widget->set_colors_from_argb(proxy->preview_colors);
        });

        list_factory->signal_setup().connect([](const std::shared_ptr<Gtk::ListItem> item) {
            auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
            box->append(*Gtk::make_managed<ColorDisplayWidget>());
            box->append(*Gtk::make_managed<Gtk::Label>(""));
            item->set_child(*box);
        });
        list_factory->signal_bind().connect([](const std::shared_ptr<Gtk::ListItem> item) {
            auto proxy = std::dynamic_pointer_cast<PaletteProxy>(item->get_item());
            auto* box = dynamic_cast<Gtk::Box*>(item->get_child());
            auto* widget = dynamic_cast<ColorDisplayWidget*>(box->get_first_child());
            auto* label = dynamic_cast<Gtk::Label*>(box->get_last_child());
            widget->set_colors_from_argb(proxy->preview_colors);
            label->set_label(proxy->name);
        });

        std::string current = m_bridge->get_current_palette();
        for (guint i = 0; i < palette_list->get_n_items(); i++) {
            if (palette_list->get_item(i)->name == current) {
                set_selected(i);
                break;
            }
        }

        property_selected().signal_changed().connect([this]() {
            auto proxy = std::dynamic_pointer_cast<PaletteProxy>(get_selected_item());
            if (proxy) {
                m_bridge->set_palette(proxy->name);
            }
        });
    }

protected:
    std::shared_ptr<DasherBridge> m_bridge;
    Glib::RefPtr<Gio::ListStore<PaletteProxy>> palette_list = Gio::ListStore<PaletteProxy>::create();
    Glib::RefPtr<Gtk::SignalListItemFactory> header_factory = Gtk::SignalListItemFactory::create();
    Glib::RefPtr<Gtk::SignalListItemFactory> list_factory = Gtk::SignalListItemFactory::create();
};
