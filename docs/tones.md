# Shades and the panel draw

This note explains how Superfox26 turns a page into panel pixels. Read it before you change a
waveform or the shade table. The code calls a shade a "tone", so the source files and identifiers
use that word.

## The three pigments and the shades between them

The panel pigment has three native states: black, white and red. The driver moves a pixel with
short voltage pulses. A pulse that stops part way leaves the pixel between two native states, so a
pixel can hold a grey or a muted colour as well as the three natives. The author measured 103 such
shades on the panel from photos of test pages. Each measurement is the shade's share of the light
between the photo's black and its paper, per channel.

`main/tonemap.h` holds the 103 shades. Each entry has a colour in Oklab and a pattern. The pattern
is the list of drive pulses that produce the shade. `main/tonemap_entries.inc` holds the entry
data.

## The draw

1. The renderer composites a page into a 24-bit RGB band buffer, then error diffusion dithers each
   row to shade indices in the tone frame (`main/tones.cpp`, `tone_dither`). The frame is one byte
   per pixel, 960 by 672.
2. `tones_draw()` plans the pulls for the shades the frame uses (`main/tone_plan.h`,
   `tone_plan_build`), then plays them.
3. The driver (`components/epd97`) plays the pulls on the panel.

The driver runs two drive tables in turn. Phase 1 uses one table, phase 2 the other. At each pull a
pixel plays the code its shade holds for that pull. A code is a two-bit value in the two plane
buffers. The pattern notation and the two tables are documented in `main/tonemap.h` and
`main/tone_plan.h`.

The drive tables for the firmware are generated from `main/tonemap.h` into `main/tone_tables.inc`
on the host. After a change to the shade table, regenerate that file with `make generate` in
`host_tests/tone_tables`. The firmware does not build while the file is stale.

A panel whose controller is not the driver-B variant cannot run the shade tables. For that panel
`tones_draw()` reduces the frame to the three native colours and uses the panel's own waveform.

## Drive limits

A pixel must never be driven on one rail longer than the panel's own stock waveform drives it on
that rail. The stock waveform is the only drive known to be safe for this film. The limits are in
`components/epd97/epd97_config.h`:

- `EPD_PUSH_MAX_VDH` is 80 frames. VDH drives a pixel toward black.
- `EPD_PUSH_MAX_VDL` is 99 frames. VDL drives a pixel toward white.
- `EPD_PUSH_MAX_VDHR` is 36 frames. VDHR brings the red pigment forward.

VDH and VDHR drive the same way, so the two together up to the next VDL frame are held to the VDH
limit. A frame is 23.7 ms.

Three places keep a draw inside the limits:

- The table generator in `host_tests/tone_tables` refuses a pattern that breaks a limit.
- `tones_init()` checks the expanded patterns at boot through `tone_plan_init()`.
- The planner `main/tone_plan.h` keeps every pixel within the limits as it builds each draw.

The driver itself does not check what a pull drives. If you add or change a shade pattern, the
generator and the boot check are what stop an unsafe waveform. Keep them in the build.
