#!/usr/bin/env python3
"""sieda-studio end to end: the page and the tool endpoint answer only with the token and a loopback Host; tools run;
the app stops on /quit. Usage: studio_test.py <path to sieda-studio>"""
import http.client, json, subprocess, sys, tempfile

def request(port, method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    return r.status, r.read().decode("utf-8", "replace")

def main():
    with tempfile.TemporaryDirectory() as root:
        p = subprocess.Popen([sys.argv[1], "--no-browser", "--root", root], stdout=subprocess.PIPE, text=True)
        try:
            url = p.stdout.readline().split("SiEDA Studio: ")[1].strip()
            port, token = int(url.split(":")[2].split("/")[0]), url.split("t=")[1]
            assert request(port, "GET", "/")[0] == 403, "page without the token"
            assert request(port, "GET", "/?t=" + "0" * 32)[0] == 403, "page with a wrong token"
            assert request(port, "GET", "/?t=" + token, headers={"Host": "attacker.example:%d" % port})[0] == 403, "foreign Host"
            status, page = request(port, "GET", "/?t=" + token)
            assert status == 200 and token in page and "__SIEDA_TOKEN__" not in page, "page"
            call = lambda name, args, hdr: request(port, "POST", "/rpc", json.dumps(
                {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": args}}), hdr)
            assert call("project_summary", {}, {})[0] == 403, "tool call without the token"
            auth = {"X-Sieda-Token": token, "Content-Type": "application/json"}
            status, body = call("project_load_example", {"id": "led_indicator"}, auth)
            assert status == 200 and not json.loads(body)["result"].get("isError"), body
            status, body = call("project_save_as", {"path": "designs/led.siedaproj"}, auth)
            assert status == 200 and not json.loads(body)["result"].get("isError"), body
            state = json.loads(request(port, "GET", "/state", headers=auth)[1])
            assert state["files"] == ["designs/led.siedaproj"] and state["revision"] > 0, state
            bearer = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}
            status, body = call("project_summary", {}, bearer)
            assert status == 200 and "LED indicator" in body, "MCP endpoint with the bearer token"
            assert request(port, "POST", "/quit", "{}", auth)[0] == 200
            assert p.wait(timeout=30) == 0
        finally:
            if p.poll() is None:
                p.kill()
    print("sieda-studio: ok")

main()
