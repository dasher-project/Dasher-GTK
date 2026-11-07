#!/usr/bin/env python3
"""GTK#98 review I3: content assertion for the generated po files.

The freshness gate (git diff --exit-code) only proves the committed files
REGENERATE identically — reproducible garbage passes it (review C1: the
corrupted 11k-msgstr regeneration sailed through). This script checks
CONTENT: every translated msgstr must equal the catalogue's translation for
that locale, modulo the msgfmt newline top-up the generator applies.

Exits 1 on any mismatch. Run after Scripts/generate-from-shared.py.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SHARED = os.path.join(REPO, "shared-resources", "ui-strings.json")
PO_DIR = os.path.join(REPO, "po")

# Catalogue keys whose en form differs from the code's msgid (mirrors the
# generator's MSGID_TO_KEY).
MSGID_TO_KEY = {
    "Open Text File": "open_text_file",
    "Save Text File": "save_text_file",
}


def po_unescape(s):
    return s.replace('\\"', '"').replace("\\\\", "\\").replace("\\n", "\n")


def main():
    shared = json.load(open(SHARED))["shared"]
    # en-text -> entry (the generator keys msgids by English text)
    # Mirror the GENERATOR's key selection exactly: sorted key order,
    # first-wins dedup for duplicate en texts (seen_msgids in the generator),
    # then the msgid-form overrides. A checker that picks a different key for
    # duplicate-text msgids than the generator did is a false-positive
    # machine (107 of them, at first run).
    by_en = {}
    seen = set()
    for key in sorted(shared.keys()):
        entry = shared[key]
        en = entry.get("en", "")
        if not en or en in seen:
            continue
        seen.add(en)
        by_en[en] = (key, entry)
    for msgid, key in MSGID_TO_KEY.items():
        if key in shared:
            by_en[msgid] = (key, shared[key])

    mismatches = []
    checked = 0
    for locale in open(os.path.join(PO_DIR, "LINGUAS")).read().split():
        po_path = os.path.join(PO_DIR, f"{locale}.po")
        po = open(po_path).read()
        for m in re.finditer(r'msgid "((?:[^"\\]|\\.)*)"\nmsgstr "((?:[^"\\]|\\.)*)"', po):
            msgid, msgstr = po_unescape(m.group(1)), po_unescape(m.group(2))
            hit = by_en.get(msgid)
            if not hit or not msgstr:
                continue  # untranslated or app-side msgid
            _key, entry = hit
            want = entry.get(locale) or entry.get("en", "")
            # The generator tops up trailing newlines to match the msgid;
            # compare on the newline-stripped tails.
            if msgstr.rstrip("\n") != want.rstrip("\n"):
                mismatches.append(f"{locale}: {msgid[:40]!r} -> {msgstr[:40]!r} (catalogue: {want[:40]!r})")
            checked += 1

    print(f"{checked} translated entries checked against the catalogue")
    if mismatches:
        for e in mismatches[:10]:
            print(" ", e)
        print(f"FAIL: {len(mismatches)} msgstr(s) differ from the catalogue")
        sys.exit(1)
    print("content OK: every msgstr matches the catalogue")


if __name__ == "__main__":
    main()
