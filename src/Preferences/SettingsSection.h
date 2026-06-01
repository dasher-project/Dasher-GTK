#pragma once

#include "Engine/DasherBridge.h"
#include "UIComponents/SyncedSwitch.h"
#include "UIComponents/SyncedSlider.h"
#include "UIComponents/SyncedSpinButton.h"
#include "UIComponents/SyncedTextbox.h"
#include "UIComponents/SyncedEnumDropdown.h"
#include "UIComponents/SyncedStringDropdown.h"
#include "UIComponents/PopoverMenuButtonInfo.h"
#include <gtkmm/box.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/frame.h>
#include <gtkmm/scrolledwindow.h>
#include <memory>
#include <vector>
#include <string>

class SettingsSection : public Gtk::Box {
public:
    SettingsSection(const std::string& title, std::shared_ptr<DasherBridge> bridge, const std::string& group_filter, const std::string& subgroup_filter = "")
        : Gtk::Box(Gtk::Orientation::VERTICAL, 8), m_bridge(bridge)
    {
        set_margin_start(12);
        set_margin_end(12);
        set_margin_top(8);
        set_margin_bottom(8);

        int param_count = m_bridge->get_parameter_count();
        for (int i = 0; i < param_count; i++) {
            DasherParameterInfo info = m_bridge->get_parameter_info(i);
            if (info.name.empty()) continue;
            if (!group_filter.empty() && info.group != group_filter) continue;
            if (!subgroup_filter.empty() && info.subgroup != subgroup_filter) continue;

            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            row->set_margin_top(4);
            row->set_margin_bottom(4);

            auto* name_label = Gtk::make_managed<Gtk::Label>(info.name);
            name_label->set_halign(Gtk::Align::START);
            name_label->set_hexpand(true);
            row->append(*name_label);

            switch (info.ui_type) {
            case 1:
                row->append(*Gtk::make_managed<SyncedSwitch>(info.key, m_bridge));
                break;
            case 2:
                row->append(*Gtk::make_managed<SyncedSlider>(info.key, m_bridge, info.min_val, info.max_val, info.step));
                break;
            case 3:
                row->append(*Gtk::make_managed<SyncedSpinButton>(info.key, m_bridge, info.min_val, info.max_val, info.step));
                break;
            case 4:
                row->append(*Gtk::make_managed<SyncedEnumDropdown>(info.key, m_bridge));
                break;
            case 5:
                row->append(*Gtk::make_managed<SyncedTextbox>(info.key, m_bridge));
                break;
            default:
                break;
            }

            if (!info.desc.empty()) {
                row->append(*Gtk::make_managed<PopoverMenuButtonInfo>(info.desc));
            }

            append(*row);
        }

        if (get_first_child() == nullptr) {
            append(*Gtk::make_managed<Gtk::Label>("No settings available for this section."));
        }
    }

private:
    std::shared_ptr<DasherBridge> m_bridge;
};
