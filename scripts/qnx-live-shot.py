#!/usr/bin/env python3
"""qnx-live-shot.py — render the live QNX console to a PNG, for the every-2 s
feed of the on-target demo.

Reads the guest's serial output from a log file the bridge appends to, draws
the last lines as a terminal on a dark background, and stamps the phase the
demo is in (booting / crash recorded / power cut / survived). A dark, green-on-
black terminal is deliberate: it is a picture of the target's serial console,
not a web page.

Usage: qnx-live-shot.py <log-file> <out.png>
"""

from __future__ import annotations

import datetime as dt
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 1080, 700
BG = (10, 14, 20)
GREEN = (111, 224, 120)
AMBER = (242, 163, 60)
RED = (239, 107, 107)
DIM = (110, 122, 140)
FONT_PATHS = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
)


def load_font(size: int) -> ImageFont.ImageFont:
    for path in FONT_PATHS:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def phase(text: str) -> tuple[str, tuple[int, int, int]]:
    if "SURVIVED THE POWER CUT" in text:
        return "THE EVENT SURVIVED THE POWER CUT", GREEN
    if "READY" in text:
        return "CRASH IS ON THE MEDIA — waiting for the power cut", AMBER
    if "recovery done" in text or "AFTER THE POWER CUT" in text:
        return "rebooting — the journal runs the recovery", AMBER
    if "serving" in text and "backend qnx" in text:
        return "the journal serves — priority 62, backend qnx", GREEN
    return "booting QNX 8.0", DIM


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    try:
        text = open(sys.argv[1], "r", errors="replace").read()
    except OSError:
        text = ""

    lines = [line.rstrip() for line in text.splitlines()[-42:]]
    label, colour = phase(text)

    img = Image.new("RGB", (W, H), BG)
    draw = ImageDraw.Draw(img)
    mono = load_font(15)
    mono_big = load_font(22)

    draw.text((18, 14), "AMBRE — QNX 8.0 on QEMU — live serial console", fill=GREEN, font=mono_big)
    draw.text((18, 46), dt.datetime.now().strftime("%H:%M:%S"), fill=DIM, font=mono)
    draw.rectangle([18, 66, 860, 90], outline=(40, 50, 62), width=1)
    draw.text((24, 69), label, fill=colour, font=mono)

    y = 100
    for line in lines:
        if y > H - 26:
            break
        c = GREEN
        if line.startswith("=="):
            c = AMBER
        elif "SURVIVED" in line:
            c = GREEN
        elif "POWER CUT" in line or "ERROR" in line or "refused" in line:
            c = RED
        draw.text((18, y), line[:150], fill=c, font=mono)
        y += 15

    img.save(sys.argv[2], "PNG")
    return 0


if __name__ == "__main__":
    sys.exit(main())