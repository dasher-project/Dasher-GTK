#!/usr/bin/env node
// Batch-translate the remaining GTK .po and Android strings.xml msgstrs
// using jsontt (Google Translate — free, no API key).
//
// Usage: node Scripts/translate-missing.js
//
// Preserves any existing non-empty translations (Apple-derived or hand-written);
// only fills empty msgstrs / untranslated Android entries.

const { jsonTranslator } = require("@parvineyvazov/json-translator");
const fs = require("fs");
const path = require("path");

const GTK = path.resolve(__dirname, "..");
const PO_DIR = path.join(GTK, "po");
const ANDROID_RES = path.join(GTK, "..", "Dasher-Android", "app", "src", "main", "res");
const LINGUAS = fs.readFileSync(path.join(PO_DIR, "LINGUAS"), "utf8").trim().split("\n");

const LOCALES = LINGUAS.filter(l => l !== "fr"); // French already complete

// Google Translate locale codes (mostly same, some need mapping)
const gtLocaleMap = {
  "pt-PT": "pt",
  "zh-CN": "zh-CN",
  // others map directly
};

function gtLocale(code) {
  return gtLocaleMap[code] || code;
}

function parsePo(content) {
  // Parse msgid/msgstr pairs (handles multi-line, escapes)
  const entries = [];
  const lines = content.split("\n");
  let current = null;
  let inMsgstr = false;

  for (const line of lines) {
    let m;
    if ((m = line.match(/^msgid "(.*)"/))) {
      if (current && current.msgid) entries.push(current);
      current = { msgid: m[1], msgstr: "" };
      inMsgstr = false;
    } else if ((m = line.match(/^msgstr "(.*)"/))) {
      current.msgstr = m[1];
      inMsgstr = true;
    } else if ((m = line.match(/^"(.*)"/)) && current) {
      if (inMsgstr) current.msgstr += m[1];
      else current.msgid += m[1];
    }
  }
  if (current && current.msgid) entries.push(current);
  return entries;
}

function poUnescape(s) {
  return s.replace(/\\n/g, "\n").replace(/\\"/g, '"').replace(/\\\\/g, "\\");
}

function poEscape(s) {
  return s.replace(/\\/g, "\\\\").replace(/"/g, '\\"').replace(/\n/g, "\\n");
}

async function main() {
  const translator = await jsonTranslator({ from: "en" });

  for (const locale of LOCALES) {
    const gtCode = gtLocale(locale);
    const poPath = path.join(PO_DIR, `${locale}.po`);
    if (!fs.existsSync(poPath)) {
      console.log(`  ${locale}: no .po file, skipping`);
      continue;
    }

    const content = fs.readFileSync(poPath, "utf8");
    const entries = parsePo(content);
    const missing = entries.filter(e => e.msgid && !e.msgstr && e.msgid !== "");

    if (missing.length === 0) {
      console.log(`  ${locale}: complete`);
      continue;
    }

    console.log(`  ${locale}: translating ${missing.length} strings...`);

    // Translate in batches
    const jsonInput = {};
    missing.forEach((e, i) => { jsonInput[`k${i}`] = poUnescape(e.msgid); });

    try {
      const translated = await translator.translate(jsonInput, "en", gtCode);
      // Write back
      let newContent = content;
      for (let i = 0; i < missing.length; i++) {
        const key = `k${i}`;
        const translated_text = translated[key];
        if (translated_text) {
          const escaped = poEscape(translated_text);
          const oldPair = `msgid "${missing[i].msgid}"\nmsgstr ""`;
          const newPair = `msgid "${missing[i].msgid}"\nmsgstr "${escaped}"`;
          newContent = newContent.replace(oldPair, newPair);
        }
      }
      fs.writeFileSync(poPath, newContent);
      const done = Object.values(translated).filter(v => v).length;
      console.log(`    -> ${done}/${missing.length} translated`);
    } catch (err) {
      console.error(`    -> FAILED: ${err.message}`);
    }

    // Rate-limit to be polite
    await new Promise(r => setTimeout(r, 500));
  }

  // Android strings.xml
  console.log("\nAndroid strings.xml:");
  for (const locale of LOCALES) {
    const dirName = "values-" + locale.replace("pt-PT", "pt-rPT").replace("zh-CN", "zh-rCN").replace("_", "-r");
    const androidPath = path.join(ANDROID_RES, dirName, "strings.xml");
    if (!fs.existsSync(androidPath)) {
      console.log(`  ${locale}: no strings.xml, skipping`);
      continue;
    }

    const content = fs.readFileSync(androidPath, "utf8");
    const enPath = path.join(ANDROID_RES, "values", "strings.xml");
    const enContent = fs.readFileSync(enPath, "utf8");

    // Parse English to get the source strings
    const enStrings = {};
    for (const m of enContent.matchAll(/<string name="([^"]+)">(.*?)<\/string>/gs)) {
      enStrings[m[1]] = m[2];
    }

    // Parse current locale file
    const currentStrings = {};
    for (const m of content.matchAll(/<string name="([^"]+)">(.*?)<\/string>/gs)) {
      currentStrings[m[1]] = m[2];
    }

    // Find untranslated (same as English)
    const missing = [];
    for (const [key, enVal] of Object.entries(enStrings)) {
      if (currentStrings[key] === enVal && enVal !== "Dasher" && enVal !== "dasher.txt") {
        missing.push({ key, value: enVal });
      }
    }

    if (missing.length === 0) {
      console.log(`  ${locale}: complete`);
      continue;
    }

    const gtCode = gtLocale(locale);
    console.log(`  ${locale}: translating ${missing.length} strings...`);

    const jsonInput = {};
    missing.forEach((m, i) => { jsonInput[`k${i}`] = m.value; });

    try {
      const translated = await translator.translate(jsonInput, "en", gtCode);
      let newContent = content;
      for (let i = 0; i < missing.length; i++) {
        const key = `k${i}`;
        const translated_text = translated[key];
        if (translated_text) {
          const escaped = translated_text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/'/g, "\\'");
          const old = `<string name="${missing[i].key}">${missing[i].value}</string>`;
          const neu = `<string name="${missing[i].key}">${escaped}</string>`;
          newContent = newContent.replace(old, neu);
        }
      }
      fs.writeFileSync(androidPath, newContent);
      const done = Object.values(translated).filter(v => v).length;
      console.log(`    -> ${done}/${missing.length} translated`);
    } catch (err) {
      console.error(`    -> FAILED: ${err.message}`);
    }

    await new Promise(r => setTimeout(r, 500));
  }
}

main().then(() => console.log("\nDone.")).catch(err => { console.error(err); process.exit(1); });
