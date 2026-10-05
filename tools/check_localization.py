#!/usr/bin/env python3
"""Checks the interface translations in SiEDA/Resources/<language>.lproj/Localizable.strings.

    python3 tools/check_localization.py            # check every language (CI runs this)
    python3 tools/check_localization.py --missing  # also list UI strings in the Swift sources not yet translated

Fails when a language is missing a key that English has (or has extra ones), when a value is empty, or when a
translation drops or adds a format specifier (%@, %lld, …). UI strings in the sources that are not in the tables
yet show in English until translated; --missing lists them (with --strict they fail the check).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESOURCES = os.path.join(ROOT, "SiEDA", "Resources")
LANGUAGES = ["en", "zh-Hant", "ko", "zh-Hans", "ja", "ms", "nl", "de", "vi", "fil",
             "hi", "te", "as", "gu", "kn", "ml", "mr", "or", "pa", "ta"]

ENTRY = re.compile(r'^"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)";\s*$')
FORMAT = re.compile(r"%(?:\d+\$)?(?:lld|ld|d|@|f|lf|\.\d+f|%)")
# SwiftUI views whose first string-literal argument is a LocalizedStringKey.
UI_LITERAL = re.compile(
    r'\b(Text|Button|Label|Toggle|Picker|Section|Menu|TextField|SecureField|GroupBox|Stepper|DisclosureGroup|'
    r'LabeledContent|ProgressView|Link|CommandMenu|navigationTitle|help|alert|confirmationDialog|'
    r'ContentUnavailableView|Slider|ColorPicker|DatePicker|WindowGroup)\s*\(\s*"((?:[^"\\]|\\.)*)"')


def parse(path):
    """Returns {key: value} with the file's escapes kept, and a list of syntax errors."""
    table, errors = {}, []
    in_comment = False
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if in_comment:
                in_comment = "*/" not in line
                continue
            if not line or line.startswith("//"):
                continue
            if line.startswith("/*"):
                in_comment = "*/" not in line
                continue
            m = ENTRY.match(line)
            if not m:
                errors.append(f"{path}:{number}: not a \"key\" = \"value\"; line")
                continue
            if m.group(1) in table:
                errors.append(f"{path}:{number}: duplicate key {m.group(1)!r}")
            table[m.group(1)] = m.group(2)
    return table, errors


def source_keys():
    keys = set()
    for dirpath, _, files in os.walk(os.path.join(ROOT, "SiEDA")):
        for name in files:
            if name.endswith(".swift"):
                with open(os.path.join(dirpath, name), encoding="utf-8") as f:
                    for m in UI_LITERAL.finditer(f.read()):
                        key = m.group(2)
                        if "\\(" not in key and re.search("[A-Za-z]", key):
                            keys.add(key)
    return keys


def main(argv):
    errors = []
    tables = {}
    for lang in LANGUAGES:
        path = os.path.join(RESOURCES, f"{lang}.lproj", "Localizable.strings")
        if not os.path.exists(path):
            errors.append(f"missing {os.path.relpath(path, ROOT)}")
            continue
        tables[lang], parse_errors = parse(path)
        errors += parse_errors
    english = tables.get("en", {})
    for lang, table in tables.items():
        for key in sorted(set(english) - set(table)):
            errors.append(f"{lang}: missing {key!r}")
        for key in sorted(set(table) - set(english)):
            errors.append(f"{lang}: key not in English {key!r}")
        for key, value in table.items():
            if not value.strip():
                errors.append(f"{lang}: empty translation for {key!r}")
            if sorted(FORMAT.findall(value)) != sorted(FORMAT.findall(key)):
                errors.append(f"{lang}: format specifiers differ for {key!r}: {value!r}")

    missing = sorted(source_keys() - set(english))
    if "--missing" in argv or "--strict" in argv:
        for key in missing:
            print(f"untranslated: {key}")
    if "--strict" in argv:
        errors += [f"untranslated UI string {key!r}" for key in missing]

    for e in errors:
        print("error:", e)
    print(f"{len(tables)} languages, {len(english)} strings, {len(missing)} UI strings in the sources not translated yet")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
