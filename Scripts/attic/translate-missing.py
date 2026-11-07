#!/usr/bin/env python3
"""Batch-translate missing GTK .po and Android strings using MyMemory's free
translation API (no key required, 1000 words/day per IP).

Preserves any existing non-empty translations; only fills empty msgstrs and
Android entries that are still English. Rate-limited to be polite.

Usage: python3 Scripts/translate-missing.py
"""
import json
import os
import re
import sys
import time
import urllib.request
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
GTK = os.path.dirname(HERE)
PO_DIR = os.path.join(GTK, "po")
ANDROID_RES = os.path.join(GTK, "..", "Dasher-Android", "app", "src", "main", "res")

LOCALES = [l.strip() for l in open(os.path.join(PO_DIR, "LINGUAS")).read().strip().split("\n") if l.strip()]
LOCALES = [l for l in LOCALES if l != "fr"]  # French is hand-translated

GT_MAP = {"pt-PT": "pt-PT", "zh-CN": "zh-CN"}


def translate(text, target):
    """Translate via Google Cloud Translation API v2 (keyed)."""
    if not text or not text.strip():
        return text
    key = os.environ.get("GOOGLE_TRANSLATE_API_KEY")
    if not key:
        raise RuntimeError("GOOGLE_TRANSLATE_API_KEY not set — export it before running")
    try:
        url = f"https://translation.googleapis.com/language/translate/v2?key={key}"
        payload = json.dumps({
            "q": text,
            "source": "en",
            "target": target,
            "format": "text",
        }).encode()
        req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as r:
            data = json.loads(r.read().decode())
        result = data["data"]["translations"][0]["translatedText"]
        return result if result else None
    except Exception as e:
        print(f"      error: {e}", file=sys.stderr)
        return None


def parse_po_entries(content):
    entries = []
    lines = content.split("\n")
    current_msgid = None
    current_msgstr = None
    in_msgstr = False
    for line in lines:
        m = re.match(r'^msgid "(.*)"$', line)
        if m:
            if current_msgid is not None:
                entries.append((current_msgid, current_msgstr or ""))
            current_msgid = m.group(1)
            current_msgstr = None
            in_msgstr = False
        else:
            m = re.match(r'^msgstr "(.*)"$', line)
            if m:
                current_msgstr = m.group(1)
                in_msgstr = True
            else:
                m = re.match(r'^"(.*)"$', line)
                if m and current_msgid is not None:
                    if in_msgstr and current_msgstr is not None:
                        current_msgstr += m.group(1)
                    elif not in_msgstr:
                        current_msgid += m.group(1)
    if current_msgid is not None:
        entries.append((current_msgid, current_msgstr or ""))
    return entries


def po_unescape(s):
    return s.replace("\\n", "\n").replace('\\"', '"').replace("\\\\", "\\")


def po_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def translate_po_file(locale):
    po_path = os.path.join(PO_DIR, f"{locale}.po")
    if not os.path.exists(po_path):
        return 0, 0
    content = open(po_path).read()
    entries = parse_po_entries(content)
    missing = [(mid, mstr) for mid, mstr in entries if mid and not mstr and mid != ""]
    if not missing:
        return 0, 0

    target = GT_MAP.get(locale, locale)
    translated = 0
    print(f"  {locale}: {len(missing)} strings")

    for msgid, _ in missing:
        english = po_unescape(msgid)
        result = translate(english, target)
        if result and result.strip():
            escaped = po_escape(result.strip())
            old = f'msgid "{msgid}"\nmsgstr ""'
            new = f'msgid "{msgid}"\nmsgstr "{escaped}"'
            content = content.replace(old, new, 1)
            translated += 1
        time.sleep(0.5)

    open(po_path, "w").write(content)
    return translated, len(missing)


def android_escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("'", "\\'")


def translate_android(locale):
    dir_name = "values-" + locale.replace("pt-PT", "pt-rPT").replace("zh-CN", "zh-rCN").replace("_", "-r")
    android_path = os.path.join(ANDROID_RES, dir_name, "strings.xml")
    en_path = os.path.join(ANDROID_RES, "values", "strings.xml")
    if not os.path.exists(android_path):
        return 0, 0

    content = open(android_path).read()
    en_content = open(en_path).read()
    en_strings = dict(re.findall(r'<string name="([^"]+)">(.*?)</string>', en_content, re.DOTALL))
    current = dict(re.findall(r'<string name="([^"]+)">(.*?)</string>', content, re.DOTALL))

    skip = {"app_name", "save_default_name"}
    missing = []
    for key, en_val in en_strings.items():
        if key in skip:
            continue
        if key in current and current[key] == en_val:
            missing.append((key, en_val))

    if not missing:
        return 0, 0

    target = GT_MAP.get(locale, locale)
    translated = 0
    print(f"  {locale}: {len(missing)} strings")

    for key, en_val in missing:
        plain = en_val.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("\\'", "'")
        result = translate(plain, target)
        if result and result.strip():
            escaped = android_escape(result.strip())
            old = f'<string name="{key}">{en_val}</string>'
            new = f'<string name="{key}">{escaped}</string>'
            content = content.replace(old, new, 1)
            translated += 1
        time.sleep(0.5)

    open(android_path, "w").write(content)
    return translated, len(missing)


def main():
    print("GTK .po:")
    ttr = tmis = 0
    for loc in LOCALES:
        tr, miss = translate_po_file(loc)
        ttr += tr
        tmis += miss
    print(f"  total: {ttr}/{tmis}")

    print("\nAndroid:")
    ttr = tmis = 0
    for loc in LOCALES:
        tr, miss = translate_android(loc)
        ttr += tr
        tmis += miss
    print(f"  total: {ttr}/{tmis}")


if __name__ == "__main__":
    main()
