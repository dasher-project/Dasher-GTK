#!/usr/bin/env python3
"""Generate po/ and values-XX/ translation files from the shared UI catalogue.

Consumes shared-resources/ui-strings.json (the dasher-shared-resources git
submodule) as the canonical English source, and seeds translations from
Dasher-Apple's Localizable.xcstrings where the English keys match. Locales
with no Apple match get scaffold files ready for a batch MT pass.

Also regenerates the Android values-XX/ files from the same catalogue.

Usage:
    python3 Scripts/gen-translations.py
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GTK = os.path.dirname(HERE)
SHARED = os.path.join(GTK, "shared-resources", "ui-strings.json")
APPLE_XCSTRINGS = os.path.join(GTK, "..", "Dasher-Apple", "Localization", "Localizable.xcstrings")
ANDROID_RES = os.path.join(GTK, "..", "Dasher-Android", "app", "src", "main", "res")
PO_DIR = os.path.join(GTK, "po")
LINGUAS = os.path.join(PO_DIR, "LINGUAS")

def load_shared_catalogue():
    with open(SHARED) as f:
        data = json.load(f)
    return data["meta"]["locales"], data["shared"]

def load_apple_translations():
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

def find_apple_match(english, apple):
    if english in apple:
        return apple[english]
    stripped = english.strip()
    if stripped in apple:
        return apple[stripped]
    return None

def po_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")

def generate_po(locale, msgids, apple):
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
    return "values-" + locale.replace("_", "-r").replace("pt-PT", "pt-rPT").replace("zh-CN", "zh-rCN")

def generate_android_xml(locale, strings, apple):
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<resources>"]
    translated = 0
    for key, value in strings.items():
        match = find_apple_match(value, apple)
        if match and locale in match:
            lines.append(f'    <string name="{key}">{android_escape(match[locale])}</string>')
            translated += 1
        else:
            lines.append(f'    <string name="{key}">{android_escape(value)}</string>')
    lines.append("</resources>")
    return "\n".join(lines) + "\n", translated, len(strings)

def main():
    print("Loading shared catalogue...")
    locales, shared_strings = load_shared_catalogue()
    print(f"  {len(shared_strings)} canonical strings, {len(locales)} locales")

    print("Loading Apple translations...")
    apple = load_apple_translations()
    print(f"  {len(apple)} translatable keys")

    # GTK: extract actual source strings via xgettext, then match against catalogue
    print("Extracting GTK strings...")
    pot = os.path.join(PO_DIR, "dasher-gtk.pot")
    subprocess.run([
        "xgettext", "--files-from=" + os.path.join(PO_DIR, "POTFILES.in"),
        "--directory=" + GTK, "--output=" + pot,
        "--keyword=_", "--from-code=UTF-8", "--force-po",
    ], check=True, capture_output=True)
    msgids = []
    with open(pot) as f:
        for line in f:
            m = re.match(r'^msgid "(.*)"$', line)
            if m and m.group(1) and m.group(1) != "":
                msgids.append(m.group(1))
    print(f"  {len(msgids)} msgids from source")

    # Android: parse English strings.xml
    print("Extracting Android strings...")
    en_path = os.path.join(ANDROID_RES, "values", "strings.xml")
    with open(en_path) as f:
        en_content = f.read()
    android_strings = dict(re.findall(r'<string name="([^"]+)">(.*?)</string>', en_content, re.DOTALL))
    print(f"  {len(android_strings)} string resources")

    linguas = []
    for locale in locales:
        if locale == "en":
            continue
        # GTK .po
        po_content, tr, total = generate_po(locale, msgids, apple)
        with open(os.path.join(PO_DIR, f"{locale}.po"), "w") as f:
            f.write(po_content)
        linguas.append(locale)

        # Android strings.xml
        xml_content, atr, atotal = generate_android_xml(locale, android_strings, apple)
        android_dir = os.path.join(ANDROID_RES, android_locale_dir(locale))
        os.makedirs(android_dir, exist_ok=True)
        with open(os.path.join(android_dir, "strings.xml"), "w") as f:
            f.write(xml_content)

        pct = 100 * tr // total if total else 0
        print(f"  {locale:6s} GTK {tr:3d}/{total} ({pct:2d}%)  Android {atr:3d}/{atotal}")

    with open(LINGUAS, "w") as f:
        f.write("\n".join(linguas) + "\n")
    print(f"\nLINGUAS: {len(linguas)} locales")

if __name__ == "__main__":
    main()
