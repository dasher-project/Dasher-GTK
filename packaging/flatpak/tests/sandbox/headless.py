#!/usr/bin/env python3
"""Start Dasher headless inside the Flatpak sandbox and report what happened.

Run by smoke-test.py inside `flatpak build` (Platform runtime), never on the
host. It needs no display server and no session bus from outside:

  - a private `dbus-daemon --session` is started here, inside the sandbox;
  - `gtk4-broadwayd` (GTK's Broadway backend, part of the GNOME runtime) is
    the display, so nothing is drawn on a real screen.

The app is then started through its launcher, has to take its name on that
bus, stay alive for the requested time, and is stopped again. Everything
observed goes to a JSON file; the judging is done by smoke-test.py.

Usage: headless.py RESULT.json LOG_DIR COMMAND BUS_NAME ALIVE_SECONDS
"""

import json
import os
import re
import signal
import subprocess
import sys
import time

BROADWAY_DISPLAY = ":7"
STARTUP_TIMEOUT = 45  # seconds until the bus name has to be owned
TERM_TIMEOUT = 5  # seconds the app gets to exit after SIGTERM
# A home of its own, so nothing the app writes outlives the sandbox and
# nothing of the user's settings is read.
HOME = "/tmp/dasher-smoke-home"
BUS_DRIVER = ("org.freedesktop.DBus", "/org/freedesktop/DBus")


def gdbus(dest, path, method, *args, timeout=10):
    """Call a method on the session bus; returns gdbus' stdout ('' on error)."""
    cmd = ["gdbus", "call", "--session", "--dest", dest, "--object-path", path, "--method"]
    try:
        done = subprocess.run(
            cmd + [method, *args], capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return ""
    return done.stdout.strip() if done.returncode == 0 else ""


def has_owner(name):
    return gdbus(*BUS_DRIVER, "org.freedesktop.DBus.NameHasOwner", name) == "(true,)"


def owner_of(name):
    out = gdbus(*BUS_DRIVER, "org.freedesktop.DBus.GetNameOwner", name)
    return out[2:-3] if out.startswith("('") else None


def well_known_names():
    out = gdbus(*BUS_DRIVER, "org.freedesktop.DBus.ListNames")
    # "(['org.freedesktop.DBus', ':1.0', ...],)"
    names = [n.strip(" '") for n in out.strip("()[],").split(",")] if out else []
    return [n for n in names if n and not n.startswith(":") and n != "org.freedesktop.DBus"]


def exported_actions(bus_name):
    """The actions the application exports on the bus (org.gtk.Actions on
    its object path): what could be activated from outside, a `quit` for
    example. None when the question got no answer."""
    out = gdbus(bus_name, "/" + bus_name.replace(".", "/"), "org.gtk.Actions.List")
    # "(['quit', 'about'],)" or "(@as [],)"
    return sorted(re.findall(r"'([^']*)'", out)) if "[" in out else None


def wait_until(predicate, timeout, step=0.2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(step)
    return predicate()


def stop_children(children):
    """Kill what is still running, the last started first, so that nothing
    outlives the helper whichever way it ends."""
    for child in reversed(children):
        if child.poll() is None:
            child.kill()
            try:
                child.wait(10)
            except subprocess.TimeoutExpired:
                pass


def observe(result, children, log_dir, command, bus_name, alive):
    """Start the bus, the display and the app, and write what happens into
    `result`. Every process started is appended to `children` at once, for
    main() to stop. Returns early, with result["error"] set, when the bus
    or the display does not come up."""
    env = dict(os.environ)
    env["HOME"] = HOME
    for var, sub in (
        ("XDG_DATA_HOME", "data"),
        ("XDG_CONFIG_HOME", "config"),
        ("XDG_CACHE_HOME", "cache"),
        ("XDG_STATE_HOME", "state"),
    ):
        env[var] = f"{HOME}/{sub}"
        os.makedirs(env[var], exist_ok=True)
    runtime_dir = env.get("XDG_RUNTIME_DIR") or f"{HOME}/run"
    os.makedirs(runtime_dir, exist_ok=True)
    env["XDG_RUNTIME_DIR"] = runtime_dir
    for var in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "AT_SPI_BUS_ADDRESS"):
        env.pop(var, None)

    # ── private session bus ──────────────────────────────────────────────
    result["stage"] = "bus"
    bus = subprocess.Popen(
        ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
        stdout=subprocess.PIPE,
        stderr=open(f"{log_dir}/dbus-daemon.log", "w"),
        env=env,
        text=True,
    )
    children.append(bus)
    address = bus.stdout.readline().strip()
    if not address:
        result["error"] = "dbus-daemon printed no address"
        return
    env["DBUS_SESSION_BUS_ADDRESS"] = address
    os.environ["DBUS_SESSION_BUS_ADDRESS"] = address  # for gdbus() above

    # ── display: GTK's Broadway backend ──────────────────────────────────
    result["stage"] = "display"
    broadway = subprocess.Popen(
        ["gtk4-broadwayd", BROADWAY_DISPLAY],
        stdout=open(f"{log_dir}/broadwayd.log", "w"),
        stderr=subprocess.STDOUT,
        env=env,
    )
    children.append(broadway)
    # Display :N listens on $XDG_RUNTIME_DIR/broadway<N+1>.socket.
    socket_path = f"{runtime_dir}/broadway{int(BROADWAY_DISPLAY[1:]) + 1}.socket"
    if not wait_until(lambda: os.path.exists(socket_path) or broadway.poll() is not None, 15):
        result["error"] = f"gtk4-broadwayd did not create {socket_path}"
        return
    if broadway.poll() is not None:
        result["error"] = f"gtk4-broadwayd exited with status {broadway.returncode}"
        return
    env["GDK_BACKEND"] = "broadway"
    env["BROADWAY_DISPLAY"] = BROADWAY_DISPLAY

    # ── command line: --help, and --version when --help lists it ─────────
    result["stage"] = "help"
    done = subprocess.run([command, "--help"], capture_output=True, text=True, env=env, timeout=60)
    result["help"] = {"status": done.returncode, "stdout": done.stdout, "stderr": done.stderr}
    if "--version" in done.stdout:
        done = subprocess.run(
            [command, "--version"], capture_output=True, text=True, env=env, timeout=60
        )
        result["version"] = {
            "status": done.returncode,
            "stdout": done.stdout,
            "stderr": done.stderr,
        }

    # ── the app itself ───────────────────────────────────────────────────
    result["stage"] = "start"
    started = time.monotonic()
    app = subprocess.Popen(
        [command], stdout=open(f"{log_dir}/app.log", "w"), stderr=subprocess.STDOUT, env=env
    )
    children.append(app)
    owned = wait_until(lambda: has_owner(bus_name) or app.poll() is not None, STARTUP_TIMEOUT)
    result["name_owned"] = bool(owned and has_owner(bus_name))
    result["seconds_to_name"] = round(time.monotonic() - started, 2)
    # Which well-known names does the app's connection hold? More than
    # the expected one means the manifest needs another --own-name.
    connection = owner_of(bus_name)
    result["names_owned"] = sorted(
        n for n in well_known_names() if connection and owner_of(n) == connection
    )
    result["names_on_bus"] = sorted(well_known_names())

    result["stage"] = "alive"
    end = time.monotonic() + alive
    while time.monotonic() < end and app.poll() is None:
        time.sleep(0.2)
    result["alive_seconds"] = round(time.monotonic() - started, 2)
    result["exited_early"] = app.poll() is not None
    if app.poll() is not None:
        result["exit_status"] = app.returncode
    result["name_owned_at_end"] = has_owner(bus_name)
    result["actions"] = exported_actions(bus_name)

    # ── shutdown ─────────────────────────────────────────────────────────
    result["stage"] = "shutdown"
    if app.poll() is None:
        app.send_signal(signal.SIGTERM)
        try:
            app.wait(TERM_TIMEOUT)
            result["exited_on_sigterm"] = True
        except subprocess.TimeoutExpired:
            result["exited_on_sigterm"] = False
            app.kill()
            app.wait(10)
        result["exit_status"] = app.returncode
    result["name_released"] = not has_owner(bus_name)
    # The crash reporter writes this file from its signal handlers.
    crash = f"{env['XDG_DATA_HOME']}/dasher/pending_crash.txt"
    if os.path.exists(crash):
        with open(crash, errors="replace") as f:
            result["pending_crash"] = f.read(4000)
    result["stage"] = "done"


def main(argv=None):
    result_path, log_dir, command, bus_name, alive = (sys.argv[1:] if argv is None else argv)[:5]
    result = {"stage": "setup"}
    children = []
    try:
        observe(result, children, log_dir, command, bus_name, float(alive))
    except Exception as e:  # report, never hang the sandbox
        result["error"] = f"{type(e).__name__}: {e}"
    finally:
        # Also on SystemExit and KeyboardInterrupt: no child is left behind,
        # and the result says how far the start got.
        stop_children(children)
        with open(result_path, "w") as f:
            json.dump(result, f, indent=1)


if __name__ == "__main__":
    main()
