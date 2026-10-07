#!/usr/bin/env python3
"""End-to-end test of `sieda-mcp --connect` (registered with CTest as sieda_mcp_connect).

Starts a small HTTP server on 127.0.0.1 (an ephemeral port) that plays the SiEDA app's live MCP endpoint: bearer
token, Mcp-Session-Id, 202 for notifications, a JSON reply, a Server-Sent-Events reply and a chunked reply. Then runs
the bridge with JSON-RPC lines on stdin and checks every stdout line, plus the failure paths: a wrong token and an
unreachable app both answer each request with a JSON-RPC error (so the client never hangs) and notifications with
nothing.

usage: mcp_connect_test.py <path to sieda-mcp>
"""
import json
import os
import socket
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = "test-token-123"
SESSION = "session-abc"


def fail(message):
    print("FAIL:", message)
    sys.exit(1)


class Endpoint(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    seen = []  # (method, session header) per POST

    def log_message(self, *args):  # keep the test output quiet
        pass

    def reply(self, status, body=b"", content_type="application/json", headers=None, chunked=False):
        self.send_response(status)
        if body or chunked:
            self.send_header("Content-Type", content_type)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(body), 7):  # small chunks
                part = body[i:i + 7]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.path != "/mcp":
            return self.reply(404, b"not found", "text/plain")
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            return self.reply(401, b"unauthorized", "text/plain", {"WWW-Authenticate": "Bearer"})
        accept = self.headers.get("Accept", "")
        if "application/json" not in accept or "text/event-stream" not in accept:
            return self.reply(406, b"bad accept", "text/plain")
        msg = json.loads(body)
        method = msg.get("method")
        Endpoint.seen.append((method, self.headers.get("Mcp-Session-Id")))
        if "id" not in msg:
            return self.reply(202)
        if method == "initialize":
            result = {"protocolVersion": "2025-06-18", "serverInfo": {"name": "fake-app"}}
            text = json.dumps({"jsonrpc": "2.0", "id": msg["id"], "result": result}, indent=2)  # newlines on purpose
            return self.reply(200, text.encode(), headers={"Mcp-Session-Id": SESSION})
        if method == "sse":
            event = json.dumps({"jsonrpc": "2.0", "id": msg["id"], "result": {"via": "sse"}})
            return self.reply(200, ("event: message\ndata: " + event + "\n\n").encode(), "text/event-stream")
        if method == "chunked":
            event = json.dumps({"jsonrpc": "2.0", "id": msg["id"], "result": {"via": "chunked", "pad": "x" * 100}})
            return self.reply(200, event.encode(), chunked=True)
        echo = {"jsonrpc": "2.0", "id": msg["id"], "result": {"echo": method}}
        return self.reply(200, json.dumps(echo).encode())


def run_bridge(exe, url, lines, token=None):
    env = dict(os.environ)
    env.pop("SIEDA_MCP_TOKEN", None)
    if token is not None:
        env["SIEDA_MCP_TOKEN"] = token
    proc = subprocess.run([exe, "--connect", url, "--timeout", "20"], input="\n".join(lines) + "\n",
                          capture_output=True, text=True, timeout=60, env=env)
    if proc.returncode != 0:
        fail(f"bridge exited with {proc.returncode}: {proc.stderr}")
    return [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]


def main():
    if len(sys.argv) != 2:
        fail(__doc__)
    exe = sys.argv[1]
    server = ThreadingHTTPServer(("127.0.0.1", 0), Endpoint)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{port}/mcp"

    lines = [
        json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}}),
        json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
        json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}),
        json.dumps({"jsonrpc": "2.0", "id": 3, "method": "sse"}),
        json.dumps({"jsonrpc": "2.0", "id": "four", "method": "chunked"}),
    ]
    out = run_bridge(exe, url, lines, TOKEN)
    if [m.get("id") for m in out] != [1, 2, 3, "four"]:
        fail(f"responses out of order or missing: {out}")
    if out[0]["result"]["serverInfo"]["name"] != "fake-app":
        fail("initialize result not passed through")
    if out[1]["result"]["echo"] != "tools/list":
        fail("tools/list not forwarded")
    if out[2]["result"]["via"] != "sse" or out[3]["result"]["via"] != "chunked":
        fail("SSE / chunked replies not decoded")
    sessions = [s for (m, s) in Endpoint.seen if m != "initialize"]
    if Endpoint.seen[0] != ("initialize", None) or any(s != SESSION for s in sessions):
        fail(f"Mcp-Session-Id not carried after initialize: {Endpoint.seen}")

    # A wrong token: each request gets a JSON-RPC error, the notification nothing.
    bad = run_bridge(exe, url, lines[:3], "wrong")
    if [m.get("id") for m in bad] != [1, 2] or any(m["error"]["code"] != -32000 for m in bad):
        fail(f"wrong token should give one error per request: {bad}")
    if "401" not in bad[0]["error"]["message"]:
        fail("the 401 is not explained")

    server.shutdown()
    server.server_close()
    # Nothing listening any more: errors, not a hang.
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    dead = probe.getsockname()[1]
    probe.close()
    gone = run_bridge(exe, f"http://127.0.0.1:{dead}/mcp", lines[:2], TOKEN)
    if [m.get("id") for m in gone] != [1] or "unreachable" not in gone[0]["error"]["message"]:
        fail(f"unreachable app should answer with an error: {gone}")

    # Invalid URL: usage error.
    proc = subprocess.run([exe, "--connect", "https://example.com/mcp"], input="", capture_output=True, text=True)
    if proc.returncode != 2:
        fail("https URL should be refused")
    print("ok: sieda-mcp --connect bridged", len(out), "responses")


if __name__ == "__main__":
    main()
