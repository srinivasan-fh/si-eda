#!/usr/bin/env python3
"""CycloneDX 1.5 SBOM for a SiEDA release: what ships in it and the files it is made of.

Usage: make_sbom.py --version 1.2.0 [artefact ...] > sieda-1.2.0.cdx.json
Every artefact named on the command line is listed with its SHA-256. The output is deterministic for the same
inputs (no timestamps other than SOURCE_DATE_EPOCH, when set) so a rebuilt release gives the same SBOM.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import uuid
from datetime import datetime, timezone

# What a release carries besides SiEDA's own code. Licences are named only where the project is sure of them.
COMPONENTS = [
    {"type": "data", "name": "KiCad symbol and footprint libraries (pinouts and land patterns in the catalog)",
     "externalReferences": [{"type": "vcs", "url": "https://gitlab.com/kicad/libraries"}],
     "licenses": [{"license": {"id": "CC-BY-SA-4.0"}}]},
    {"type": "data", "name": "Espressif KiCad libraries (ESP32-C6 / ESP32-H2 pinouts)",
     "externalReferences": [{"type": "vcs", "url": "https://github.com/espressif/kicad-libraries"}]},
    {"type": "library", "name": "HarfBuzz", "scope": "optional",
     "description": "PDF text shaping when installed at build time (CMake SIEDA_HARFBUZZ)",
     "externalReferences": [{"type": "vcs", "url": "https://github.com/harfbuzz/harfbuzz"}],
     "licenses": [{"license": {"id": "MIT"}}]},
    {"type": "machine-learning-model", "name": "sieda-circuit-v1",
     "description": "SiEDA's own circuit model (SiEDA/Resources/Models), trained by tools/circuit_lm"},
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("artefacts", nargs="*")
    args = parser.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        commit = subprocess.run(["git", "-C", root, "rev-parse", "HEAD"], capture_output=True, text=True,
                                check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        commit = ""
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    sieda = {"type": "application", "name": "SiEDA", "version": args.version, "bom-ref": "sieda"}
    if commit:
        sieda["externalReferences"] = [{"type": "vcs", "url": "https://github.com/srinivasan-fh/si-eda",
                                        "comment": commit}]
    files = [{"type": "file", "name": os.path.basename(p),
              "hashes": [{"alg": "SHA-256", "content": sha256(p)}]} for p in sorted(args.artefacts)]
    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        # Derived from the version and commit, so the same release always gets the same serial number.
        "serialNumber": "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, f"sieda/{args.version}/{commit}")),
        "version": 1,
        "metadata": {"timestamp": datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                     "component": sieda},
        "components": COMPONENTS + files,
    }
    json.dump(bom, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
