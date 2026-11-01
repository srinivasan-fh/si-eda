#!/usr/bin/env python3
"""sieda-studio end to end: the page and the tool endpoint answer only with the token and a loopback Host; tools run;
the app stops on /quit. Usage: studio_test.py <path to sieda-studio>"""
import http.client, json, os, shutil, subprocess, sys, tempfile

def request(port, method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    return r.status, r.read().decode("utf-8", "replace")

def check_script():
    """The page's script parses (node --check), when Node.js is installed."""
    node = shutil.which("node")
    if not node:
        return
    html = open(os.path.join(os.path.dirname(__file__), "..", "studio", "ui.html"), encoding="utf-8").read()
    script = html.split("<script>", 1)[1].split("</script>", 1)[0]
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False, encoding="utf-8") as f:
        f.write(script)
    try:
        r = subprocess.run([node, "--check", f.name], capture_output=True, text=True)
        assert r.returncode == 0, "ui.html script: " + r.stderr
    finally:
        os.unlink(f.name)

def check_translations():
    """Core/studio/i18n.json is current (tools/make_studio_i18n.py) and every Studio string has its translations."""
    tool = os.path.join(os.path.dirname(__file__), "..", "..", "tools", "make_studio_i18n.py")
    r = subprocess.run([sys.executable, tool, "--check"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    missing = subprocess.run([sys.executable, tool, "--missing"], capture_output=True, text=True).stdout.strip()
    assert not missing, "Studio strings without translations:\n" + missing

def main():
    check_script()
    check_translations()
    with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as data:
        # A plug-in in the user's plug-in folder (XDG_DATA_HOME/SiEDA/Plugins) that edits the design's copy.
        plugins = os.path.join(data, "SiEDA", "Plugins")
        os.makedirs(plugins)
        script = os.path.join(plugins, "retune")
        with open(script, "w") as f:
            f.write("#!%s\nimport os\nprint('retuning R1')\np = os.environ['SIEDA_PROJECT']\n"
                    "t = open(p).read()\nopen(p, 'w').write(t.replace('\"330\"', '\"470\"'))\n" % sys.executable)
        os.chmod(script, 0o755)
        env = dict(os.environ, XDG_DATA_HOME=data)
        p = subprocess.Popen([sys.argv[1], "--no-browser", "--root", root], stdout=subprocess.PIPE, text=True, env=env)
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
            if os.name != "nt":
                assert request(port, "GET", "/plugins")[0] == 403, "plug-ins without the token"
                listed = json.loads(request(port, "GET", "/plugins", headers=auth)[1])
                assert listed["plugins"] == ["retune"], listed
                run = lambda body: json.loads(request(port, "POST", "/plugins/run", json.dumps(body), auth)[1])
                assert "error" in run({"name": "../retune", "sandbox": False}), "only plug-ins from the folder"
                r = run({"name": "retune", "sandbox": False})
                assert r.get("applied") and "retuning R1" in r["output"], r
                design = lambda: call("project_export_json", {}, auth)[1]
                assert '\\"470\\"' in design(), "the plug-in's change is the design"
                call("project_undo", {}, auth)
                assert '\\"330\\"' in design(), "one undo step takes it back"
                r = run({"name": "retune", "sandbox": True})
                # Sandboxed: applied where bubblewrap runs; a clear refusal without it (or where it may not make namespaces).
                assert r.get("applied") or "bubblewrap" in r.get("error", "") or "bwrap" in r.get("output", ""), r
            status, words = request(port, "GET", "/i18n?lang=de", headers=auth)
            assert status == 200 and json.loads(words).get("File") == "Datei", "German interface words"
            assert request(port, "GET", "/i18n?lang=xx", headers=auth)[1] == "{}", "unknown language"
            assert request(port, "GET", "/i18n?lang=de")[0] == 403, "translations without the token"
            assert request(port, "POST", "/quit", "{}", auth)[0] == 200
            assert p.wait(timeout=30) == 0
        finally:
            if p.poll() is None:
                p.kill()
    print("sieda-studio: ok")

main()
