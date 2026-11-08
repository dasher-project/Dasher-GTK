#pragma once

#include "Engine/DasherBridge.h"
#include "i18n.h"
#include "Output/DirectModeService.h"
#include "Output/KeyboardWindowX11.h"
#include "UiSettings.h"
#include "UpdateChecker.h"
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
#include <gtkmm/flowbox.h>
#include <gtkmm/gesturedrag.h>
#include <sigc++/connection.h>
#include <functional>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>

class TargetContextWatcher;

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
    ImageButton m_play_button = ImageButton(_("Game"), "gamepad-2");
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
    // Issue #110 item 3: the output-pane toolbar wraps (Gtk::FlowBox) — the
    // previous single-row ActionBar clipped its overflow once the pane got
    // narrow, hiding Copy/Read; Windows' toolbar panel wraps the same way.
    Gtk::FlowBox m_panel_bar;
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
    Gtk::Label m_wpm_label;         // live "4.2 cps · 50 wpm" readout (RFC 0012)
    Gtk::Label m_game_target_label; // game mode target overlay (Windows parity)
    double m_speed_min = 0.1;       // v5 units; re-derived from the manifest
    double m_speed_max = 10.0;
    Gtk::Label m_speech_label = Gtk::Label(_("Speech"));
    Gtk::Switch m_speech_switch;

    std::unique_ptr<DirectModeService> m_direct_mode;
    std::unique_ptr<TtsService> m_tts;
    bool m_direct_mode_active = false;

    // ── Editor contract (RFC 0019) ──
    // True while a programmatic (engine-origin) set_text is being applied to
    // the output TextView — the user-edit handlers must not feed it back into
    // the engine. See wire_editor_sync() in MainWindow.cpp.
    bool m_suppress_editor_sync = false;
    // True while a seed_buffer call is in flight — its synchronous event-2
    // push lands inside the call, and speak-on-space must not fire for it
    // (opening a file ending in a space spoke its last word, loop-3).
    bool m_seed_push_in_flight = false;
    void wire_editor_sync();

    // ── RFC 0021 control-mode custom actions (issue #109) ──
    // select/move/delete arrive from the engine as registered-action
    // callbacks (see DasherBridge::set_action_callback) already marshalled
    // to a main-loop idle; dispatched to editor mode (native TextIter
    // measurement over the output pane) or keyboard mode (ydotool chords).
    void on_control_action(const std::string& name, const DasherBridge::ActionAttributes& attrs);
    // Bumped at every dispatch (Windows' _unitActionGeneration pattern): the
    // GTK dispatch is synchronous so a second action cannot interleave
    // mid-application, but the counter documents that contract and fails
    // safe if the path ever becomes async.
    int m_control_action_generation = 0;

    // RFC 0015 clause 8 / RFC 0019 clause 6: caret-move context triggers on
    // direct-mode targets (atspi). Started/stopped with keyboard mode.
    // (Global forward declaration above the class.)
    TargetContextWatcher& target_watcher();

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

    // Game mode target overlay (Windows parity — SyncGameModeState).
    void update_game_target();

    // Keyboard-mode mini bar (like Dasher-Windows' KeyboardMiniBar): added as
    // an overlay on the message overlay (itself a Gtk::Overlay), floating
    // top-right, settings + exit. Visible only while keyboard mode is on.
    Gtk::Box m_keyboard_minibar = Gtk::Box(Gtk::Orientation::HORIZONTAL, 2);
    // Keyboard-mode mini-bar (RFC 0015 clipboard bridge + RFC 0019 New):
    // New + Select All / Copy / Paste (target-side injections) + Settings +
    // Exit. Windows-parity (#56's mini-bar).
    Gtk::Button m_minibar_new_btn;
    Gtk::Button m_minibar_reanchor_btn;
    Gtk::Button m_minibar_selectall_btn;
    Gtk::Button m_minibar_copy_btn;
    Gtk::Button m_minibar_paste_btn;
    Gtk::Button m_minibar_settings_btn;
    Gtk::Button m_minibar_exit_btn;
    // RFC 0012 direct-entry clause (2026-10-30 amendment): compact live-WPM
    // in the mini-bar, current rate only, ~2 Hz, mirroring Windows' mini-bar
    // and Android's IME top bar. GTK #97.
    Gtk::Label m_minibar_wpm_label;

    // ── Issue #110: keyboard-mode UX parity ──
    // Item 1 — the keyboard-mode window is an undecorated dock (no title
    // bar to grab), so the mini-bar grows a drag grip that moves the
    // window with direct X11 configure requests: the WM ignores
    // _NET_WM_MOVERESIZE for dock-typed windows (XFWM4 live report), so
    // gdk_toplevel_begin_move is useless here and the drag is client-side.
    Gtk::Image m_minibar_drag_grip;
    void on_grip_drag_begin(Gtk::EventController& ctrl);
    void on_grip_drag_update(Gtk::EventController& ctrl);
    bool m_window_drag_active = false;
    int m_window_drag_x0 = 0;
    int m_window_drag_y0 = 0;
    double m_drag_begin_surf_x = 0;
    double m_drag_begin_surf_y = 0;
    double m_drag_applied_dx = 0;
    double m_drag_applied_dy = 0;
    // Item 2 — mini-bar parity with Dasher-Windows' KeyboardMiniBar /
    // Apple's directModeMiniBar: the editing keys (backspace + caret
    // arrows, hold to repeat — "for AAC users these are the most-used
    // controls") and Cut. The control-mode toggle is deliberately absent
    // (maintainer decision: settings-only pending RFC 0021's review).
    Gtk::Button m_minibar_backspace_btn;
    Gtk::Button m_minibar_left_btn;
    Gtk::Button m_minibar_right_btn;
    Gtk::Button m_minibar_cut_btn;
    void wire_editing_key(Gtk::Button& btn, const char* telemetry_action, int evdev_code, int repeat_ms);
    // Hold-to-repeat state (Windows' WireEditingKeyRepeat shape): the
    // action fires immediately on press, a 350 ms deliberate-hold delay,
    // then repeat ticks at repeat_ms. Telemetry only on the initial press.
    sigc::connection m_editkey_delay_conn;
    sigc::connection m_editkey_repeat_conn;
    void start_edit_key_repeat(const std::function<void()>& action, int repeat_ms);
    void stop_edit_key_repeat();

    // RFC 0015 parity: keyboard-mode window opacity (persisted frontend-side).
    ui::UiSettings m_ui_settings = ui::UiSettings::load();
    double keyboard_opacity() const;
    void set_keyboard_opacity(double v);

    // Saved window geometry, per mode (issue #74): the window comes back
    // where the user left it. Size restores on all backends; position only
    // under X11 (Wayland compositors place windows themselves).
    void capture_geometry(ui::WindowGeometry& g);
    void save_geometry_for_current_mode();
    void apply_geometry(const ui::WindowGeometry& g, bool live);
    // Slide an off-monitor position onto the nearest live monitor (no-op
    // when the geometry's centre is already on one).
    void clamp_geometry_to_monitor(ui::WindowGeometry& g);
    bool on_close_request() override;

    // Built lazily on first open: the Preferences content queries engine
    // tables (palette list, permitted values) at construction, so it must
    // not exist before the engine realises — and rebuilding it later
    // re-orders the sidebar pages (dynamic pages re-append after the
    // static ones), which is why section order used to shift between
    // launches.
    std::unique_ptr<PreferencesWindow> m_preferences_window;
    PreferencesWindow& preferences_window();

    // Frontend analytics consent; drives the first-run opt-in prompt below.
    analytics::AnalyticsSettings m_analytics = analytics::AnalyticsSettings::load();
    void maybe_show_consent_dialog();
    void capture_app_launched();
};
