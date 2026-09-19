# The panel skin — how it is generated and how it is driven

The whole panel subsystem lives in this directory: a generator, a rasterizer,
and these notes. Nothing else in the tree describes the panel's geometry.

| file | what it is |
| --- | --- |
| `gen_svg.py` | the generator — the source of the panel skin |
| `rasterize.py` | the rasterizer — turns the SVG into the raster the emulator loads |
| `README.md` | this file: the lighting API, the geometry, the invariants |
| `octatrack.svg` | **generated**, gitignored — the panel, every control unlit |
| `octatrack-elements.json` | **generated**, gitignored — geometry for every interactive element |

The two generated files come out of a single `gen_svg.py` run, so they cannot
drift out of sync. Two make targets drive the pair:

```sh
make panel-svg      # gen_svg.py    -> octatrack.svg + octatrack-elements.json
make panel          # rasterize.py  -> out/panel/panel.bin, what the emulator loads
```

`gen_svg.py` writes its two outputs into the current directory, so run it from
here (`make panel-svg` cd's in for you). `rasterize.py` locates both the inputs
and `out/panel/` from its own path, so it runs from anywhere:
`python3 assets/panel/rasterize.py`. It needs `rsvg-convert` and `magick`
(`brew install librsvg imagemagick`), and takes `--selftest out.png` to
composite a sample of lit and pressed states into a PNG you can look at.

Edit `gen_svg.py` and re-run it; never hand-edit the SVG. `rasterize.py` packs
into `out/panel/panel.bin`: the base raster, an under layer plus an
ink-coverage sprite per lightable element, pressed variants, the fader handle,
the headphones pointer and the hit geometry. `src/skin.c` draws from that file,
so the SVG is the design and the `.bin` is what runs.

The geometry was measured from photographs of a real unit and its printed
labels; the layout, colours and label hierarchy are all expressed in
`gen_svg.py` itself, which is the record of them.

Coordinate system: `viewBox="0 0 1250 682"`. Every number below and in the JSON
is in those units.

## Lighting buttons and LEDs

Every button is a `<g class="key" id="...">`, every LED a `<g class="led" id="...">`.

- Add class `lit` to light it. Colour defaults to red; override with a helper
  class (`lit-red`, `lit-green`, `lit-yellow`) or inline `style="--lit:#22ccee"`.
- T1-T8 (`btn-t1`..`btn-t8`) light red or green — `lit lit-red` / `lit lit-green`.
- Trigs are `trig-1`..`trig-16`. Trigs 1/5/9/13 also carry class `marked`: they
  have a printed square ring (white unlit, lit-colour + glow when lit). Do NOT
  add `marked` to other keys — the ring is physical print that exists only on
  those four.
- Trig underlines (`.u`) and the ring are always present in the DOM
  (transparent / white); `lit` only recolours them.
- The record button (`btn-rec`) icon is stroke-only: when lit, only the circle
  outline lights, never the fill. Same for the play/stop/arrow icons.
- Page LEDs (`led-page-1`..`4`) have their hardware colours baked in via inline
  `--lit` (yellow, red, red, red) — just toggle `lit`.
- Other LEDs: `led-card-status`, `led-in-a/b/c/d`, `led-int-l`, `led-int-r`,
  `led-rec-status`.
- `btn-func` has a dark inline label fill; it does not light on hardware, and
  its inline style would defeat `.lit` recolouring anyway.

Full id list with bounding boxes: `octatrack-elements.json` → `buttons`, `leds`.

## Knobs (headphones + 7 encoders)

Ids: `knob-phones` (has a pointer dot), `knob-level`, `knob-a`..`knob-f`. The
JSON gives each knob's base centre (`cx,cy,r`) and **top-face** centre
(`face_cx, face_cy, face_r` — offset up-left of the base for the 3D look).

- Do NOT rotate the whole knob group: the shadow and the face offset would
  orbit.
- `knob-phones`: rotate its `.pointer` circle around the face centre, e.g.
  `svg.querySelector('#knob-phones .pointer').setAttribute('transform',
  'rotate(A fx fy)')` with `fx,fy` = `face_cx, face_cy` from the JSON.
- Encoders are endless and have no printed pointer, which matches the hardware.
  If you want visible rotation, append your own indicator inside the knob group
  at the face centre and rotate that. Keep it inside the group so ids stay
  stable.

## Crossfader

- Move `#fader-handle` (carriage + cap) with `transform="translate(dx 0)"`.
- Travel: `dx` from `0` (far left, the default) to `166` (far right). The slot is
  drawn full width; the handle just slides over it. Values also in the JSON →
  `fader`.

## Screen

- `<rect id="screen" x="473" y="172" width="240" height="120">` — exactly 2:1,
  i.e. a 128x64 display at 1.875 SVG units per native pixel.
- Overlay frames by positioning a canvas over that rect, or by inserting an
  `<image>`/`<foreignObject>` at those coordinates inside the SVG. For crisp
  128x64 pixels, scale with `image-rendering: pixelated`.
- The bezel print ("8 Track Dynamic Performance Sampler", "Octatrack MKII") is
  outside the cutout; leave it.

## Invariants worth keeping

1. **State toggles must never change geometry.** Every state visual exists
   permanently and `lit` only changes paint. That invariant is what lets
   `rasterize.py` decompose each element into an under layer and a
   coverage sprite and composite any RGB the firmware sends — if a state moved
   something, the raster pipeline would have to re-render instead of blit. If
   you add elements, follow the same pattern (transparent when off) and
   re-verify by diffing two renders with the `class` attributes stripped.
2. **CSS beats presentation attributes.** The stylesheet sets `text-anchor` and
   `font-weight` globally; anything element-specific uses inline `style="..."`.
   To override appearance, use inline styles or more specific selectors.
3. **The glow filter needs a non-zero bounding box.** The filter region is
   percentage-based, so a stroked `<line>`/`<path>` with zero bbox height
   disappears when filtered. That is why trig underlines are thin `<rect>`s.
   Give any new glowing element real area.
4. **Fonts are Helvetica Neue / Arial at weight 600.** For pixel-identical text
   across platforms, convert text to paths or ship a webfont; nothing in the
   emulator path depends on it, because the raster is built once by
   `make panel`.
