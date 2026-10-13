"""SiEDA from Python.

A thin client for the `sieda-mcp` server: every MCP tool (see docs/MCP.md) is a method, its arguments keyword
arguments, its result a dict. It needs only the Python standard library and the `sieda-mcp` binary.

    from sieda import Sieda
    with Sieda(root="designs") as s:
        s.project_load_example(id="led_indicator")
        s.pcb_update_from_schematic()
        s.pcb_autoroute()
        print(s.pcb_drc())
        s.output_fabrication_package(dir="fab")

Files are read and written inside `root` (the server's sandbox). `tools()` lists the tools with their schemas.
"""
import json
import subprocess

__all__ = ["Sieda", "SiedaError"]
PROTOCOL = "2025-06-18"


class SiedaError(RuntimeError):
    """A tool call that failed (the tool's own message) or a broken server."""


class Sieda:
    def __init__(self, executable="sieda-mcp", root=".", read_only=False):
        args = [executable, "--root", root] + (["--read-only"] if read_only else [])
        self._proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self._id = 0
        self._rpc("initialize", {"protocolVersion": PROTOCOL, "capabilities": {},
                                 "clientInfo": {"name": "sieda.py", "version": "1"}})
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _send(self, message):
        self._proc.stdin.write(json.dumps(message) + "\n")
        self._proc.stdin.flush()

    def _rpc(self, method, params):
        self._id += 1
        self._send({"jsonrpc": "2.0", "id": self._id, "method": method, "params": params})
        line = self._proc.stdout.readline()
        if not line:
            raise SiedaError("sieda-mcp exited")
        reply = json.loads(line)
        if "error" in reply:
            raise SiedaError(reply["error"].get("message", "JSON-RPC error"))
        return reply["result"]

    def tools(self):
        """The server's tools: [{name, title, description, inputSchema, annotations}]."""
        return self._rpc("tools/list", {})["tools"]

    def call(self, tool, **arguments):
        """Runs one tool; returns its JSON result (a dict), or raises SiedaError with the tool's message."""
        result = self._rpc("tools/call", {"name": tool, "arguments": arguments})
        text = "".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")
        if result.get("isError"):
            raise SiedaError(text)
        try:
            return json.loads(text) if text else {}
        except ValueError:
            return {"text": text}

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return lambda **arguments: self.call(name, **arguments)

    def close(self):
        if self._proc.poll() is None:
            self._proc.stdin.close()
            self._proc.wait(timeout=30)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
