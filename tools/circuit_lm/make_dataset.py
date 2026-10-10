#!/usr/bin/env python3
"""Training data for SiEDA's own circuit model: (request → design plan JSON) pairs.

Sources, all from SiEDA itself:
  * the reference circuits of the Offline Designer (SiEDA/AI/OfflineProvider.swift): every part (active and passive,
    with SiEDA's kind names and values) and every pin-to-pin link, asked for in many phrasings;
  * five families whose values are computed from the request (E12 values): LED series resistor from supply, colour
    and current; divider ratio; RC cut-off; op-amp gain; NPN LED driver supply.

    python3 tools/circuit_lm/make_dataset.py [out_dir]     # → train.jsonl, test.jsonl, card.json
      [--phrasings 100] [--contexts 30] [--extra my-designs.jsonl] [--templates templates.json]

The app's Own LLM → Dataset tab runs it with --templates (the reference circuits it ships, exported as JSON) and
--extra (the designs added from the app, one {"prompt", "plan"} per line).
"""
import json
import math
import os
import random
import re
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
MAX_PARTS = 30  # longer circuits do not fit the model's context
E12 = [1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2]


def e12(x):
    """Nearest E12 value."""
    d = 10 ** math.floor(math.log10(x))
    return min((m * d for m in E12 + [10.0]), key=lambda v: abs(math.log(v / x)))


def eng(x, unit=""):
    """10000 → 10k, 1e-7 → 100n (SiEDA value strings)."""
    for p, s in ((1e9, "G"), (1e6, "M"), (1e3, "k"), (1, ""), (1e-3, "m"), (1e-6, "u"), (1e-9, "n"), (1e-12, "p")):
        if abs(x) >= p * 0.999:
            v = x / p
            t = f"{v:.3g}"
            return (t.replace(".", s) if "." in t and s and len(t) <= 3 else t + s) + unit
    return f"{x:g}"


TEMPLATES = None  # --templates: the reference circuits as JSON (from the app) instead of OfflineProvider.swift


def templates():
    if TEMPLATES:
        return [dict(t, components=[dict(c, rotation=int(c.get("rotation", 0))) for c in t["components"]])
                for t in json.load(open(TEMPLATES)) if 0 < len(t["components"]) <= MAX_PARTS]
    src = open(os.path.join(ROOT, "SiEDA/AI/OfflineProvider.swift")).read()
    out = []
    for block in src.split("Template(\n")[1:]:
        comps = re.findall(r'PlannedComponent\(ref: "([^"]+)", kind: "([^"]+)", value: "([^"]*)", x: (-?[\d.]+), y: (-?[\d.]+)'
                           r'(?:, rotation: (-?\d+))?', block)
        if not comps or len(comps) > MAX_PARTS:
            continue
        board = re.search(r"PlannedBoard\(width: ([\d.]+), height: ([\d.]+)", block)
        out.append({
            "title": re.search(r'title: "([^"]+)"', block).group(1),
            "summary": re.search(r'summary: "([^"]+)"', block).group(1),
            "keywords": re.findall(r'"([^"]+)"', re.search(r"keywords: \[(.*?)\]", block, re.S).group(1)),
            "components": [dict(ref=r, kind=k, value=v, x=float(x), y=float(y), rotation=int(rot or 0))
                           for r, k, v, x, y, rot in comps],
            "connections": [dict(zip(("from", "to"), c)) for c in re.findall(r'PlannedConnection\(from: "([^"]+)", to: "([^"]+)"\)', block)],
            "board": {"width": float(board.group(1)), "height": float(board.group(2))} if board else {"width": 40, "height": 30},
        })
    return out


def router():
    """OfflineProvider.template(for:) over every reference circuit: keywords minus excludes, later (more specific)
    circuits first, the LED indicator as the fallback. Returns the title a request selects."""
    order = [(t["title"], t["keywords"], t.get("excludes", [])) for t in json.load(open(TEMPLATES))] if TEMPLATES else []
    src = "" if TEMPLATES else open(os.path.join(ROOT, "SiEDA/AI/OfflineProvider.swift")).read()
    for block in src.split("Template(\n")[1:]:
        words = lambda key: re.findall(r'"([^"]*)"', (re.search(key + r": \[(.*?)\]", block, re.S) or re.match("()", "")).group(1))
        order.append((re.search(r'title: "([^"]+)"', block).group(1), words("keywords"), words("excludes")))

    def route(request):
        text = " " + request.lower() + " "
        for title, keywords, excludes in reversed(order):
            if any(k in text for k in keywords) and not any(e in text for e in excludes):
                return title
        return order[0][0]
    return route


# Where a request says what the circuit is for: words of other circuits too, so the model learns which one wins.
CONTEXTS = ["for a 3D printer", "for a fan", "for a pump", "for a conveyor", "for a CNC machine", "for an industrial machine",
            "for a robot", "for a robot arm", "for a test bench", "for a lab project", "for a prototype", "for my product",
            "for a small machine", "for a fan motor", "for a pump motor", "for an e-bike", "with low-cost parts",
            "for a student project", "for a home appliance", "for a sensor board"]


def contexts(rng, t, n, route, by_title):
    """(prompt, plan) pairs: a keyword of t plus an application context, labelled by the Offline Designer's rule."""
    out = []
    for i in range(n):
        k = t["keywords"][i % len(t["keywords"])].strip()
        p = f"{rng.choice(VERBS)} {rng.choice(['a ', 'an ', 'the ', ''])}{k} {rng.choice(CONTEXTS)}" + rng.choice(["", ".", " please", "?"])
        target = by_title.get(route(p))
        if target:
            out.append((p[0].upper() + p[1:], plan_json(target)))
    return out


def plan_json(t, values=None, title=None):
    """The compact plan the model writes (keys the app's DesignPlan decoder reads)."""
    comps = []
    for c in t["components"]:
        d = {"ref": c["ref"], "kind": c["kind"], "value": (values or {}).get(c["ref"], c["value"]),
             "x": int(c["x"]), "y": int(c["y"])}
        if c["rotation"]:
            d["rotation"] = c["rotation"]
        comps.append(d)
    plan = {"title": title or t["title"], "components": comps, "connections": t["connections"],
            "board": {"width": int(t["board"]["width"]), "height": int(t["board"]["height"])}}
    return json.dumps(plan, ensure_ascii=False, separators=(",", ":"))


VERBS = ["Design", "Make", "Create", "Build", "Draw", "I need", "Give me", "Please design", "Can you make", "Generate",
         "Sketch", "Lay out", "Put together", "I want", "Help me build"]


def phrasings(rng, t, n):
    subjects = [t["title"], t["title"].lower(), t["summary"].rstrip("."), t["summary"].lower().rstrip(".")]
    subjects += [f"a {k.strip()} circuit" for k in t["keywords"]] + [f"{k.strip()}" for k in t["keywords"]]
    out = set()
    while len(out) < n:  # every subject (each keyword too) at least twice, then at random
        s = subjects[len(out) % len(subjects)] if len(out) < 2 * len(subjects) else rng.choice(subjects)
        v = rng.choice(VERBS)
        art = "" if s[0].isupper() or s.startswith("a ") else rng.choice(["a ", "an ", "the ", ""])
        p = f"{v} {art}{s}".strip() + rng.choice(["", ".", " please", " for my board", " on a small PCB", "?"])
        out.add(p[0].upper() + p[1:])
    return sorted(out)


def families(rng, by_title):
    """(prompt, plan) pairs with values computed from the request."""
    out = []
    led = by_title["LED Indicator"]
    colours = {"red": ("Red", 2.0), "green": ("Green", 2.1), "blue": ("Blue", 3.0), "white": ("White", 3.0), "yellow": ("Yellow", 2.1)}
    for v in (3.3, 5, 9, 12, 15, 24):
        for name, (val, vf) in colours.items():
            for ma in (2, 5, 10, 15, 20):
                if v <= vf + 0.3:
                    continue
                r = e12((v - vf) / (ma / 1000))
                p = rng.choice([f"{rng.choice(VERBS)} a {v:g} V {name} LED indicator at {ma} mA",
                                f"{name} LED on {v:g} V with {ma} mA",
                                f"{rng.choice(VERBS)} an LED circuit: {v:g} V supply, {name} LED, {ma} mA"])
                out.append((p, plan_json(led, {"V1": f"{v:g}", "R1": eng(r), "D1": val}, f"{v:g} V {val} LED Indicator")))
    div = by_title["Voltage Divider"]
    for vin in (3.3, 5, 9, 12, 15, 24, 48):
        for vout in (1.2, 1.8, 2.5, 3.3, 5, 6, 9, 12):
            if vout >= vin:
                continue
            r2 = e12(10e3 * vout / (vin - vout))
            p = rng.choice([f"{rng.choice(VERBS)} a voltage divider from {vin:g} V to {vout:g} V",
                            f"divide {vin:g} V down to {vout:g} V", f"{vin:g} V to {vout:g} V resistor divider"])
            out.append((p, plan_json(div, {"V1": f"{vin:g}", "R1": "10k", "R2": eng(r2)}, f"{vin:g} V to {vout:g} V Divider")))
    for kind, title in (("low-pass", "RC Low-Pass Filter"), ("high-pass", "RC High-Pass Filter")):
        t = by_title[title]
        for fc in (10, 50, 100, 300, 1e3, 1.6e3, 3e3, 10e3, 20e3, 50e3, 100e3):
            c = e12(1 / (2 * math.pi * 1e3 * fc))
            fs = eng(fc, "Hz")
            p = rng.choice([f"{rng.choice(VERBS)} an RC {kind} filter at {fs}", f"{kind} filter with {fs} cutoff",
                            f"RC {kind} with fc = {fs}"])
            out.append((p, plan_json(t, {"R1": "1k", "C1": eng(c)}, f"RC {kind.title()} Filter, {fs}")))
    for g in (2, 3, 5, 10, 11, 20, 50, 100):
        ni = by_title["Non-Inverting Amplifier"]
        p = rng.choice([f"{rng.choice(VERBS)} a non-inverting amplifier with a gain of {g}", f"op-amp gain {g} non-inverting",
                        f"non inverting amp, gain {g}"])
        out.append((p, plan_json(ni, {"R1": eng(e12(10e3 * (g - 1))), "R2": "10k"}, f"Non-Inverting Amplifier, Gain {g}")))
        inv = by_title["Inverting Amplifier"]
        p = rng.choice([f"{rng.choice(VERBS)} an inverting amplifier with a gain of {g}", f"inverting op-amp gain -{g}",
                        f"inverting amp, gain {g}"])
        out.append((p, plan_json(inv, {"R1": "10k", "R2": eng(e12(10e3 * g))}, f"Inverting Amplifier, Gain −{g}")))
    drv = by_title["NPN LED Driver"]
    for v in (3.3, 5, 9, 12, 24):
        for name, (val, vf) in colours.items():
            r = e12((v - vf - 0.1) / 0.009)
            p = rng.choice([f"{rng.choice(VERBS)} an NPN transistor driver for a {name} LED from {v:g} V",
                            f"transistor switch for a {name} LED, {v:g} V supply"])
            out.append((p, plan_json(drv, {"V1": f"{v:g}", "R2": eng(r), "D1": val}, f"NPN {val} LED Driver, {v:g} V")))
    return out


def main():
    import argparse
    global TEMPLATES
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir", nargs="?", default=os.path.join(os.path.dirname(__file__), "data"))
    ap.add_argument("--phrasings", type=int, default=100, help="requests per reference circuit")
    ap.add_argument("--contexts", type=int, default=30, help="requests with an application context per circuit")
    ap.add_argument("--extra", help="your own designs: one {\"prompt\", \"plan\"} JSON object per line")
    ap.add_argument("--templates", help="the reference circuits as JSON (the app's export)")
    args = ap.parse_args()
    TEMPLATES = args.templates
    out_dir = args.out_dir
    os.makedirs(out_dir, exist_ok=True)
    rng = random.Random(42)
    ts = templates()
    by_title = {t["title"]: t for t in ts}
    pairs = []
    for t in ts:
        pairs += [(p, plan_json(t)) for p in phrasings(rng, t, args.phrasings)]
    route = router()
    for t in ts:
        pairs += contexts(rng, t, args.contexts, route, by_title)
    fam = families(rng, by_title)
    pairs += fam * 3  # computed values are harder: seen more often
    extra = [json.loads(l) for l in open(args.extra) if l.strip()] if args.extra and os.path.exists(args.extra) else []
    for e in extra:  # each own design in several wordings
        plan = json.dumps(json.loads(e["plan"]), ensure_ascii=False, separators=(",", ":"))
        pairs += [(f"{v} {e['prompt']}".strip(), plan) for v in VERBS[:10]] + [(e["prompt"], plan)]
    rng.shuffle(pairs)
    seen, test, train = set(), [], []
    for p, plan in pairs:
        if p in seen:
            continue
        seen.add(p)
        (test if rng.random() < 0.06 else train).append({"prompt": p, "plan": plan})
    for name, rows in (("train", train), ("test", test)):
        with open(os.path.join(out_dir, f"{name}.jsonl"), "w") as f:
            for r in rows:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
    kinds = sorted({c["kind"] for t in ts for c in t["components"]})
    titles = [t["title"] for t in ts] + [json.loads(e["plan"]).get("title", e["prompt"]) for e in extra]
    card = {"circuits": titles, "families": 5, "train": len(train), "test": len(test), "kinds": kinds, "ownDesigns": len(extra)}
    json.dump(card, open(os.path.join(out_dir, "card.json"), "w"), indent=1, ensure_ascii=False)
    print(f"{len(ts)} circuits, {len(fam)} computed pairs, {len(extra)} own designs → {len(train)} train / {len(test)} test, "
          f"{len(kinds)} part kinds")


if __name__ == "__main__":
    main()
