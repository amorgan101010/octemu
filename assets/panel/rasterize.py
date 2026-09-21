#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rasterize assets/panel/octatrack.svg into out/panel/panel.bin.

The bin carries: the base panel raster (fader handle excised), an alpha
sprite for the fader handle, a pixel-replacement crop per lightable
element per colour (r/g/y), and the hit geometry from
octatrack-elements.json. State changes in the SVG are colour-only and
nothing moves, so a lit element composites as a straight pixel copy of
the lit render over the base.

Needs rsvg-convert and magick (brew: librsvg, imagemagick).

  python3 assets/panel/rasterize.py [--selftest out.png]
"""
import concurrent.futures
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

SCALE = 2
ASSETS = os.path.dirname(os.path.abspath(__file__))     # assets/panel, next to gen_svg.py
ROOT = os.path.dirname(os.path.dirname(ASSETS))         # the repo root, two levels up
OUTDIR = os.path.join(ROOT, "out/panel")
# librsvg does not resolve CSS custom properties, so each variant render
# substitutes its concrete ink colour for the var() fallback in the
# stylesheet. Lit elements ship as an exact two-layer decomposition solved
# from a black-ink and a green-ink render: pixel = under*(1-cov) + ink*cov,
# so cov = (green.G - black.G)/255 and the black render IS the under layer.
# The frontend draws the under crop, then the coverage sprite tinted by the
# wire's palette colour x brightness — any RGB the firmware sends.


def which(tool):
    for d in os.environ["PATH"].split(":"):
        if os.access(os.path.join(d, tool), os.X_OK):
            return True
    return False


def render(svg_text, w):
    with tempfile.TemporaryDirectory() as td:
        svg = os.path.join(td, "x.svg")
        png = os.path.join(td, "x.png")
        raw = os.path.join(td, "x.raw")
        with open(svg, "w") as f:
            f.write(svg_text)
        subprocess.run(["rsvg-convert", "-w", str(w), svg, "-o", png], check=True)
        subprocess.run(["magick", png, "-depth", "8", "RGBA:" + raw], check=True)
        with open(raw, "rb") as f:
            return f.read()


def group_span(text, gid):
    m = re.search(r'<g\b[^>]*id="%s"' % re.escape(gid), text)
    if not m:
        sys.exit("no group id=%s" % gid)
    start = m.start()
    pos = m.end()
    depth = 1
    tag = re.compile(r"<g\b|</g>")
    while depth:
        m2 = tag.search(text, pos)
        if not m2:
            sys.exit("unbalanced <g> at id=%s" % gid)
        depth += 1 if m2.group(0) == "<g" else -1
        pos = m2.end()
    return start, pos


def lit_svg(text, eid, color):
    pat = r'(<g class=")([^"]*)(" id="%s")' % re.escape(eid)
    out, n = re.subn(pat, r"\1\2 lit\3", text, count=1)
    if n != 1:
        sys.exit("cannot inject class into %s" % eid)
    return out.replace("var(--lit,#ff5340)", color)


def pressed_svg(text, eid):
    pat = r'(<g class="[^"]*" id="%s")(>| )' % re.escape(eid)
    out, n = re.subn(pat, r'\1 transform="translate(1.2 1.8)"\2',
                     text, count=1)
    if n != 1:
        sys.exit("cannot inject transform into %s" % eid)
    return out


def diff_bbox(base, img, W, H, bx, by, bw, bh):
    """Bounding box of base-vs-img pixel differences inside a margin
    around the element."""
    mg = 30 * SCALE
    x0 = max(0, int(bx * SCALE) - mg)
    y0 = max(0, int(by * SCALE) - mg)
    x1 = min(W, int((bx + bw) * SCALE) + mg)
    y1 = min(H, int((by + bh) * SCALE) + mg)
    lo_x, lo_y, hi_x, hi_y = W, H, -1, -1
    for y in range(y0, y1):
        o = y * W * 4
        ra = base[o + x0 * 4 : o + x1 * 4]
        rb = img[o + x0 * 4 : o + x1 * 4]
        if ra == rb:
            continue
        for x in range(x0, x1):
            i = (x - x0) * 4
            if ra[i : i + 4] != rb[i : i + 4]:
                if x < lo_x:
                    lo_x = x
                if x > hi_x:
                    hi_x = x
        lo_y = min(lo_y, y)
        hi_y = max(hi_y, y)
    if hi_x < 0:
        return None
    pad = 3 * SCALE
    return (max(x0, lo_x - pad), max(y0, lo_y - pad),
            min(x1 - 1, hi_x + pad), min(y1 - 1, hi_y + pad))


def crop_replace(img, W, box):
    lo_x, lo_y, hi_x, hi_y = box
    w = hi_x - lo_x + 1
    h = hi_y - lo_y + 1
    data = bytearray(w * h * 4)
    for y in range(h):
        o = ((lo_y + y) * W + lo_x) * 4
        data[y * w * 4 : (y + 1) * w * 4] = img[o : o + w * 4]
    for i in range(3, len(data), 4):
        data[i] = 255
    return lo_x, lo_y, w, h, bytes(data)


def crop_coverage(black, green, W, box):
    """White RGBA whose alpha is the ink coverage: green.G - black.G."""
    lo_x, lo_y, hi_x, hi_y = box
    w = hi_x - lo_x + 1
    h = hi_y - lo_y + 1
    data = bytearray(w * h * 4)
    for y in range(h):
        o = ((lo_y + y) * W + lo_x) * 4
        for x in range(w):
            a = green[o + x * 4 + 1] - black[o + x * 4 + 1]
            if a > 0:
                p = (y * w + x) * 4
                data[p] = data[p + 1] = data[p + 2] = 255
                data[p + 3] = a if a < 255 else 255
    return lo_x, lo_y, w, h, bytes(data)


def alpha_crop(rgba, W, H):
    lo_x, lo_y, hi_x, hi_y = W, H, -1, -1
    for y in range(H):
        row = rgba[y * W * 4 : (y + 1) * W * 4]
        if row.count(0) == len(row):
            continue
        for x in range(W):
            if row[x * 4 + 3]:
                if x < lo_x:
                    lo_x = x
                if x > hi_x:
                    hi_x = x
                if lo_y > y:
                    lo_y = y
                hi_y = y
    if hi_x < 0:
        sys.exit("empty fader handle render")
    w = hi_x - lo_x + 1
    h = hi_y - lo_y + 1
    data = bytearray(w * h * 4)
    for y in range(h):
        o = ((lo_y + y) * W + lo_x) * 4
        data[y * w * 4 : (y + 1) * w * 4] = rgba[o : o + w * 4]
    return lo_x, lo_y, w, h, bytes(data)


def pack_name(s):
    b = s.encode()
    if len(b) > 27:
        sys.exit("name too long: %s" % s)
    return b + b"\0" * (28 - len(b))


def main():
    for tool in ("rsvg-convert", "magick"):
        if not which(tool):
            sys.exit("rasterize: %s not found (brew install librsvg "
                     "imagemagick)" % tool)
    with open(os.path.join(ASSETS, "octatrack.svg")) as f:
        text = f.read()
    with open(os.path.join(ASSETS, "octatrack-elements.json")) as f:
        geo = json.load(f)

    vb_w, vb_h = geo["viewBox"][2], geo["viewBox"][3]
    W, H = vb_w * SCALE, vb_h * SCALE

    hs, he = group_span(text, "fader-handle")
    head_end = text.index("</style>") + len("</style>")
    handle_doc = text[:head_end] + text[hs:he] + "</svg>"
    base_nohandle_doc = text[:hs] + text[he:]

    # The headphones pointer ships as its own sprite (rotated at runtime):
    # excise it from the shipped base and bake it at the MIN position
    # (bottom-left; the SVG draws it at upper-left, so rotate -90 about the
    # face centre). Cropped to the face square so runtime rotation about the
    # dst centre pivots on the face centre.
    pm = re.search(r'<circle class="pointer"[^>]*/>', base_nohandle_doc)
    if not pm:
        sys.exit("no headphones pointer circle")
    pointer_el = pm.group(0)
    base_nohandle_doc = base_nohandle_doc[:pm.start()] + base_nohandle_doc[pm.end():]
    ph = geo["knobs"]["knob-phones"]
    pointer_doc = (text[:head_end] +
                   '<g transform="rotate(-90 %g %g)">%s</g></svg>'
                   % (ph["face_cx"], ph["face_cy"], pointer_el))

    base_full = render(text, W)
    base_nohandle = render(base_nohandle_doc, W)
    handle_rgba = render(handle_doc, W)
    pointer_rgba = render(pointer_doc, W)
    if len(base_full) != W * H * 4:
        sys.exit("unexpected raster size")

    jobs = []
    for eid, bb in geo["buttons"].items():
        # btn-func never lights (dark inline label), but it does press
        jobs.append((eid, eid != "btn-func", True,
                     bb["x"], bb["y"], bb["w"], bb["h"]))
    for eid, bb in geo["leds"].items():
        jobs.append((eid, True, False,
                     bb["cx"] - bb["r"], bb["cy"] - bb["r"],
                     2 * bb["r"], 2 * bb["r"]))

    def run(job):
        eid, lit, pressable, bx, by, bw, bh = job
        out = []
        if lit:
            black = render(lit_svg(text, eid, "#000000"), W)
            green = render(lit_svg(text, eid, "#00ff00"), W)
            b1 = diff_bbox(base_full, black, W, H, bx, by, bw, bh)
            b2 = diff_bbox(base_full, green, W, H, bx, by, bw, bh)
            if b2 is None:
                print("rasterize: %s lights nothing, skipped" % eid)
            else:
                box = b2 if b1 is None else (min(b1[0], b2[0]), min(b1[1], b2[1]),
                                             max(b1[2], b2[2]), max(b1[3], b2[3]))
                out.append(("%s|u" % eid, 0) + crop_replace(black, W, box))
                out.append(("%s|i" % eid, 1) + crop_coverage(black, green, W, box))
        if pressable:
            prs = render(pressed_svg(text, eid), W)
            box = diff_bbox(base_full, prs, W, H, bx, by, bw, bh)
            if box is None:
                print("rasterize: %s pressed renders no change" % eid)
            else:
                out.append(("%s|p" % eid, 0) + crop_replace(prs, W, box))
        return out

    sprites = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for r in ex.map(run, jobs):
            sprites.extend(r)
    sprites.append(("fader-handle", 1) + alpha_crop(handle_rgba, W, H))

    px0 = int((ph["face_cx"] - ph["face_r"]) * SCALE)
    py0 = int((ph["face_cy"] - ph["face_r"]) * SCALE)
    pw = int(2 * ph["face_r"] * SCALE)
    data = bytearray(pw * pw * 4)
    for y in range(pw):
        o = ((py0 + y) * W + px0) * 4
        data[y * pw * 4 : (y + 1) * pw * 4] = pointer_rgba[o : o + pw * 4]
    sprites.append(("phones-pointer", 1, px0, py0, pw, pw, bytes(data)))

    hits = []
    for eid, bb in geo["buttons"].items():
        hits.append((eid, 0, bb["x"], bb["y"], bb["w"], bb["h"], 0, 0))
    for eid, kb in geo["knobs"].items():
        hits.append((eid, 1, kb["cx"], kb["cy"], kb["r"],
                     kb["face_cx"], kb["face_cy"], kb["face_r"]))
    fd = geo["fader"]
    hits.append(("fader", 2, fd["slot"]["x"], fd["slot"]["y"],
                 fd["slot"]["w"], fd["slot"]["h"], fd["travel"]["max"], 0))
    sc = geo["screen"]
    hits.append(("screen", 3, sc["x"], sc["y"], sc["w"], sc["h"], 0, 0))

    os.makedirs(OUTDIR, exist_ok=True)
    out = os.path.join(OUTDIR, "panel.bin")
    with open(out + ".tmp", "wb") as f:
        # v2 added the plate rect (viewBox units) after the raster size.
        pl = geo["plate"]
        f.write(struct.pack("<6I", 0x4E50544F, 2, SCALE, vb_w, vb_h, 0))
        f.write(struct.pack("<2I", W, H))
        f.write(struct.pack("<4I", pl["x"], pl["y"], pl["w"], pl["h"]))
        f.write(base_nohandle)
        f.write(struct.pack("<I", len(sprites)))
        for name, kind, x, y, w, h, data in sprites:
            f.write(pack_name(name))
            f.write(struct.pack("<B3x2i2I", kind, x, y, w, h))
            f.write(data)
        f.write(struct.pack("<I", len(hits)))
        for name, kind, *v in hits:
            f.write(pack_name(name))
            f.write(struct.pack("<I6f", kind, *v))
    os.replace(out + ".tmp", out)
    print("rasterize: %s: %d sprites, %d hit shapes"
          % (out, len(sprites), len(hits)))

    if len(sys.argv) > 2 and sys.argv[1] == "--selftest":
        frame = bytearray(base_nohandle)
        spr = {s[0]: s[1:] for s in sprites}

        def replace(name):
            kind, x, y, w, h, data = spr[name]
            for yy in range(h):
                o = ((y + yy) * W + x) * 4
                frame[o : o + w * 4] = data[yy * w * 4 : (yy + 1) * w * 4]

        def blend(name, tint, dx=0):
            kind, x, y, w, h, data = spr[name]
            for yy in range(h):
                for xx in range(w):
                    p = (yy * w + xx) * 4
                    a = data[p + 3]
                    if not a:
                        continue
                    o = ((y + yy) * W + x + dx + xx) * 4
                    for ch in range(3):
                        s = tint[ch] if tint else data[p + ch]
                        v = frame[o + ch] * (255 - a) // 255 + s * a // 255
                        frame[o + ch] = v if v < 255 else 255

        for n, t in [("trig-1", (255, 70, 50)), ("trig-5", (255, 210, 60)),
                     ("btn-t1", (255, 70, 50)), ("btn-t2", (90, 230, 110)),
                     ("btn-play", (90, 230, 110)), ("btn-stop", (230, 230, 230)),
                     ("btn-proj", (230, 230, 230)), ("btn-src", (255, 70, 50)),
                     ("led-rec-status", (255, 70, 50)),
                     ("led-card-status", (90, 230, 110))]:
            replace(n + "|u")
            blend(n + "|i", t)
        replace("btn-yes|p")
        blend("fader-handle", None, 100 * SCALE)
        with tempfile.TemporaryDirectory() as td:
            raw = os.path.join(td, "x.raw")
            with open(raw, "wb") as f:
                f.write(frame)
            subprocess.run(["magick", "-size", "%dx%d" % (W, H), "-depth", "8",
                            "RGBA:" + raw, sys.argv[2]], check=True)
        print("rasterize: selftest -> %s" % sys.argv[2])


if __name__ == "__main__":
    main()
