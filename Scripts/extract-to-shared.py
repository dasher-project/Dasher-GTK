#!/usr/bin/env python3
"""Extract translations from ALL Dasher frontends into the shared ui-strings.json.

Sources:
  po/*.po                                    (GTK)
  ../Dasher-Android/app/src/main/res/values-XX/strings.xml  (Android)
  ../Dasher-Apple/Localization/Localizable.xcstrings         (Apple)

Writes:
  shared-resources/ui-strings.json  (merged: canonical English + all locales)

Merging strategy: Apple translations are the most mature (123 keys, 32
locales, curated). GTK has our hand-reviewed French and the batch MT set.
Android mirrors GTK. On conflicts, the first source to provide a translation
wins (Apple first, then GTK, then Android fills gaps).
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GTK = os.path.dirname(HERE)
PO_DIR = os.path.join(GTK, "po")
ANDROID_RES = os.path.join(GTK, "..", "Dasher-Android", "app", "src", "main", "res")
APPLE_XCSTRINGS = os.path.join(GTK, "..", "Dasher-Apple", "Localization", "Localizable.xcstrings")
SHARED = os.path.join(GTK, "shared-resources", "ui-strings.json")

LOCALES = [l.strip() for l in open(os.path.join(PO_DIR, "LINGUAS")).read().strip().split("\n") if l.strip()]


def key_from_english(english):
    clean = re.sub(r'<[^>]+>', '', english)
    key = re.sub(r'[^a-z0-9]+', '_', clean.lower()).strip('_')
    return key[:60] if len(key) > 60 else key


def parse_po(filepath):
    if not os.path.exists(filepath):
        return {}
    content = open(filepath).read()
    entries = {}
    msgid = None
    msgstr = None
    in_msgstr = False
    for line in content.split("\n"):
        m = re.match(r'^msgid "(.*)"$', line)
        if m:
            if msgid is not None and msgid and msgstr:
                entries[msgid] = msgstr
            msgid = m.group(1)
            msgstr = None
            in_msgstr = False
        else:
            m = re.match(r'^msgstr "(.*)"$', line)
            if m:
                msgstr = m.group(1)
                in_msgstr = True
            else:
                m = re.match(r'^"(.*)"$', line)
                if m:
                    if in_msgstr and msgstr is not None:
                        msgstr += m.group(1)
                    elif not in_msgstr and msgid is not None:
                        msgid += m.group(1)
    if msgid is not None and msgid and msgstr:
        entries[msgid] = msgstr
    return entries


def po_unescape(s):
    return s.replace("\\n", "\n").replace('\\"', '"').replace("\\\\", "\\")


def parse_android_xml(filepath):
    if not os.path.exists(filepath):
        return {}
    content = open(filepath).read()
    entries = {}
    for m in re.finditer(r'<string name="([^"]+)">(.*?)</string>', content, re.DOTALL):
        value = m.group(2)
        value = value.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("\\'", "'")
        entries[m.group(1)] = value
    return entries


def load_apple():
    """Extract {english: {locale: translated}} from xcstrings."""
    if not os.path.exists(APPLE_XCSTRINGS):
        print("  (no Apple xcstrings found, skipping)")
        return {}
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


def main():
    # Load existing catalogue
    with open(SHARED) as f:
        catalogue = json.load(f)

    # Build translation map: key -> {locale: text}
    translations = {}
    en_to_key = {}  # English text -> catalogue key

    # Seed from existing catalogue (preserves any hand additions)
    for key, val in catalogue.get("shared", {}).items():
        if isinstance(val, str):
            translations[key] = {"en": val}
            en_to_key[val] = key
        elif isinstance(val, dict):
            translations[key] = dict(val)
            if "en" in val:
                en_to_key[val["en"]] = key

    def ensure_key(english):
        """Get or create a catalogue key for this English text."""
        if english in en_to_key:
            return en_to_key[english]
        key = key_from_english(english)
        if key and key not in translations:
            translations[key] = {"en": english}
            en_to_key[english] = key
            return key
        elif key in translations:
            # Key collision with different English text — suffix
            alt = key + "_2"
            while alt in translations:
                alt = alt + "_"
            translations[alt] = {"en": english}
            en_to_key[english] = alt
            return alt
        return None

    def add_translation(english, locale, translated, source):
        if not translated or not translated.strip():
            return 0
        if translated == english:
            return 0  # untranslated fallback
        key = ensure_key(english)
        if not key:
            return 0
        entry = translations.setdefault(key, {})
        if locale not in entry:
            entry[locale] = translated
            return 1
        return 0  # already has a translation from a higher-priority source

    # 1. Apple (highest priority — most mature)
    print("Extracting Apple translations...")
    apple = load_apple()
    apple_count = 0
    for english, locs in apple.items():
        for locale, translated in locs.items():
            if locale in LOCALES or locale == "en":
                apple_count += add_translation(english, locale, translated, "apple")
    print(f"  {len(apple)} English keys, {apple_count} translations added")

    # 2. GTK .po files (French is hand-reviewed)
    print("Extracting GTK translations...")
    gtk_count = 0
    for locale in LOCALES:
        po_file = os.path.join(PO_DIR, f"{locale}.po")
        entries = parse_po(po_file)
        loc_added = 0
        for msgid, msgstr in entries.items():
            english = po_unescape(msgid)
            translated = po_unescape(msgstr)
            loc_added += add_translation(english, locale, translated, "gtk")
        gtk_count += loc_added
        if loc_added:
            print(f"  {locale}: {loc_added} new")
    print(f"  total: {gtk_count} translations added")

    # 3. Android (fills remaining gaps)
    print("Extracting Android translations...")
    android_en = parse_android_xml(os.path.join(ANDROID_RES, "values", "strings.xml"))
    android_count = 0
    for locale in LOCALES:
        dir_name = ("values-" + locale.replace("pt-PT", "pt-rPT")
                    .replace("zh-CN", "zh-rCN").replace("_", "-r"))
        xml_file = os.path.join(ANDROID_RES, dir_name, "strings.xml")
        entries = parse_android_xml(xml_file)
        loc_added = 0
        for name, translated in entries.items():
            en_text = android_en.get(name)
            if not en_text:
                continue
            loc_added += add_translation(en_text, locale, translated, "android")
        android_count += loc_added
    print(f"  total: {android_count} translations added")

    # Write merged catalogue
    output = {
        "_comment": catalogue.get("_comment", "Shared Dasher v6 UI string catalogue (RFC 0003)."),
        "meta": catalogue.get("meta", {"locales": LOCALES}),
        "shared": dict(sorted(translations.items())),
    }

    with open(SHARED, "w") as f:
        json.dump(output, f, indent=2, ensure_ascii=False, sort_keys=True)
        f.write("\n")

    # Report
    total_keys = len(translations)
    print(f"\nCatalogue written: {total_keys} keys")
    print(f"Sources: Apple {apple_count}, GTK {gtk_count}, Android {android_count}")
    print("Coverage by locale:")
    for loc in output["meta"]["locales"]:
        if loc == "en":
            continue
        count = sum(1 for t in translations.values() if loc in t)
        print(f"  {loc:6s} {count:3d}/{total_keys}")


if __name__ == "__main__":
    main()
