#pragma once

#include <gtkmm/window.h>

// X11 window management for keyboard (direct-entry) mode — restores the v5
// behaviour GTK4 removed (gtk_window_set_accept_focus / set_keep_above /
// stick no longer exist). While Dasher types into other applications it must
// stay visible but never take keyboard focus, or it would type into itself:
//
//   - input hint off : clicks still steer, keyboard focus stays with the
//                      user's document (the window manager skips us on focus)
//   - keep above     : the canvas stays visible over the target app
//   - all desktops   : visible on every workspace, like v5's gtk_window_stick
//
// Compiled only when X11 is available; on Wayland these are no-ops and the
// known limitation (steer without clicking, e.g. gaze/hover drivers) applies —
// see governance RFC 0015, open sub-question 1.
namespace KeyboardWindowX11 {

// Apply all three keyboard-mode window states to the given GTK window's
// surface. Safe to call on any backend: non-X11 surfaces are ignored.
void engage(Gtk::Window& window);

// Restore normal (focused, unraised, per-desktop) window behaviour.
void release(Gtk::Window& window);

// Keyboard-mode window opacity, 0.2–1.0 (matching Dasher-Windows/Apple's
// slider ranges) via EWMH _NET_WM_WINDOW_OPACITY. No-op off X11. Applied
// separately from engage() so the slider can update it live.
void set_opacity(Gtk::Window& window, double opacity);

} // namespace KeyboardWindowX11

// X11 window placement for saved-geometry restore (issue #74): GTK4 removed
// gtk_window_move()/resize(), so restoring a window's position (and live
// per-mode resizing) goes through XLib — the same route the keyboard-mode
// properties use. All functions are no-ops off X11; under Wayland compositors
// place windows themselves and clients cannot override that (by design).
namespace WindowPlacementX11 {

// True and fills x/y with the window's current root coordinates.
bool query(Gtk::Window& window, int& x, int& y);

// Move (and optionally resize, when w/h > 0) a mapped window. Honoured by
// EWMH-compliant window managers as a configure request.
void move_to(Gtk::Window& window, int x, int y, int w = 0, int h = 0);

// Resize a mapped window in place, leaving its position alone.
void resize(Gtk::Window& window, int w, int h);

} // namespace WindowPlacementX11
