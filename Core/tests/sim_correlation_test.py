#!/usr/bin/env python3
"""SiEDA's simulator against ngspice, the open-source SPICE reference: the same circuits (SiEDA's own SPICE netlist
export) must give the same operating points, AC response and transient waveforms. Registered with CTest as
sieda_sim_correlation when ngspice is installed.

usage: sim_correlation_test.py <path to sieda-mcp> <path to ngspice>
"""
import bisect, json, math, os, subprocess, sys, tempfile


def mcp(server, calls):
    """Runs tool calls in one sieda-mcp session; returns each call's parsed JSON result."""
    lines = [json.dumps({"jsonrpc": "2.0", "id": i, "method": "tools/call", "params": {"name": n, "arguments": a}})
             for i, (n, a) in enumerate(calls)]
    out = subprocess.run([server], input="\n".join(lines) + "\n", capture_output=True, text=True, timeout=300).stdout
    results = {}
    for line in out.splitlines():
        m = json.loads(line)
        r = m["result"]
        if r.get("isError"):
            raise AssertionError("%s failed: %s" % (calls[m["id"]][0], r["content"][0]["text"]))
        results[m["id"]] = json.loads(r["content"][0]["text"])
    return [results[i] for i in range(len(calls))]


def spice_name(net):
    return net.replace("$", "_").lower()


def ngspice(binary, netlist, control):
    """Runs a netlist with a .control block in batch mode; returns stdout and the wrdata file (if any)."""
    with tempfile.TemporaryDirectory() as d:
        data = os.path.join(d, "out.txt")
        body = netlist.replace(".op\n", "").replace(".end\n", "")
        deck = body + ".control\n" + control.replace("DATA", data) + "\n.endc\n.end\n"
        path = os.path.join(d, "deck.cir")
        open(path, "w").write(deck)
        r = subprocess.run([binary, "-b", path], capture_output=True, text=True, timeout=120)
        rows = []
        if os.path.exists(data):
            for line in open(data):
                parts = line.split()
                if len(parts) >= 2:
                    rows.append((float(parts[0]), float(parts[1])))
        return r.stdout + r.stderr, rows


def check_op(server, binary, example):
    load, net, op = mcp(server, [("project_load_example", {"id": example}), ("sim_spice_netlist", {}), ("sim_dc_op", {})])
    text, _ = ngspice(binary, net["netlist"], "op\nprint all")
    ref = {}
    for line in text.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            try:
                ref[k.strip().lower()] = float(v.split()[0])
            except ValueError:
                pass
    compared = 0
    for name, v in op["netVoltages"].items():
        if name == "GND":
            continue
        key = spice_name(name)
        assert key in ref, "%s: ngspice has no node %s (%s)" % (example, key, sorted(ref))
        tol = max(1e-3, 1e-3 * abs(ref[key]))
        assert abs(v - ref[key]) <= tol, "%s: %s = %.6g V, ngspice %.6g V" % (example, name, v, ref[key])
        compared += 1
    assert compared, example + ": nothing compared"
    return "%s: %d node voltages agree" % (example, compared)


def interpolate(rows, x):
    xs = [r[0] for r in rows]
    i = min(max(bisect.bisect_left(xs, x), 1), len(xs) - 1)
    (x0, y0), (x1, y1) = rows[i - 1], rows[i]
    return y0 if x1 == x0 else y0 + (y1 - y0) * (x - x0) / (x1 - x0)


def check_rc(server, binary):
    _, net, ac, tran = mcp(server, [("project_load_example", {"id": "rc_lowpass"}), ("sim_spice_netlist", {}),
                                    ("sim_ac", {"start": "10", "stop": "100k", "pointsPerDecade": 10, "maxPoints": 1000, "nets": ["OUT"]}),
                                    ("sim_transient", {"stop": "5m", "step": "1u", "maxPoints": 1000, "nets": ["OUT"]})])
    deck = net["netlist"].replace("SIN(0 1 1000)", "SIN(0 1 1000) AC 1")
    _, rows = ngspice(binary, deck, "ac dec 10 10 100k\nwrdata DATA vdb(out)")
    out = next(n for n in ac["nets"] if n["name"] == "OUT")
    worst = 0
    for f, db in zip(ac["frequency"], out["magnitudeDb"]):
        worst = max(worst, abs(db - interpolate(rows, f)))
    assert worst < 0.05, "rc_lowpass AC differs from ngspice by %.3f dB" % worst
    _, rows = ngspice(binary, net["netlist"], "tran 1u 5m\nwrdata DATA v(out)")
    wave = next(n for n in tran["nets"] if n["name"] == "OUT")["values"]
    worstT = max(abs(v - interpolate(rows, t)) for t, v in zip(tran["time"], wave))
    assert worstT < 0.01, "rc_lowpass transient differs from ngspice by %.4f V (of 1 V)" % worstT
    return "rc_lowpass: AC within %.3f dB, transient within %.4f V of ngspice" % (worst, worstT)


def main():
    server, binary = sys.argv[1], sys.argv[2]
    for msg in [check_op(server, binary, e) for e in ("led_indicator", "transistor_switch", "opamp_noninverting")] + [check_rc(server, binary)]:
        print(msg)
    print("sim correlation: ok")


main()
