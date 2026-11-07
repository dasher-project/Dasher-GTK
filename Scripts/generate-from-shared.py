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

# Catalogue keys whose en form differs from the msgid the code uses
# (gettext keys by English text). Map: msgid -> catalogue key. (Review C2:
# without these, Title Case _() calls in MainWindow.cpp dropped out of the
# po and 64 translated pairs regressed to English.)
MSGID_TO_KEY = {
    "Open Text File": "open_text_file",
    "Save Text File": "save_text_file",
}


def load_catalogue():
    with open(SHARED) as f:
        data = json.load(f)
    locales = data["meta"]["locales"]
    shared = data["shared"]
    return locales, shared


def po_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


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
    reverse = {v: k for k, v in MSGID_TO_KEY.items()}
    for key in sorted(shared.keys()):

        entry = shared[key]
        # Use the code's msgid form when one is mapped (Title Case etc.)
        en_text = reverse.get(key) or entry.get("en", "")
        if not en_text:
            continue

        # Skip if we already emitted this msgid (gettext keys by English text)
        if en_text in seen_msgids:
            continue
        seen_msgids.add(en_text)

        translated = entry.get(locale, "")
        # gettext consistency: a msgid ending in N newlines requires the
        # msgstr to end with the same N (msgfmt fatal-errors otherwise).
        # DEFICIT form — append only the missing ones. (Review C1: the first
        # version computed 'last len(stripped) chars' = mostly the whole
        # English text, and appended it to 11k msgstrs. Never append content;
        # only top up the newline deficit.)
        needed = len(en_text) - len(en_text.rstrip("\n"))
        have = len(translated) - len(translated.rstrip("\n")) if translated else 0
        if translated and needed > have:
            translated += "\n" * (needed - have)
        lines.append(f'msgid "{po_escape(en_text)}"')
        if translated:
            lines.append(f'msgstr "{po_escape(translated)}"')
            count += 1
        else:
            lines.append('msgstr ""')
        lines.append("")

    return "\n".join(lines), count


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

    print("\nDone. (Android generates its own strings now — Dasher-Android#82.)")


if __name__ == "__main__":
    main()
