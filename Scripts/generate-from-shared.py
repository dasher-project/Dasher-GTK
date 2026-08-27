#!/usr/bin/env python3
"""Generate GTK .po files and Android values-XX/ from the shared UI catalogue.

The shared catalogue (shared-resources/ui-strings.json) is the single source
of truth for all Dasher v6 UI translations. Translators edit that file;
this script propagates to each frontend's native format:

  - GTK: po/<locale>.po files (compiled to .mo by CMake at build time)
  - Android: app/src/main/res/values-<locale>/strings.xml

The per-frontend files are BUILD OUTPUTS, not source. Regenerate after
editing the catalogue:

    python3 Scripts/generate-from-shared.py

The extraction (frontend → catalogue) was a one-time bootstrap; from now on
translation edits go into ui-strings.json directly.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GTK = os.path.dirname(HERE)
SHARED = os.path.join(GTK, "shared-resources", "ui-strings.json")
ANDROID_RES = os.path.join(GTK, "..", "Dasher-Android", "app", "src", "main", "res")
PO_DIR = os.path.join(GTK, "po")
LINGUAS = os.path.join(PO_DIR, "LINGUAS")

# Keys the frontends actually use (mapped to their native names).
# GTK msgids are the English text; Android string names are the key.
# Platform-specific keys are tagged so we can skip them per frontend.
GTK_ONLY_KEYS = {
    "keyboard_mode_opacity", "keyboard_mode_title", "keyboard_mode_exit",
    "keyboard_mode_stopped", "keyboard_mode_description",
    "layout_description", "dwell_description",
    "help_about_dasher", "game_target_no_data",
}
ANDROID_ONLY_KEYS = {
    "ime_name", "input_method_title", "input_method_touch",
    "input_method_tilt", "input_method_joystick",
    "toolbar_new_description", "toolbar_open_description",
    "toolbar_save_description", "toolbar_share_description",
    "toolbar_game_description", "toolbar_settings_description",
}


def load_catalogue():
    with open(SHARED) as f:
        data = json.load(f)
    locales = data["meta"]["locales"]
    shared = data["shared"]
    return locales, shared


def po_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def android_escape(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace("'", "\\'"))


def android_locale_dir(locale):
    return "values-" + locale.replace("pt-PT", "pt-rPT").replace("zh-CN", "zh-rCN").replace("_", "-r")


def generate_po(locale, shared):
    """Generate a .po file from the shared catalogue."""
    lines = [
        f"# {locale} translation for dasher-gtk.",
        "# Copyright (C) 2026 The Dasher Project",
        "# This file is distributed under the same license as the dasher-gtk package.",
        "#",
        "# GENERATED from shared-resources/ui-strings.json — do not edit directly.",
        "# Edit the catalogue and run Scripts/generate-from-shared.py.",
        "#",
        'msgid ""',
        'msgstr ""',
        '"Project-Id-Version: dasher-gtk\\n"',
        '"Language: ' + locale + '\\n"',
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        "",
    ]

    count = 0
    seen_msgids = set()  # deduplicate: same English text = same msgid
    for key in sorted(shared.keys()):
        if key in ANDROID_ONLY_KEYS:
            continue

        entry = shared[key]
        en_text = entry.get("en", "")
        if not en_text:
            continue

        # Skip if we already emitted this msgid (gettext keys by English text)
        if en_text in seen_msgids:
            continue
        seen_msgids.add(en_text)

        translated = entry.get(locale, "")
        lines.append(f'msgid "{po_escape(en_text)}"')
        if translated:
            lines.append(f'msgstr "{po_escape(translated)}"')
            count += 1
        else:
            lines.append('msgstr ""')
        lines.append("")

    return "\n".join(lines), count


def generate_android_xml(locale, shared):
    """Generate a values-XX/strings.xml from the shared catalogue."""
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<resources>",
             "    <!-- GENERATED from shared-resources/ui-strings.json — do not edit directly.",
             "         Edit the catalogue and run Scripts/generate-from-shared.py. -->"]

    count = 0
    for key in sorted(shared.keys()):
        if key in GTK_ONLY_KEYS:
            continue

        entry = shared[key]
        en_text = entry.get("en", "")
        if not en_text:
            continue

        # Android resource names must start with a letter and not be Java keywords
        res_name = key if key[0].isalpha() else "s_" + key
        if res_name in ("default", "class", "int", "long", "boolean", "new",
                        "public", "private", "static", "final", "switch",
                        "case", "break", "continue", "return", "void", "if",
                        "else", "for", "while", "do", "try", "catch", "this",
                        "super", "import", "package", "true", "false", "null"):
            res_name = "s_" + res_name

        translated = entry.get(locale, en_text)  # fallback to English
        lines.append(f'    <string name="{res_name}">{android_escape(translated)}</string>')
        if locale in entry:
            count += 1

    lines.append("</resources>")
    return "\n".join(lines) + "\n", count


def main():
    print("Loading shared catalogue...")
    locales, shared = load_catalogue()
    print(f"  {len(shared)} keys, {len(locales)} locales")

    # GTK .po files
    print("\nGTK .po files:")
    linguas = []
    for locale in locales:
        if locale == "en":
            continue
        content, count = generate_po(locale, shared)
        po_path = os.path.join(PO_DIR, f"{locale}.po")
        with open(po_path, "w") as f:
            f.write(content)
        linguas.append(locale)
        print(f"  {locale:6s} {count:3d}/{len(shared)} translated")

    with open(LINGUAS, "w") as f:
        f.write("\n".join(linguas) + "\n")
    print(f"  LINGUAS: {len(linguas)} locales")

    # Android strings.xml files
    print("\nAndroid strings.xml:")
    for locale in locales:
        if locale == "en":
            continue
        content, count = generate_android_xml(locale, shared)
        android_dir = os.path.join(ANDROID_RES, android_locale_dir(locale))
        os.makedirs(android_dir, exist_ok=True)
        xml_path = os.path.join(android_dir, "strings.xml")
        with open(xml_path, "w") as f:
            f.write(content)
        print(f"  {locale:6s} {count:3d}/{len(shared)} translated")

    print("\nDone. Rebuild both frontends to pick up the new files.")


if __name__ == "__main__":
    main()
