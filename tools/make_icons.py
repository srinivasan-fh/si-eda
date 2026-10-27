#!/usr/bin/env python3
"""Writes the Linux / Windows icons from the macOS app icon (SiEDA/Assets.xcassets/AppIcon.appiconset):
packaging/linux/sieda-studio.png (256 px), packaging/windows/sieda-studio.ico (16-256 px) and the Studio page's
favicon (a 32 px PNG data URL in Core/studio/ui.html). Needs Pillow; run it again when the app icon changes."""
import base64, io, os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
icon = Image.open(os.path.join(ROOT, "SiEDA", "Assets.xcassets", "AppIcon.appiconset", "icon_512x512@2x.png")).convert("RGBA")
icon.resize((256, 256), Image.LANCZOS).save(os.path.join(ROOT, "packaging", "linux", "sieda-studio.png"), optimize=True)
icon.save(os.path.join(ROOT, "packaging", "windows", "sieda-studio.ico"),
          sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
buf = io.BytesIO()
icon.resize((32, 32), Image.LANCZOS).save(buf, "PNG", optimize=True)
ui = os.path.join(ROOT, "Core", "studio", "ui.html")
html = open(ui, encoding="utf-8").read()
link = '<link rel="icon" href="data:image/png;base64,' + base64.b64encode(buf.getvalue()).decode() + '">'
start = html.find('<link rel="icon"')
html = html[:start] + link + html[html.index(">", start) + 1:] if start >= 0 else html.replace("<title>", link + "<title>", 1)
open(ui, "w", encoding="utf-8").write(html)
print("icons written")
