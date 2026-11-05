#!/usr/bin/env python3
"""Turns the generated catalog files (catalog(...) calls, parts.back() lines) into static data tables.

A few thousand catalog(...) calls in one function made StandardParts.cpp the slowest file of the build (10 minutes,
more in an Xcode Debug build). The same parts as constant tables compile in seconds, and the core builds them at run
time through addCatalogRows (StandardParts.cpp) exactly as the calls did. fetch_catalog_parts.py writes its output
through `tables()`; run this file to convert a catalog file in place:  python3 tools/catalog_tables.py FILE...
"""
import re
import sys

TOKEN = re.compile(r'\s*(?:("(?:[^"\\]|\\.)*")|(-?\d+(?:\.\d*)?(?:[eE][-+]?\d+)?)|([A-Za-z_][\w:.]*)|(.))', re.S)


def tokens(text):
    pos = 0
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m or m.end() == pos:
            if text[pos:].strip() == "": return
            raise SystemExit(f"cannot read catalog text at: {text[pos:pos + 60]!r}")
        pos = m.end()
        s, n, ident, p = m.groups()
        if s is not None: yield ("str", s)
        elif n is not None: yield ("num", n)
        elif ident is not None: yield ("id", ident)
        elif p is not None and not p.isspace(): yield ("p", p)


def value(toks, i):
    """One value: a literal or a braced list (nested). Returns (value, next index)."""
    kind, v = toks[i]
    if kind == "p" and v == "{":
        items, i = [], i + 1
        while not (toks[i] == ("p", "}")):
            item, i = value(toks, i)
            items.append(item)
            if toks[i] == ("p", ","): i += 1
        return items, i + 1
    return (kind, v), i + 1


def parse(text):
    """[(args of catalog(...), bodyDepth or None, lands or None)] in file order, and the header comment lines."""
    header = [l for l in text.splitlines() if l.startswith("//")]
    body = "\n".join(l for l in text.splitlines() if not l.startswith("//")).replace("parts.back().spec.package.", "PB.")
    toks = list(tokens(body))
    parts, i = [], 0
    while i < len(toks):
        if toks[i] == ("id", "catalog"):
            assert toks[i + 1] == ("p", "(") and toks[i + 2] == ("id", "parts") and toks[i + 3] == ("p", ",")
            i += 4
            args = []
            while toks[i] != ("p", ")"):
                v, i = value(toks, i)
                args.append(v)
                if toks[i] == ("p", ","): i += 1
            i += 1
            assert toks[i] == ("p", ";"), toks[i]
            i += 1
            parts.append([args, None, None])
        elif toks[i][0] == "id" and toks[i][1].startswith("PB."):
            field = toks[i][1].rsplit(".", 1)[1]
            assert toks[i + 1] == ("p", "=")
            v, i = value(toks, i + 2)
            assert toks[i] == ("p", ";")
            i += 1
            if field == "bodyDepth": parts[-1][1] = v
            elif field == "lands": parts[-1][2] = v
            else: raise SystemExit(f"unknown field {field}")
        else:
            raise SystemExit(f"unexpected {toks[i]}")
    return header, parts


def lit(v):
    kind, s = v
    return s


def tables(text):
    """The catalog text (catalog calls) as data tables, the same parts in the same order."""
    header, parts = parse(text)
    pins, lands, rows = [], [], []
    for args, depth, pads in parts:
        cat, name, maker, desc, ref, pkg, n, pitch, body, fp, pinlist = args
        p0 = len(pins)
        for number, pname, ptype in pinlist:
            pins.append("{%s, %s, %s}" % (lit(number), lit(pname), lit(ptype)))
        l0 = len(lands)
        for land in pads or []:
            lands.append("{" + ", ".join(lit(x) for x in land) + "}")
        rows.append("{%s}" % ", ".join([lit(cat), lit(name), lit(maker), lit(desc), lit(ref), lit(pkg), lit(n), lit(pitch),
                                         lit(body), lit(fp), str(p0), str(len(pins) - p0),
                                         lit(depth) if depth is not None else "-1", str(l0), str(len(lands) - l0)]))
    out = header + ["// (Data tables: tools/catalog_tables.py; read by addCatalogRows in StandardParts.cpp.)", "{"]
    out.append("    static const CatalogPin pins[] = {")
    out += ["        %s," % p for p in pins]
    out.append("    };")
    if lands:
        out.append("    static const CatalogLand lands[] = {")
        out += ["        %s," % l for l in lands]
        out.append("    };")
    else:
        out.append("    static const CatalogLand* const lands = nullptr;")
    out.append("    static const CatalogRow rows[] = {")
    out += ["        %s," % r for r in rows]
    out.append("    };")
    out.append("    addCatalogRows(parts, rows, sizeof rows / sizeof rows[0], pins, lands);")
    out.append("}")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    for path in sys.argv[1:]:
        text = open(path, encoding="utf-8").read()
        if "addCatalogRows" in text:
            print(f"{path}: already tables")
            continue
        open(path, "w", encoding="utf-8").write(tables(text))
        print(f"{path}: converted")
