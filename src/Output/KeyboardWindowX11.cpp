#include "KeyboardWindowX11.h"

#include <gtkmm/window.h>

// Double gate: our X11 find AND GDK actually built with the X11 backend.
// macOS GTK is Quartz: Homebrew pulls in libx11 transitively, so the plain
// DASHER_HAVE_X11 check passes there but gdk/x11 headers don't exist.
// GDK_WINDOWING_X11 (gdkconfig.h) is only defined for X11 GDK builds.
#if defined(DASHER_HAVE_X11) && defined(GDK_WINDOWING_X11)
#define DASHER_KEYBOARD_X11
#endif

#ifdef DASHER_KEYBOARD_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

namespace {

Display* x11_display_of(Gtk::Window& window) {
    auto surface = window.get_surface();
    if (!surface) return nullptr;
    GdkDisplay* gdk_dpy = surface->get_display()->gobj();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy)) return nullptr;
    return gdk_x11_display_get_xdisplay(gdk_dpy);
}

Window x11_window_of(Gtk::Window& window) {
    auto surface = window.get_surface();
    if (!surface) return None;
    GdkSurface* gdk_surf = surface->gobj();
    if (!GDK_IS_X11_SURFACE(gdk_surf)) return None;
    return gdk_x11_surface_get_xid(gdk_surf);
}

void set_input_focus_hint(Gtk::Window& window, bool accept) {
    // EWMH window-type is the robust lever: a _NET_WM_WINDOW_TYPE_DOCK is
    // never given keyboard focus by EWMH-compliant window managers (XFWM4
    // included), stays on top, and appears on all desktops — v5's three
    // gtk_window_* calls in one property. GTK4 also rewrites WM_HINTS.input
    // behind our backs whenever it manages focus, so a plain WM_HINTS toggle
    // does not survive; the type hint does.
    Display* dpy = x11_display_of(window);
    const Window xid = x11_window_of(window);
    if (!dpy || xid == None) return;

    const Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    const Atom dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    const Atom normal = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);

    if (accept) {
        XChangeProperty(dpy, xid, type_atom, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&normal), 1);
    } else {
        XChangeProperty(dpy, xid, type_atom, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&dock), 1);
    }
    XFlush(dpy);
}

void set_net_state(Gtk::Window& window, const char* atom_name, bool add) {
    // Toggle an EWMH _NET_WM_STATE atom (above / sticky) the same way
    // wmctrl -b add,above does: send a client message to the root window.
    Display* dpy = x11_display_of(window);
    const Window xid = x11_window_of(window);
    if (!dpy || xid == None) return;

    const Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    const Atom target = XInternAtom(dpy, atom_name, False);

    XEvent ev = {};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = xid;
    ev.xclient.message_type = state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = add ? 1 /*_NET_WM_STATE_ADD*/ : 0 /*_NET_WM_STATE_REMOVE*/;
    ev.xclient.data.l[1] = static_cast<long>(target);
    ev.xclient.data.l[2] = 0;

    const Window root = DefaultRootWindow(dpy);
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

} // namespace

namespace KeyboardWindowX11 {

void engage(Gtk::Window& window) {
    set_input_focus_hint(window, /*accept=*/false);
    set_net_state(window, "_NET_WM_STATE_ABOVE", true);
    set_net_state(window, "_NET_WM_STATE_STICKY", true);
}

void release(Gtk::Window& window) {
    set_net_state(window, "_NET_WM_STATE_ABOVE", false);
    set_net_state(window, "_NET_WM_STATE_STICKY", false);
    set_input_focus_hint(window, /*accept=*/true);
    set_opacity(window, 1.0); // fully opaque again
}

void set_opacity(Gtk::Window& window, double opacity) {
    Display* dpy = x11_display_of(window);
    const Window xid = x11_window_of(window);
    if (!dpy || xid == None) return;

    if (opacity < 0.2) opacity = 0.2;
    if (opacity > 1.0) opacity = 1.0;
    // CARDINAL, 0xFFFFFFFF = fully opaque.
    const unsigned long value = static_cast<unsigned long>(opacity * 0xFFFFFFFFUL);
    XChangeProperty(dpy, xid, XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char*>(&value), 1);
    XFlush(dpy);
}

} // namespace KeyboardWindowX11

#else // !DASHER_KEYBOARD_X11

namespace KeyboardWindowX11 {

void engage(Gtk::Window&) {}
void release(Gtk::Window&) {}
void set_opacity(Gtk::Window&, double) {}

} // namespace KeyboardWindowX11

#endif
