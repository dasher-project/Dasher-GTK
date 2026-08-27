#pragma once

#include "Engine/DasherBridge.h"
#include "i18n.h"
#include "Output/DirectModeService.h"
#include "Output/KeyboardWindowX11.h"
#include "UiSettings.h"
#include "Output/TtsService.h"
#include "UIComponents/ImageButton.h"
#include "UIComponents/ImageToggleButton.h"
#include "UIComponents/KeyboardSetupDialog.h"
#include "UIComponents/RenderingCanvas.h"
#include "UIComponents/MessageOverlay.h"
#include "UIComponents/SyncedSpinButton.h"
#include "UIComponents/SyncedStringDropdown.h"
#include "UIComponents/SyncedSwitch.h"
#include "UIComponents/SyncedColorDropdown.h"
#include "Preferences/PreferencesWindow.h"
#include "Analytics/AnalyticsClient.h"
#include "Analytics/AnalyticsSettings.h"
#include <gtkmm/alertdialog.h>
#include <gtkmm/window.h>
#include <atomic>
#include <memory>
#include <gtkmm/box.h>
#include <gtkmm/paned.h>
#include <gtk/gtk.h> // gtk_paned_set_*_child(null) to detach on re-layout
#include <gtkmm/actionbar.h>
#include <gtkmm/label.h>
#include <gtkmm/textview.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/separator.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/fontdialog.h>
#include <gtkmm/fontdialogbutton.h>
#include <gtkmm/menubutton.h>
#include <giomm/menu.h>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/switch.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>

class MainWindow : public Gtk::Window {
  public:
    MainWindow();
    ~MainWindow() override;

  protected:
    Gtk::Box m_main_box = Gtk::Box(Gtk::Orientation::VERTICAL);
    Gtk::Paned m_pane = Gtk::Paned(Gtk::Orientation::HORIZONTAL);

    Gtk::ActionBar m_header_bar;

    // Pack a child into an ActionBar with breathing room around it. GTK4
    // removed widget spacing from CSS (the `spacing` property is silently
    // ignored) and ActionBar's internal box isn't accessible from code, so
    // margins on each child are the only lever we have.
    void pack_bar_start(Gtk::ActionBar& bar, Gtk::Widget& child);
    ImageButton m_new_button = ImageButton(_("New"), "file-plus");
    ImageButton m_open_button = ImageButton(_("Open"), "folder-open");
    ImageButton m_save_button = ImageButton(_("Save"), "save");
    ImageButton m_play_button = ImageButton(_("Play"), "gamepad-2");
    // Layout/output-mode menu (Apple's layoutPickerMenu / Windows' BtnMode):
    // Right / Left / Bottom / Top / Keyboard — where the text pane sits, with
    // Keyboard hiding it entirely for direct entry.
    Gtk::MenuButton m_layout_button;
    Glib::RefPtr<Gio::Menu> m_layout_menu = Gio::Menu::create();
    Gtk::Label m_layout_label = Gtk::Label(_("Right side"));
    enum class PaneLayout { Right, Left, Bottom, Top, Keyboard };
    PaneLayout m_pane_layout = PaneLayout::Right;
    void set_pane_layout(PaneLayout layout);
    // Legacy toggle target for the keyboard mode handlers.
    ImageToggleButton m_keyboard_button = ImageToggleButton(_("Keyboard"), "keyboard");
    ImageButton m_pref_button = ImageButton(_("Prefs"), "settings");

    RenderingCanvas m_canvas;
    MessageOverlay m_message_overlay;

    Gtk::Box m_side_panel = Gtk::Box(Gtk::Orientation::VERTICAL);
    Gtk::ActionBar m_panel_bar;
    ImageButton m_copy_button = ImageButton(_("Copy"), "copy");
    ImageButton m_copyall_button = ImageButton(_("Copy all"), "copy-all");
    ImageButton m_paste_button = ImageButton(_("Paste"), "paste");
    ImageButton m_read_button = ImageButton(_("Read"), "read");
    Gtk::TextView m_text_view;

    Gtk::ActionBar m_footer_bar;
    SyncedStringDropdown m_alphabet_chooser;
    // Speed control in the Windows bottom-bar style: "Speed  [–] 5.7 [+]"
    // where the value is v5-style (raw LP_MAX_BITRATE / 100, e.g. 0.8,
    // 5.0) — not the raw integer the engine stores.
    Gtk::Label m_speed_label = Gtk::Label(_("Speed"));
    Gtk::Button m_speed_down_btn;
    Gtk::Label m_speed_value_label;
    Gtk::Button m_speed_up_btn;
    double m_speed_step = 0.1; // in v5 units; re-derived from the manifest
    Gtk::Label m_learning_label = Gtk::Label(_("Learning"));
    SyncedSwitch m_learning_switch;
    Gtk::Label m_wpm_label;   // live "4.2 cps · 50 wpm" readout (RFC 0012)
    double m_speed_min = 0.1; // v5 units; re-derived from the manifest
    double m_speed_max = 10.0;
    Gtk::Label m_speech_label = Gtk::Label(_("Speech"));
    Gtk::Switch m_speech_switch;

    std::unique_ptr<DirectModeService> m_direct_mode;
    std::unique_ptr<TtsService> m_tts;
    bool m_direct_mode_active = false;

    // Cleared in the destructor before any teardown. Cross-thread callbacks
    // (DirectModeService's failure callback runs on its worker) capture a
    // shared copy and check it inside main-thread idles, so an idle queued
    // during shutdown cannot touch a half-destroyed window.
    std::shared_ptr<std::atomic<bool>> m_ui_alive = std::make_shared<std::atomic<bool>>(true);

    // Setup helper shown when Keyboard mode is enabled without ydotool (issue #38).
    std::unique_ptr<KeyboardSetupDialog> m_keyboard_setup_dialog;
    void show_keyboard_setup_dialog();
    // Injection failed mid-session (daemon stopped, permissions lost): turn the
    // mode off and tell the user why, instead of dropping output silently.
    void handle_keyboard_mode_failure();

    // Speed stepper helpers (Windows-style bottom bar). Raw engine units are
    // LP_MAX_BITRATE (v5's ×100 scale); the UI shows the v5 value.
    void nudge_speed(double delta_v5);
    void update_speed_display();

    // Keyboard-mode mini bar (like Dasher-Windows' KeyboardMiniBar): added as
    // an overlay on the message overlay (itself a Gtk::Overlay), floating
    // top-right, settings + exit. Visible only while keyboard mode is on.
    Gtk::Box m_keyboard_minibar = Gtk::Box(Gtk::Orientation::HORIZONTAL, 2);
    Gtk::Button m_minibar_settings_btn;
    Gtk::Button m_minibar_exit_btn;

    // RFC 0015 parity: keyboard-mode window opacity (persisted frontend-side).
    ui::UiSettings m_ui_settings = ui::UiSettings::load();
    double keyboard_opacity() const;
    void set_keyboard_opacity(double v);

    PreferencesWindow m_preferences_window;

    // Frontend analytics consent; drives the first-run opt-in prompt below.
    analytics::AnalyticsSettings m_analytics = analytics::AnalyticsSettings::load();
    void maybe_show_consent_dialog();
    void capture_app_launched();
};
