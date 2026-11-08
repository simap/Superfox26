#!/usr/bin/env python3
"""Copy the TTF faces into the LittleFS image root under fixed names.

usage: stage_fonts.py FSROOT [FONT_DIR]

Writes FSROOT/fonts/sans.ttf, bold.ttf, mono.ttf and monobold.ttf. FONT_DIR defaults to the
macOS supplemental fonts. B26_FONT_DIR, B26_FONT_SANS, B26_FONT_BOLD, B26_FONT_MONO and
B26_FONT_MONO_BOLD in the environment override the directory and the source file names. A copy
is skipped when the destination already has the same size and a newer time.
"""
import os
import shutil
import sys

DEFAULT_DIR = "/System/Library/Fonts/Supplemental"
FACES = [
    ("sans.ttf", os.environ.get("B26_FONT_SANS", "Arial.ttf")),
    ("bold.ttf", os.environ.get("B26_FONT_BOLD", "Arial Bold.ttf")),
    ("mono.ttf", os.environ.get("B26_FONT_MONO", "Courier New.ttf")),
    ("monobold.ttf", os.environ.get("B26_FONT_MONO_BOLD", "Courier New Bold.ttf")),
]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    font_dir = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("B26_FONT_DIR", DEFAULT_DIR)
    dest_dir = os.path.join(root, "fonts")
    os.makedirs(dest_dir, exist_ok=True)
    total = 0
    for dest_name, src_name in FACES:
        src = os.path.join(font_dir, src_name)
        dest = os.path.join(dest_dir, dest_name)
        s = os.stat(src)
        total += s.st_size
        if os.path.exists(dest):
            d = os.stat(dest)
            if d.st_size == s.st_size and d.st_mtime >= s.st_mtime:
                continue
        shutil.copyfile(src, dest)
    print("fonts staged in %s: %d bytes, %s" % (dest_dir, total, ", ".join(s for _, s in FACES)))


if __name__ == "__main__":
    main()
