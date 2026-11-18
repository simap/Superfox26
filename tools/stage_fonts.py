#!/usr/bin/env python3
"""Copy the font pack into the LittleFS image root.

usage: stage_fonts.py FSROOT [FONT_DIR]

FONT_DIR holds a pack that tools/fontpack wrote: the font files, the licence files and
manifest.json, which lists them. It defaults to data/fonts in the project, or B26_FONT_DIR in the
environment. Copies every face and licence file the manifest lists to FSROOT/fonts and removes any
other file there. The firmware opens the faces by the file names of main/fonts.cpp FACE_FILES, so a
pack of other fonts keeps those names. A face missing from the pack falls back to another
(fonts.h face_set::resolve). Only sans.ttf is required.
"""
import filecmp
import json
import os
import shutil
import sys

DEFAULT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "fonts")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    font_dir = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("B26_FONT_DIR", DEFAULT_DIR)
    with open(os.path.join(font_dir, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    names = [x["file"] for x in manifest["faces"]] + [x["file"] for x in manifest.get("licenses", [])]
    if "sans.ttf" not in names:
        sys.exit("%s: the pack has no sans.ttf, which the firmware needs" % font_dir)
    dest_dir = os.path.join(root, "fonts")
    os.makedirs(dest_dir, exist_ok=True)
    for name in os.listdir(dest_dir):
        if name not in names:
            os.remove(os.path.join(dest_dir, name))
    total = 0
    for name in names:
        src = os.path.join(font_dir, name)
        dest = os.path.join(dest_dir, name)
        total += os.path.getsize(src)
        if not os.path.exists(dest) or not filecmp.cmp(src, dest, shallow=False):
            shutil.copyfile(src, dest)
    print("fonts staged in %s from %s: %d files, %d bytes" % (dest_dir, os.path.abspath(font_dir), len(names), total))


if __name__ == "__main__":
    main()
