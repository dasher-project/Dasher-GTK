#pragma once

#include "Output/DirectModeService.h"
#include <gdkmm/clipboard.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/frame.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <utility>

// Shown when the user enables Keyboard mode but ydotool is missing (issue #38).
// Instead of a dead greyed-out button, this guides the user through installing
// ydotool for their distro (with copy-to-clipboard) and offers a Retry that
// re-checks and enables the mode. Sandboxed builds get a "not available" note.
// GTK-frontend-only; no DasherCore involvement.
class KeyboardSetupDialog : public Gtk::Window {
  public:
    KeyboardSetupDialog(Gtk::Window& parent, DirectModeService* service, std::function<void()> on_available)
        : m_service(service), m_on_available(std::move(on_available)), m_box(Gtk::Orientation::VERTICAL, 12) {
        set_transient_for(parent);
        set_modal(true);
        set_title("Enable Keyboard Mode");
        set_default_size(460, -1);
        set_hide_on_close(true);

        m_box.set_margin(16);
        set_child(m_box);

        if (is_sandboxed())
            build_sandbox_message();
        else
            build_install_guide();
    }

  private:
    // Flatpak/AppImage sandboxes can't reach /dev/uinput, so keyboard mode is
    // unavailable there regardless of ydotool.
    static bool is_sandboxed() {
        if (std::getenv("FLATPAK_ID")) return true;
        return std::ifstream("/.flatpak-info").good();
    }

    static std::string strip(std::string s) {
        auto trim = [](char c) { return c == '"' || c == '\'' || c == ' ' || c == '\n' || c == '\r'; };
        while (!s.empty() && trim(s.front()))
            s.erase(s.begin());
        while (!s.empty() && trim(s.back()))
            s.pop_back();
        return s;
    }

    // Install commands for the distro detected from /etc/os-release (ID / ID_LIKE),
    // with a generic fallback for anything unrecognised.
    static std::string install_command() {
        std::string id, id_like;
        std::ifstream os("/etc/os-release");
        std::string line;
        while (std::getline(os, line)) {
            if (line.rfind("ID=", 0) == 0)
                id = strip(line.substr(3));
            else if (line.rfind("ID_LIKE=", 0) == 0)
                id_like = strip(line.substr(8));
        }
        auto has = [&](const std::string& n) {
            return id.find(n) != std::string::npos || id_like.find(n) != std::string::npos;
        };
        if (has("debian") || has("ubuntu")) return "sudo apt install ydotool\nsudo systemctl enable --now ydotool";
        if (has("fedora") || has("rhel")) return "sudo dnf install ydotool\nsudo systemctl enable --now ydotool";
        if (has("arch")) return "sudo pacman -S ydotool\nsudo systemctl enable --now ydotool";
        return "Install \"ydotool\" with your package manager, then:\nsudo systemctl enable --now ydotool";
    }

    void build_sandbox_message() {
        auto* title = Gtk::make_managed<Gtk::Label>();
        title->set_markup("<b>Keyboard mode isn't available in this build</b>");
        title->set_halign(Gtk::Align::START);
        m_box.append(*title);

        auto* body = Gtk::make_managed<Gtk::Label>(
            "Typing into other applications needs access to /dev/uinput, which a sandboxed "
            "Flatpak or AppImage build can't provide.\n\n"
            "Install Dasher via your package manager or build from source to use keyboard mode.");
        body->set_wrap(true);
        body->set_halign(Gtk::Align::START);
        body->set_xalign(0.0f);
        m_box.append(*body);

        auto* close = Gtk::make_managed<Gtk::Button>("Close");
        close->set_halign(Gtk::Align::END);
        close->signal_clicked().connect([this]() { set_visible(false); });
        m_box.append(*close);
    }

    void build_install_guide() {
        auto* title = Gtk::make_managed<Gtk::Label>();
        title->set_markup("<b>Install ydotool to use keyboard mode</b>");
        title->set_halign(Gtk::Align::START);
        m_box.append(*title);

        auto* body = Gtk::make_managed<Gtk::Label>(
            "Keyboard mode types Dasher's output into other applications using ydotool, which "
            "isn't installed. Run these commands, then press Retry:");
        body->set_wrap(true);
        body->set_halign(Gtk::Align::START);
        body->set_xalign(0.0f);
        m_box.append(*body);

        m_command = install_command();
        auto* cmd_label = Gtk::make_managed<Gtk::Label>(m_command);
        cmd_label->set_selectable(true);
        cmd_label->set_halign(Gtk::Align::START);
        cmd_label->set_xalign(0.0f);
        cmd_label->set_margin(8);
        cmd_label->add_css_class("monospace");
        auto* cmd_frame = Gtk::make_managed<Gtk::Frame>();
        cmd_frame->set_child(*cmd_label);
        m_box.append(*cmd_frame);

        m_status.set_halign(Gtk::Align::START);
        m_status.set_wrap(true);
        m_status.set_xalign(0.0f);
        m_status.add_css_class("dim-label");
        m_box.append(m_status);

        auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        buttons->set_halign(Gtk::Align::END);

        auto* copy = Gtk::make_managed<Gtk::Button>("Copy commands");
        copy->signal_clicked().connect([this]() {
            get_clipboard()->set_text(m_command);
            m_status.set_text("Copied to clipboard.");
        });
        buttons->append(*copy);

        auto* retry = Gtk::make_managed<Gtk::Button>("Retry");
        retry->add_css_class("suggested-action");
        retry->signal_clicked().connect([this]() { on_retry(); });
        buttons->append(*retry);

        m_box.append(*buttons);
    }

    void on_retry() {
        if (m_service && m_service->recheck()) {
            m_status.set_text("ydotool found. Keyboard mode is ready.");
            if (m_on_available) m_on_available();
            set_visible(false);
        } else {
            m_status.set_text(
                "ydotool still isn't available. Make sure it's installed and the ydotool service is running.");
        }
    }

    DirectModeService* m_service;
    std::function<void()> m_on_available;
    std::string m_command;
    Gtk::Box m_box;
    Gtk::Label m_status;
};
