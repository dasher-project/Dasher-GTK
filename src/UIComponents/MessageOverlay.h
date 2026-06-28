#pragma once

#include "Engine/DasherBridge.h"
#include "PopoverBox.h"
#include "gtkmm/box.h"
#include "gtkmm/overlay.h"
#include <chrono>
#include <memory>
#include <vector>
#include <array>

class MessageOverlay : public Gtk::Overlay {
public:
    MessageOverlay() {
        add_overlay(m_box);
        m_box.append(m_message1);
        m_box.append(m_message2);

        m_box.set_valign(Gtk::Align::START);
        m_box.set_halign(Gtk::Align::CENTER);
        m_box.set_vexpand(false);
        m_box.set_hexpand(false);

        m_message1.property_child_revealed().signal_changed().connect([this]() {
            if (!m_message1.get_reveal_child()) erase_message(&m_message1);
        });
        m_message2.property_child_revealed().signal_changed().connect([this]() {
            if (!m_message2.get_reveal_child()) erase_message(&m_message2);
        });

        add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
            for (auto& m : messages) {
                if (m.timed && m.widget &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - m.start_time).count() > 5000) {
                    m.widget->set_reveal_child(false);
                    m.timed = false;
                }
            }
            for (auto& m : messages) {
                if (m.widget) continue;
                for (auto* p : slots) {
                    if (!p->get_reveal_child() && !p->get_child_revealed()) {
                        p->reveal(m.text);
                        m.start_time = std::chrono::steady_clock::now();
                        m.widget = p;
                        m_box.reorder_child_at_start(*p);
                        break;
                    }
                }
            }
            return true;
        });
    }

    void ConnectToDasher(std::shared_ptr<DasherBridge> bridge) {
        m_bridge = bridge;
    }

    void show_message(const std::string& text, bool timed = true) {
        messages.push_back({text, timed, std::chrono::steady_clock::now(), nullptr});
    }

private:
    void erase_message(PopoverBox* box) {
        messages.erase(std::remove_if(messages.begin(), messages.end(),
            [&](const QueuedMessage& m) { return m.widget == box; }), messages.end());
    }

    struct QueuedMessage {
        std::string text;
        bool timed;
        std::chrono::time_point<std::chrono::steady_clock> start_time;
        PopoverBox* widget = nullptr;
    };

    std::shared_ptr<DasherBridge> m_bridge;
    Gtk::Box m_box = Gtk::Box(Gtk::Orientation::VERTICAL);
    PopoverBox m_message1;
    PopoverBox m_message2;
    std::array<PopoverBox*, 2> slots = {&m_message1, &m_message2};
    std::vector<QueuedMessage> messages;
};
