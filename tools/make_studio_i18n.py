#!/usr/bin/env python3
"""Writes Core/studio/i18n.json: the macOS app's interface translations (SiEDA/Resources/<code>.lproj) for every
string SiEDA Studio shows, plus Studio's own strings from Core/studio/i18n_extra.json. --check fails when the file is
out of date; --missing lists Studio strings with no translation."""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, "Core", "studio", "ui.html")
EXTRA = os.path.join(ROOT, "Core", "studio", "i18n_extra.json")
OUT = os.path.join(ROOT, "Core", "studio", "i18n.json")
ENTRY = re.compile(r'^"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)";\s*$')


def unescape(s):
    return s.encode("utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8") if "\\" in s else s


def tables():
    res = os.path.join(ROOT, "SiEDA", "Resources")
    out = {}
    for d in sorted(os.listdir(res)):
        if d.endswith(".lproj") and d != "en.lproj":
            t = {}
            with open(os.path.join(res, d, "Localizable.strings"), encoding="utf-8") as f:
                for line in f:
                    m = ENTRY.match(line.strip())
                    if m:
                        t[unescape(m.group(1))] = unescape(m.group(2))
            out[d[:-6]] = t
    return out


def studio_strings(html):
    """Strings Studio puts on screen: menu labels, tr("…") calls, element text, titles and placeholders."""
    found = set(re.findall(r'\["([^"\]]+?)"(?:,|\])', html))           # MENUS entries ["Label", …] / ["Header"]
    found |= set(re.findall(r'\btr\("([^"]+)"\)', html))
    found |= set(re.findall(r'^ "([^"]+)":\[', html, re.M))                    # MENUS titles
    found |= set(re.findall(r'\b(?:title|placeholder|data-en)="([^"]+)"', html))
    markup = html.split("<script>")[0] + "".join(re.findall(r"<dialog.*?</dialog>", html, re.S))
    found |= {t.strip() for t in re.findall(r'>([^<>{}$`=;|#()]+)<', markup) if t.strip()}
    found |= set(re.findall(r'data-tab="([^"]+)"', html))
    keep = {"SiEDA", "SiEDA Studio", "gh auth login", "BOM", "DRC", "ERC", "3D", "Auto", "Midnight Navy", "Matrix Green", "Graphite"}
    return {s for s in found if re.search(r"[A-Za-z]{2}", s) and len(s) < 300 and s not in keep
            and (s[0].isupper() or " " in s) and not s.startswith(("http", "#")) and "/" not in s.split(" ")[0]}


def build():
    html = open(UI, encoding="utf-8").read().replace("\r\n", "\n")
    wanted = studio_strings(html)
    extra = json.load(open(EXTRA, encoding="utf-8")) if os.path.exists(EXTRA) else {}
    out = {}
    for code, t in tables().items():
        out[code] = {k: v for k, v in sorted(t.items()) if k in wanted and v != k}
        for k, v in extra.get(code, {}).items():
            out[code][k] = v
        out[code] = dict(sorted(out[code].items()))
    return wanted, out


def main():
    wanted, out = build()
    text = json.dumps(out, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n"
    if "--missing" in sys.argv:
        have = {code: set(t) | set(out[code]) for code, t in tables().items()}
        missing = sorted(w for w in wanted if not all(w in h for h in have.values()))
        print("\n".join(missing))
        return 0
    if "--check" in sys.argv:
        current = open(OUT, encoding="utf-8").read().replace("\r\n", "\n") if os.path.exists(OUT) else ""  # a CRLF checkout
        if current != text:
            print("Core/studio/i18n.json is out of date: run python3 tools/make_studio_i18n.py", file=sys.stderr)
            return 1
        return 0
    open(OUT, "w", encoding="utf-8").write(text)
    print(f"{len(out)} languages, {max(len(t) for t in out.values())} strings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
