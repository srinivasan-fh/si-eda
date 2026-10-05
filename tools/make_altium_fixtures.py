#!/usr/bin/env python3
"""Writes the synthetic Altium test libraries Core/tests/fixtures/altium/Test.SchLib and Test.PcbLib.

They are built here, to the published formats, because no Altium-made library can be redistributed with the tests:
  * the container is an OLE compound file (MS-CFB, version 3: 512-byte sectors, a mini stream for streams under
    4096 bytes, FAT / mini FAT chains, a directory tree), written by the small writer below;
  * a SchLib holds one storage per component with a "Data" stream of records: [u32 length | type << 24] then
    either "|KEY=VALUE|…" text (type 0) or a binary pin record (type 1);
  * a PcbLib holds one storage per footprint with "Data" (a name subrecord, then primitives: a type byte followed by
    u32-length subrecords; pads have six), "Parameters" and "Header" streams.
The record layouts are the reverse-engineered ones KiCad's Altium importer reads. When `olefile` is installed the
containers are checked with it as an independent CFB reader.

Run from the repository root:  python3 tools/make_altium_fixtures.py
"""
import os
import struct

OUT = os.path.join("Core", "tests", "fixtures", "altium")
FREESECT, ENDOFCHAIN, FATSECT, NOSTREAM = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD, 0xFFFFFFFF


# ---------------------------------------------------------------- compound file writer (MS-CFB v3)

def cfb(tree):
    """tree: {name: bytes (stream) | dict (storage)} → the compound file's bytes."""
    entries = [{"name": "Root Entry", "type": 5, "data": b"", "kids": []}]

    def add(node, parent):
        names = sorted(node, key=lambda n: (len(n), n.upper()))  # CFB sibling order
        for name in names:
            value = node[name]
            e = {"name": name, "type": 1 if isinstance(value, dict) else 2,
                 "data": b"" if isinstance(value, dict) else value, "kids": []}
            entries.append(e)
            parent["kids"].append(len(entries) - 1)
            if isinstance(value, dict):
                add(value, e)

    add(tree, entries[0])
    for e in entries:  # children as a right-leaning chain (a valid binary search tree in sibling order)
        e["child"] = e["kids"][0] if e["kids"] else NOSTREAM
        e["left"] = NOSTREAM
        e["right"] = NOSTREAM
    for e in entries:
        for a, b in zip(e["kids"], e["kids"][1:]):
            entries[a]["right"] = b

    # Mini stream for small streams.
    mini, minifat = bytearray(), []
    for e in entries:
        if e["type"] == 2 and 0 < len(e["data"]) < 4096:
            n = (len(e["data"]) + 63) // 64
            e["start"] = len(mini) // 64
            minifat += [e["start"] + k + 1 for k in range(n - 1)] + [ENDOFCHAIN]
            mini += e["data"] + b"\0" * (n * 64 - len(e["data"]))
        elif e["type"] == 2 and len(e["data"]) == 0:
            e["start"] = ENDOFCHAIN
    sectors = []  # (owner, bytes) in file order
    fat = []

    def place(data):
        n = (len(data) + 511) // 512
        first = len(sectors)
        for k in range(n):
            sectors.append(data[k * 512:(k + 1) * 512].ljust(512, b"\0"))
            fat.append(first + k + 1 if k < n - 1 else ENDOFCHAIN)
        return first if n else ENDOFCHAIN

    # Directory.
    dir_bytes = bytearray()
    for e in entries:
        dir_bytes += b"\0" * 128  # filled below once the sectors are known
    while len(dir_bytes) % 512:
        dir_bytes += b"\0" * 128
    first_dir = place(bytes(dir_bytes))
    mf = b"".join(struct.pack("<I", v) for v in minifat)
    mf += b"\xff" * ((-len(mf)) % 512)
    first_minifat = place(mf) if minifat else ENDOFCHAIN
    root_start = place(bytes(mini)) if mini else ENDOFCHAIN
    entries[0]["start"], entries[0]["size"] = root_start, len(mini)
    for e in entries:
        if e["type"] == 2 and len(e["data"]) >= 4096:
            e["start"] = place(e["data"])
        if e["type"] == 1:
            e["start"] = 0
        e.setdefault("size", len(e["data"]))
    # FAT sectors (they hold their own entries).
    nfat = 1
    while (len(sectors) + nfat) > nfat * 128:
        nfat += 1
    fat_ids = list(range(len(sectors), len(sectors) + nfat))
    fat += [FATSECT] * nfat
    fat += [FREESECT] * (nfat * 128 - len(fat))
    fat_bytes = b"".join(struct.pack("<I", v) for v in fat)
    for k in range(nfat):
        sectors.append(fat_bytes[k * 512:(k + 1) * 512])
    # Directory entries.
    d = bytearray()
    for e in entries:
        name = e["name"].encode("utf-16-le")
        rec = name + b"\0\0"
        rec = rec.ljust(64, b"\0")
        rec += struct.pack("<HBB", len(name) + 2, e["type"], 1)
        rec += struct.pack("<III", e["left"], e["right"], e["child"])
        rec += b"\0" * 16 + b"\0" * 4 + b"\0" * 16
        rec += struct.pack("<II", e.get("start", 0) if e["type"] != 1 else 0, e["size"] if e["type"] != 1 else 0)
        rec += b"\0" * 4
        assert len(rec) == 128
        d += rec
    while len(d) % 512:
        empty = b"\0" * 64 + struct.pack("<HBB", 0, 0, 0) + struct.pack("<III", NOSTREAM, NOSTREAM, NOSTREAM) + b"\0" * 48
        d += empty
    for k in range(len(d) // 512):
        sectors[first_dir + k] = bytes(d[k * 512:(k + 1) * 512])
    header = b"\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1" + b"\0" * 16
    header += struct.pack("<HHHHH", 0x3E, 3, 0xFFFE, 9, 6) + b"\0" * 6
    header += struct.pack("<IIIIIIIII", 0, nfat, first_dir, 0, 4096, first_minifat, (len(mf) // 512) if minifat else 0,
                          ENDOFCHAIN, 0)
    header += b"".join(struct.pack("<I", fat_ids[i] if i < nfat else FREESECT) for i in range(109))
    assert len(header) == 512
    return header + b"".join(sectors)


# ---------------------------------------------------------------- Altium schematic records

def text_record(pairs):
    body = ("|" + "|".join(f"{k}={v}" for k, v in pairs)).encode("cp1252") + b"\0"
    return struct.pack("<I", len(body)) + body


def utf8_record(pairs):
    parts = []
    for k, v in pairs:
        if k.startswith("%UTF8%"):
            parts.append(k.encode("ascii") + b"=" + v.encode("utf-8"))
        else:
            parts.append(k.encode("ascii") + b"=" + v.encode("cp1252", "replace"))
    body = b"|" + b"|".join(parts) + b"\0"
    return struct.pack("<I", len(body)) + body


def pascal(s):
    b = s.encode("cp1252")
    return struct.pack("<B", len(b)) + b


def binary_pin(part, designator, name, electrical, x, y, length, orientation, hidden=False, mode=0):
    conglomerate = (orientation & 3) | (0x04 if hidden else 0) | 0x08 | 0x10
    body = struct.pack("<iBhB", 2, 0, part, mode) + b"\0" * 4 + pascal("") + b"\0"
    body += struct.pack("<BBhhhi", electrical, conglomerate, length, x, y, 0x000080)
    body += pascal(name) + pascal(designator) + pascal("") + b"\0" * 3
    return struct.pack("<I", len(body) | (1 << 24)) + body


def text_pin(part, designator, name, electrical, x, y, length, orientation):
    return text_record([("RECORD", 2), ("OWNERINDEX", 0), ("OWNERPARTID", part), ("ELECTRICAL", electrical),
                        ("PINCONGLOMERATE", orientation | 0x18), ("PINLENGTH", length), ("LOCATION.X", x),
                        ("LOCATION.Y", y), ("NAME", name), ("DESIGNATOR", designator)])


def schlib():
    lm358 = (
        text_record([("RECORD", 1), ("LIBREFERENCE", "LM358"), ("COMPONENTDESCRIPTION", "Dual op-amp, ±16 V"),
                     ("PARTCOUNT", 4), ("DISPLAYMODECOUNT", 2), ("OWNERPARTID", -1), ("CURRENTPARTID", 1)])
        # Part 1 (binary pins): output right, inputs left.
        + binary_pin(1, "1", "OUTA", 2, 40, 0, 20, 0)
        + binary_pin(1, "2", "-INA", 0, -40, 10, 20, 2)
        + binary_pin(1, "3", "+INA", 0, -40, -10, 20, 2)
        + binary_pin(1, "1", "OUTA", 2, 40, 0, 20, 0, mode=1)  # De Morgan alternate: skipped
        # Part 2 (text pins).
        + text_pin(2, "5", "+INB", 0, -40, -10, 20, 2)
        + text_pin(2, "6", "-INB", 0, -40, 10, 20, 2)
        + text_pin(2, "7", "OUTB", 2, 40, 0, 20, 0)
        # Part 3: the supplies, up and down.
        + binary_pin(3, "8", "V+", 7, 0, 20, 20, 1)
        + binary_pin(3, "4", "V-", 7, 0, -20, 20, 3)
        + text_record([("RECORD", 34), ("OWNERINDEX", 0), ("TEXT", "U?"), ("NAME", "Designator")])
        + text_record([("RECORD", 41), ("OWNERINDEX", 0), ("NAME", "Manufacturer"), ("TEXT", "Texas Instruments")])
        + text_record([("RECORD", 41), ("OWNERINDEX", 0), ("NAME", "Datasheet"),
                       ("TEXT", "https://www.ti.com/lit/ds/symlink/lm358.pdf")])
        + text_record([("RECORD", 45), ("OWNERINDEX", 0), ("MODELNAME", "SO8_OLD"), ("MODELTYPE", "PCBLIB"),
                       ("ISCURRENT", "F")])
        + text_record([("RECORD", 45), ("OWNERINDEX", 0), ("MODELNAME", "SOIC8_TI"), ("MODELTYPE", "PCBLIB"),
                       ("ISCURRENT", "T")])
        + text_record([("RECORD", 46), ("OWNERINDEX", 0)])
    )
    ne555 = (
        utf8_record([("RECORD", "1"), ("LIBREFERENCE", "NE555"), ("COMPONENTDESCRIPTION", "Timer"),
                     ("%UTF8%COMPONENTDESCRIPTION", "Precision timer, 4.5–16 V"), ("PARTCOUNT", "2")])
        + text_pin(1, "1", "GND", 7, 0, -40, 20, 3)
        + text_pin(1, "2", "TRIG", 0, -50, 10, 20, 2)
        + text_pin(1, "3", "OUT", 2, 50, 0, 20, 0)
        + text_pin(1, "4", "R\\E\\S\\E\\T\\", 0, -50, -10, 20, 2)
        + text_pin(1, "5", "CV", 0, -50, -20, 20, 2)
        + text_pin(1, "6", "THR", 0, -50, 0, 20, 2)
        + text_pin(1, "7", "DIS", 3, 50, 10, 20, 0)
        + text_pin(1, "8", "VCC", 7, 0, 40, 20, 1)
        + text_record([("RECORD", 34), ("TEXT", "U?")])
        + text_record([("RECORD", 45), ("MODELNAME", "DIP8"), ("MODELTYPE", "PCBLIB"), ("ISCURRENT", "T")])
    )
    header = text_record([("HEADER", "Protel for Windows - Schematic Library Editor Binary File Version 5.0"),
                          ("WEIGHT", 40), ("MINORVERSION", 2), ("COMPCOUNT", 2), ("LIBREF0", "LM358"),
                          ("LIBREF1", "NE555")])
    big = b"\0" * 5000  # a regular-sector stream (over the 4096-byte mini stream cutoff)
    return cfb({"FileHeader": header, "Storage": struct.pack("<I", 0), "LM358": {"Data": lm358},
                "NE555": {"Data": ne555}, "Padding": {"Data": struct.pack("<I", 0), "Blob": big}})


# ---------------------------------------------------------------- Altium PCB primitives

UNIT = 1 / 2.54e-6  # internal units (1/10000 mil) per mm


def u(mm):
    return int(round(mm * UNIT))


def sub(data):
    return struct.pack("<I", len(data)) + data


def pad(name, x, y, w, h, hole=0.0, shape=2, layer=1, rotation=0.0, plated=True):
    s5 = struct.pack("<BBBHHH", layer, 0x04, 0, 0, 0, 0xFFFF) + b"\0" * 4
    s5 += struct.pack("<iiiiiiiii", u(x), u(y), u(w), u(h), u(w), u(h), u(w), u(h), u(hole))
    s5 += struct.pack("<BBB", shape, shape, shape) + struct.pack("<d", rotation)
    s5 += struct.pack("<BBB", 1 if plated else 0, 0, 0) + b"\0" * 23 + struct.pack("<ii", 0, 0) + b"\0" * 7
    s5 += struct.pack("<BB", 0, 0) + b"\0" * 3 + struct.pack("<d", 0.0) + b"\0" * 6
    assert len(s5) >= 110
    return b"\x02" + sub(pascal(name)) + sub(b"") + sub(b"") + sub(b"") + sub(s5) + sub(b"")


def track(x0, y0, x1, y1, layer=33, width=0.15):
    t = struct.pack("<BBBHHH", layer, 0, 0, 0xFFFF, 0xFFFF, 0) + b"\0" * 4
    t += struct.pack("<iiiii", u(x0), u(y0), u(x1), u(y1), u(width)) + b"\0" * 12
    return b"\x04" + sub(t)


def footprint(name, primitives, description):
    data = sub(pascal(name)) + b"".join(primitives)
    params = ("|PATTERN=" + name + "|HEIGHT=1mm|DESCRIPTION=" + description).encode("cp1252") + b"\0"
    return {"Data": data, "Parameters": sub(params), "Header": struct.pack("<I", len(primitives))}


def pcblib():
    soic = []
    for i in range(4):  # pins 1-4 down the left (Altium y up), 5-8 up the right
        y = 1.905 - 1.27 * i
        soic.append(pad(str(i + 1), -2.7, y, 1.55, 0.6))
    for i in range(4):
        y = -1.905 + 1.27 * i
        if i == 1:  # pad 6 drawn upright and turned 90°
            soic.append(pad(str(i + 5), 2.7, y, 0.6, 1.55, rotation=90.0))
        else:
            soic.append(pad(str(i + 5), 2.7, y, 1.55, 0.6))
    outline = [track(-1.95, 2.45, 1.95, 2.45), track(1.95, 2.45, 1.95, -2.45), track(1.95, -2.45, -1.95, -2.45),
               track(-1.95, -2.45, -1.95, 2.45)]
    arc = b"\x01" + sub(b"\0" * 56)
    text = b"\x05" + sub(b"\0" * 230) + sub(pascal(".Designator"))
    body = b"\x0c" + sub(b"\0" * 23 + b"|V7_LAYER=MECHANICAL1|MODELTYPE=0\0")
    hdr = [pad("1", 0, 0, 1.7, 1.7, hole=1.0, shape=2, layer=74)]
    hdr += [pad(str(k + 1), 0, -2.54 * k, 1.7, 1.7, hole=1.0, shape=1, layer=74) for k in range(1, 4)]
    hdr.append(pad("", 0, 3.5, 3.2, 3.2, hole=3.2, shape=1, layer=74, plated=False))  # mounting hole
    via = b"\x03" + sub(b"\0" * 60)
    bad = [pad("1", 0, 0, 1, 1), b"\x63" + sub(b"\0" * 8), pad("2", 1, 0, 1, 1)]
    lib_header = sub(b"|HEADER=PCB 6.0 Binary Library File|VERSION=1|COMPCOUNT=3\0")
    return cfb({"FileHeader": sub(b"PCB 6.0 Binary Library File"),
                "Library": {"Data": lib_header, "Header": struct.pack("<I", 1)},
                "SOIC8_TI": footprint("SOIC8_TI", soic[:3] + [arc, text] + soic[3:] + outline + [body],
                                      "SOIC 8 lead, 1.27 mm pitch"),
                "HDR1X4": footprint("HDR1X4", hdr + [via], "Header 1x4, 2.54 mm"),
                "BAD": footprint("BAD", bad, "Unknown primitive after pad 1")})


def main():
    os.makedirs(OUT, exist_ok=True)
    # An integrated library's container (compressed copies of its libraries): recognised and refused.
    intlib = cfb({"Version.Txt": b"Version=1.0", "LibCrossRef.Txt": b"[LibCrossRef]",
                  "SchLib": {"0": b"\x02\x78\x9c" + b"\0" * 16}})
    files = {"Test.SchLib": schlib(), "Test.PcbLib": pcblib(), "Test.IntLib": intlib}
    for name, data in files.items():
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(data)
        try:
            import olefile  # independent check of the container
            ole = olefile.OleFileIO(data)
            streams = ["/".join(p) for p in ole.listdir()]
            ole.close()
            print(f"{name}: {len(data)} bytes, olefile reads {len(streams)} streams: {', '.join(streams)}")
        except ImportError:
            print(f"{name}: {len(data)} bytes (install olefile to cross-check the container)")


if __name__ == "__main__":
    main()
