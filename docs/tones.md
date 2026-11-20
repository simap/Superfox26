# Shades and the panel draw

This note explains how Superfox26 turns a page into panel pixels. Read it before you change a
waveform or the shade table. The code calls a shade a "tone", so the source files and identifiers
use that word.

## The three pigments and the shades between them

The panel pigment has three native states: black, white and red. The driver moves a pixel with
short voltage pulses. A pulse that stops part way leaves the pixel between two native states, so a
pixel can hold a grey or a muted colour as well as the three natives. The author measured 71 such
shades on the panel from photos of test pages. Each reading first takes out the camera's tone
curve, fitted to grey references on the page, and the uneven light across the photo. It then gives
the shade's share of the light between the nearby black and the nearby paper, per channel.

`main/tonemap.h` holds 73 entries: paper, black and the 71 shades. Each entry has a colour in Oklab
and a pattern. The pattern is the list of drive pulls that produce the shade.
`main/tonemap_entries.inc` holds the entry data. The red reference entry, `S4pRi3R5`, is the
strongest red the panel shows.

## The draw

1. The renderer composites a page into a 24-bit RGB band buffer, then error diffusion dithers each
   row to shade indices in the tone frame (`main/tones.cpp`, `tone_dither`). The frame is one byte
   per pixel, 960 by 672.
2. `tones_draw()` plans the pulls for the shades the frame uses (`main/tone_plan.h`,
   `tone_plan_build`), then plays them.
3. The driver (`components/epd97`) plays the pulls on the panel.

The driver runs two drive tables in turn. Phase 1 uses one table, phase 2 the other. At each pull a
pixel plays the code its shade holds for that pull. A code is a two-bit value in the two plane
buffers. The two drive tables are documented in `components/epd97/epd97_lut.h`:

- A phase 1 pull is 17 frames. Its codes are S (shake), I (ink), P (paper) and M (mono shake).
- A phase 2 pull is 11 frames, 261 ms. Its codes are h (hold), p (paper), i (ink) and w (red). A
  p pixel drives VDL in frames 1 to 4, and an i pixel drives VDH in frames 6 to 9. Frames 0, 5 and
  10 are ground. A p pixel next to an i pixel is therefore never on the opposite rail in the same
  frame. A w pixel drives VDHR in frames 1 to 9. An h pixel is not driven.

## Patterns

A pattern names the pulls of one shade. The full notation is documented in
`host_tests/tone_tables/tone_plan_expand.cpp`. In short:

- I, P, S and M are phase 1 pulls. I and P after an S or M play in phase 2 as two i or two p pulls.
- p and i are phase 2 nudges toward paper or ink. r is p then w, and R is p then two w.
- h is a phase 2 hold pull. It drives nothing and counts in the shade's phase 2 pulls.
- A number repeats the letter before it, so `i3` is three i pulls.
- One `|` mark splits phase 2. The letters before it play from the first phase 2 pull, the letters
  after it end on the last pull, and hold pulls fill the gap.

The planner (`main/tone_plan.h`) pads every shade to the same number of pulls in each phase. Phase 1
pads with S pulls. In phase 2, a shade without a `|` mark ends its letters on the last pull, with
hold pulls before them. The planner also adds pulls that cancel most of the shade's net drive: a
prefix of I or P pulls before the shade's phase 1 letters and a nudge of i or p pulls before its
phase 2 letters. What is left, the residual, is less than the drive of one nudge pull.

For example, the letters of `M3i2p5i3|i2ph` drive 8 frames more toward paper than toward ink. The
planner gives it a prefix of one I pull. In phase 1 it plays the I pull and then three M pulls at
the end, with S pulls as padding. In phase 2 it plays `iipppppiii` from the first pull, then hold
pulls, then `iiph` on the last four pulls.

The expanded patterns and the dither's tables are generated from `main/tonemap.h` into
`main/tone_tables.inc` on the host. After a change to the shade table, regenerate that file with `make generate` in
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
