#!/usr/bin/env python3
"""Generate po/ and values-XX/ translation files for Dasher-GTK and Dasher-Android.

Sources English strings from the .pot (GTK) and values/strings.xml (Android),
and seeds translations from Dasher-Apple's Localizable.xcstrings where the
English keys match. Locales with no Apple match get scaffold files with
untranslated (empty) msgstrs — ready for a translator or a batch MT pass.

Usage:
    python3 Scripts/gen-translations.py

Creates:
    po/<locale>.po          (GTK, via LINGUAS)
    ../Dasher-Android/app/src/main/res/values-<locale>/strings.xml
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GTK = os.path.dirname(HERE)
APPLE_XCSTRINGS = os.path.join(GTK, "..", "Dasher-Apple", "Localization", "Localizable.xcstrings")
ANDROID_RES = os.path.join(GTK, "..", "Dasher-Android", "app", "src", "main", "res")
PO_DIR = os.path.join(GTK, "po")
LINGUAS = os.path.join(PO_DIR, "LINGUAS")

# DasherCore's canonical 33 locales (RFC 0003)
ALL_LOCALES = [
    "af", "ar", "bn", "cs", "da", "de", "el", "es", "fa", "fi", "fr",
    "gu", "hi", "hu", "it", "kn", "ml", "mr", "nl", "pa", "pl", "pt",
    "pt-PT", "ru", "sv", "sw", "ta", "te", "th", "ur", "zh-CN", "zu",
]

def load_apple_translations():
    """Extract {english_string: {locale: translated_string}} from xcstrings."""
    with open(APPLE_XCSTRINGS) as f:
        data = json.load(f)
    result = {}
    for key, info in data.get("strings", {}).items():
        locs = {}
        for loc, details in info.get("localizations", {}).items():
            unit = details.get("stringUnit", {})
            if unit.get("state") == "translated":
                locs[loc] = unit.get("value", "")
        if locs:
            result[key] = locs
    return result

def extract_gtk_strings():
    """Run xgettext to get the English msgids from the .pot."""
    pot = os.path.join(PO_DIR, "dasher-gtk.pot")
    subprocess.run([
        "xgettext", "--files-from=" + os.path.join(PO_DIR, "POTFILES.in"),
        "--directory=" + GTK, "--output=" + pot,
        "--keyword=_", "--from-code=UTF-8", "--force-po",
        "--package-name=dasher-gtk", "--package-version=0.2.9",
    ], check=True, capture_output=True)

    msgids = []
    with open(pot) as f:
        for line in f:
            m = re.match(r'^msgid "(.*)"$', line)
            if m and m.group(1) and m.group(1) != "":
                msgids.append(m.group(1))
    return msgids

def extract_android_strings():
    """Parse English strings.xml into {key: value}."""
    path = os.path.join(ANDROID_RES, "values", "strings.xml")
    with open(path) as f:
        content = f.read()
    result = {}
    for m in re.finditer(r'<string name="([^"]+)">(.*?)</string>', content, re.DOTALL):
        result[m.group(1)] = m.group(2)
    return result

def find_apple_match(english, apple):
    """Try to find an Apple translation for this English string."""
    if english in apple:
        return apple[english]
    # Try stripped/lowercase for fuzzy matches
    stripped = english.strip()
    if stripped in apple:
        return apple[stripped]
    return None

def po_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")

def generate_po(locale, msgids, apple):
    """Generate a .po file for this locale."""
    lines = [
        f"# {locale} translation for dasher-gtk.",
        "# Copyright (C) 2026 The Dasher Project",
        "# This file is distributed under the same license as the dasher-gtk package.",
        "#",
        'msgid ""',
        'msgstr ""',
        '"Project-Id-Version: dasher-gtk 0.2.9\\n"',
        '"Language: ' + locale + '\\n"',
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        "",
    ]
    translated = 0
    for msgid in msgids:
        match = find_apple_match(msgid, apple)
        lines.append(f'msgid "{po_escape(msgid)}"')
        if match and locale in match:
            lines.append(f'msgstr "{po_escape(match[locale])}"')
            translated += 1
        else:
            lines.append('msgstr ""')
        lines.append("")
    return "\n".join(lines), translated, len(msgids)

def android_escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("'", "\\'")

def android_locale_dir(locale):
    """Convert locale code to Android resource dir name."""
    return "values-" + locale.replace("_", "-r").replace("pt-PT", "pt-rPT").replace("zh-CN", "zh-rCN")

def generate_android_xml(locale, strings, apple):
    """Generate a values-XX/strings.xml for this locale."""
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<resources>"]
    translated = 0
    for key, value in strings.items():
        match = find_apple_match(value, apple)
        if match and locale in match:
            lines.append(f'    <string name="{key}">{android_escape(match[locale])}</string>')
            translated += 1
        else:
            # Untranslated: use the English value (Android falls back
            # automatically, but having the entry makes the file valid)
            lines.append(f'    <string name="{key}">{android_escape(value)}</string>')
    lines.append("</resources>")
    return "\n".join(lines) + "\n", translated, len(strings)

def main():
    print("Loading Apple translations...")
    apple = load_apple_translations()
    print(f"  {len(apple)} translatable keys, {len(ALL_LOCALES)} locales")

    print("Extracting GTK strings...")
    msgids = extract_gtk_strings()
    print(f"  {len(msgids)} msgids")

    print("Extracting Android strings...")
    android_strings = extract_android_strings()
    print(f"  {len(android_strings)} string resources")

    # Generate .po files
    linguas = []
    for locale in ALL_LOCALES:
        if locale == "en":
            continue  # English is the source language

        # GTK .po
        po_content, tr, total = generate_po(locale, msgids, apple)
        po_path = os.path.join(PO_DIR, f"{locale}.po")
        with open(po_path, "w") as f:
            f.write(po_content)
        linguas.append(locale)

        # Android strings.xml
        xml_content, atr, atotal = generate_android_xml(locale, android_strings, apple)
        android_dir = os.path.join(ANDROID_RES, android_locale_dir(locale))
        os.makedirs(android_dir, exist_ok=True)
        android_path = os.path.join(android_dir, "strings.xml")
        with open(android_path, "w") as f:
            f.write(xml_content)

        pct = 100 * tr // total if total else 0
        apct = 100 * atr // atotal if atotal else 0
        print(f"  {locale:6s} GTK {tr:3d}/{total} ({pct:2d}%)  Android {atr:3d}/{atotal} ({apct:2d}%)")

    with open(LINGUAS, "w") as f:
        f.write("\n".join(linguas) + "\n")
    print(f"\nLINGUAS updated: {len(linguas)} locales")

if __name__ == "__main__":
    main()
