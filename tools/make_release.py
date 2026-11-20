"""Builds the single-file firmware image for the web installer and releases.

Usage (after `idf.py build`, in the ESP-IDF environment):
    python tools/make_release.py

Merges bootloader, partition table, OTA data and app (build/flasher_args.json)
into one image written at address 0x0:
    docs/firmware/zx-spec-wespi.bin          used by the web installer (docs/)
    dist/zx-spec-wespi-<version>.bin         attached to the GitHub release
and writes docs/manifest.json (ESP Web Tools) with the version.

The image covers the flash from 0x0 to the end of the app: installing it also
clears the NVS partition (WiFi network, language); the tapes (SPIFFS) stay.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
NAME = "zx-spec-wespi"


def main():
    with open(os.path.join(BUILD, "flasher_args.json")) as f:
        fa = json.load(f)
    with open(os.path.join(ROOT, "version.txt")) as f:
        version = f.read().strip()
    idf = os.environ.get("IDF_PATH")
    if not idf:
        sys.exit("run in the ESP-IDF environment (IDF_PATH not set)")
    esptool = os.path.join(idf, "components", "esptool_py", "esptool", "esptool.py")

    docs_bin = os.path.join(ROOT, "docs", "firmware", NAME + ".bin")
    os.makedirs(os.path.dirname(docs_bin), exist_ok=True)
    parts = []
    for ofs, path in sorted(fa["flash_files"].items(), key=lambda x: int(x[0], 16)):
        parts += [ofs, os.path.join(BUILD, path)]
    s = fa["flash_settings"]
    cmd = [sys.executable, esptool, "--chip", "esp32", "merge_bin", "-o", docs_bin,
           "--flash_mode", s["flash_mode"], "--flash_freq", s["flash_freq"], "--flash_size", "4MB"] + parts
    subprocess.run(cmd, check=True)

    dist = os.path.join(ROOT, "dist")
    os.makedirs(dist, exist_ok=True)
    rel_bin = os.path.join(dist, "%s-%s.bin" % (NAME, version))
    shutil.copyfile(docs_bin, rel_bin)

    manifest = {
        "name": "ZX-Spec Wespi",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32", "parts": [{"path": "firmware/%s.bin" % NAME, "offset": 0}]}],
    }
    with open(os.path.join(ROOT, "docs", "manifest.json"), "w", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("version %s: %s (%d bytes), %s" % (version, docs_bin, os.path.getsize(docs_bin), rel_bin))


if __name__ == "__main__":
    main()
