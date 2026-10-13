#!/usr/bin/env python3
"""CTest sieda_python_api: the Python client drives sieda-mcp end to end. usage: test_sieda.py <sieda-mcp>"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sieda import Sieda, SiedaError  # noqa: E402

with tempfile.TemporaryDirectory() as root, Sieda(sys.argv[1], root=root) as s:
    assert any(t["name"] == "pcb_autoroute" for t in s.tools())
    s.project_load_example(id="led_indicator")
    s.pcb_update_from_schematic()
    summary = s.project_summary()
    assert summary.get("parts", summary.get("components", 0)), summary
    s.project_save_as(path="led.siedaproj")
    assert os.path.exists(os.path.join(root, "led.siedaproj"))
    try:
        s.project_load_example(id="no_such_example")
        raise SystemExit("FAIL: a bad example id did not raise")
    except SiedaError as e:
        assert "no_such_example" in str(e)
print("ok")
