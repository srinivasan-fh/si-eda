#!/usr/bin/env python3
"""End-to-end test of the sieda-mcp stdio server (registered with CTest as sieda_mcp_stdio).

Feeds a scripted transcript (one JSON-RPC message per line) to `sieda-mcp --root <temp dir>` and checks that every
stdout line is a JSON-RPC 2.0 response, that requests are answered in order and notifications are not, and that the
key results look right (initialize, tools/list, a tool call, a tool error, a PNG render that inflates).

usage: mcp_stdio_test.py <path to sieda-mcp> <transcript.jsonl>
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
import zlib


def fail(message):
    print("FAIL:", message)
    sys.exit(1)


def png_pixels_ok(data):
    """Checks the PNG signature, chunk CRCs and that the IDAT stream inflates to height * (width + 1) bytes."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return False
    at, idat, width, height = 8, b"", 0, 0
    while at + 12 <= len(data):
        length = int.from_bytes(data[at:at + 4], "big")
        kind = data[at + 4:at + 8]
        body = data[at + 8:at + 8 + length]
        crc = int.from_bytes(data[at + 8 + length:at + 12 + length], "big")
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
            return False
        if kind == b"IHDR":
            width, height = int.from_bytes(body[0:4], "big"), int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    raw = zlib.decompress(idat)
    return width > 0 and len(raw) == height * (width + 1)


def main():
    if len(sys.argv) != 3:
        fail(__doc__)
    exe, transcript = sys.argv[1], sys.argv[2]
    with open(transcript, encoding="utf-8") as f:
        lines = [line.rstrip("\n") for line in f if line.strip()]
    expected = []  # ids answered, in order (None for a line that is not JSON)
    for line in lines:
        try:
            msg = json.loads(line)
        except ValueError:
            expected.append(None)
            continue
        if "id" in msg:
            expected.append(msg["id"])
    with tempfile.TemporaryDirectory() as root:
        proc = subprocess.run([exe, "--root", root], input="\n".join(lines) + "\n", capture_output=True, text=True,
                              timeout=300)
        # The command-line diff (Git's diff driver): identical files exit 0, a changed value exits 1 and is listed.
        v1, v2 = os.path.join(root, "v1.siedaproj"), os.path.join(root, "v2.siedaproj")
        cli = [subprocess.run([exe] + args, capture_output=True, text=True, timeout=60)
               for args in (["--diff", v1, v1], ["--diff", v1, v2], ["--git-diff", "board.siedaproj", v1, "0", "100644", v2, "1", "100644"])]
    if proc.returncode != 0:
        fail("exit code %d, stderr: %s" % (proc.returncode, proc.stderr))
    out = [line for line in proc.stdout.split("\n") if line]
    if len(out) != len(expected):
        fail("expected %d responses, got %d:\n%s" % (len(expected), len(out), proc.stdout[:2000]))
    responses = {}
    for line, want in zip(out, expected):
        try:
            r = json.loads(line)
        except ValueError:
            fail("stdout line is not JSON: " + line[:200])
        if r.get("jsonrpc") != "2.0":
            fail("not JSON-RPC 2.0: " + line[:200])
        if r.get("id") != want:
            fail("id %r answered where %r was expected" % (r.get("id"), want))
        if ("result" in r) == ("error" in r):
            fail("a response needs exactly one of result / error: " + line[:200])
        responses[want] = r

    init = responses[1]["result"]
    if init["protocolVersion"] != "2025-06-18" or init["serverInfo"]["name"] != "sieda":
        fail("initialize: %r" % init)
    for cap in ("tools", "resources", "prompts"):
        if cap not in init["capabilities"]:
            fail("capability missing: " + cap)
    tools = responses[2]["result"]["tools"]
    if len(tools) < 50 or any(t["inputSchema"].get("type") != "object" for t in tools):
        fail("tools/list: %d tools" % len(tools))
    if not any(r["uri"] == "sieda://catalog" for r in responses[3]["result"]["resources"]):
        fail("resources/list has no catalog")
    if len(responses[4]["result"]["prompts"]) < 3:
        fail("prompts/list")
    if responses[5]["result"].get("isError"):
        fail("project_load_example failed: %r" % responses[5])
    erc = json.loads(responses[6]["result"]["content"][0]["text"])
    if erc.get("errors") != 0:
        fail("ERC of the example: %r" % erc)
    if responses[7]["result"].get("isError"):
        fail("pcb_update_from_schematic failed: %r" % responses[7])
    image = responses[8]["result"]["content"][1]
    if image.get("type") != "image" or image.get("mimeType") != "image/png":
        fail("render_pcb did not return a PNG image")
    if not png_pixels_ok(base64.b64decode(image["data"])):
        fail("render_pcb PNG is not valid")
    if not responses[9]["result"].get("isError"):
        fail("a tool call with missing arguments must be an isError result")
    if responses[10]["error"]["code"] != -32601:
        fail("unknown method must be -32601")
    if responses[None]["error"]["code"] != -32700:
        fail("malformed JSON must be -32700")
    if responses[11]["result"] != {}:
        fail("ping")
    # Team / MCAD: diff against the saved version, STEP and IDF out, the IDF placement back in (nothing moved).
    for i in (12, 13, 15, 16, 17):
        if responses[i]["result"].get("isError"):
            fail("call %d failed: %r" % (i, responses[i]))
    diff = json.loads(responses[14]["result"]["content"][0]["text"])
    if "~ R1 value" not in diff.get("text", "") or diff.get("identical"):
        fail("project_diff: %r" % diff)
    if cli[0].returncode != 0 or "No changes." not in cli[0].stdout:
        fail("--diff of a file with itself: %r" % cli[0])
    if cli[1].returncode != 1 or "~ R1 value" not in cli[1].stdout:
        fail("--diff v1 v2: %r" % cli[1])
    if cli[2].returncode != 0 or "SiEDA diff board.siedaproj" not in cli[2].stdout:
        fail("--git-diff: %r" % cli[2])
    if json.loads(responses[17]["result"]["content"][0]["text"]).get("moved") != []:
        fail("IDF placement round trip moved parts: %r" % responses[17])
    print("OK: %d responses" % len(out))


if __name__ == "__main__":
    main()
