#!/usr/bin/env python3
"""sieda-mcp answers the si-llm CLI's MCP client (github.com/kernelsrinivas/si-llm, py/si/mcp_client.py) as it talks.

That client is stricter than most: it sends `initialize` with no params and no `notifications/initialized`, reads one
line per request with an 8 s timeout, takes the reply text from content[0].text, and refuses a configured command
holding any of \\n ; & | ` $ < > or longer than 240 characters.  Usage: mcp_si_llm_client_test.py <sieda-mcp>
"""
import json
import queue
import shlex
import subprocess
import sys
import tempfile
import threading


def main(exe):
    with tempfile.TemporaryDirectory() as root:
        command = f"{shlex.quote(exe)} --si-llm --root {shlex.quote(root)}"
        documented = "/Applications/SiEDA.app/Contents/MacOS/sieda-mcp --si-llm --root /Users/me/Designs"
        assert len(documented) <= 240 and not any(ch in documented for ch in "\n;&|`$><"), "si-llm would refuse it"
        proc = subprocess.Popen(shlex.split(command), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, bufsize=1)
        lines = queue.Queue()  # select() cannot wait on a pipe on Windows: a reader thread instead
        threading.Thread(target=lambda: [lines.put(l) for l in proc.stdout], daemon=True).start()
        ids = 0

        def rpc(method, params=None):
            nonlocal ids
            ids += 1
            body = {"jsonrpc": "2.0", "id": ids, "method": method}
            if params is not None:
                body["params"] = params
            proc.stdin.write(json.dumps(body) + "\n")
            proc.stdin.flush()
            try:
                line = lines.get(timeout=8)
            except queue.Empty:
                raise AssertionError(f"{method}: no answer within si-llm's 8 s")
            payload = json.loads(line or "{}")
            assert payload.get("id") == ids, payload
            assert "error" not in payload, payload
            return payload.get("result") or {}

        try:
            init = rpc("initialize")
            assert (init.get("serverInfo") or {}).get("name") == "sieda", init
            tools = [t.get("name") for t in rpc("tools/list").get("tools") or []]
            assert "pcb_drc" in tools and "project_load_example" in tools, tools[:10]
            loaded = rpc("tools/call", {"name": "project_load_example", "arguments": {"id": "led_indicator"}})
            assert not loaded.get("isError"), loaded
            drc = rpc("tools/call", {"name": "pcb_drc", "arguments": {}})
            text = str((drc.get("content") or [{}])[0].get("text") or "")
            assert not drc.get("isError") and json.loads(text).get("status"), text[:200]
            # --si-llm: si-llm's own tool names (its planner's sch_add / pcb_add / sch_wire …) do SiEDA's work, and the
            # answer is one plain sentence, which si-llm shows as its reply.
            assert {"pcb_add", "sch_add", "sch_wire", "pcb_net", "pcb_flow", "pcb_status"} <= set(tools), tools[-25:]
            rpc("tools/call", {"name": "pcb_reset", "arguments": {}})
            for name, args in [("pcb_add", {"ref": "u1", "pkg": "esp32", "x": 25, "y": 20}),
                               ("pcb_add", {"ref": "C1", "pkg": "CAP", "x": 10, "y": 10}),
                               ("pcb_net", {"name": "GND", "a": "U1", "b": "C1"})]:
                r = rpc("tools/call", {"name": name, "arguments": args})
                said = r["content"][0]["text"]
                assert not r.get("isError") and not said.startswith("{"), (name, said)
            status = rpc("tools/call", {"name": "pcb_status", "arguments": {}})["content"][0]["text"]
            assert "U1" in status and "GND" in status, status
        finally:
            proc.terminate()
            proc.wait(timeout=10)
    print("si-llm client: initialize, tools/list, tools/call ok")


if __name__ == "__main__":
    main(sys.argv[1])
