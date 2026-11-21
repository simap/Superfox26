# Shades and the panel draw

This note explains how Superfox26 turns a page into panel pixels. Read it before you change a
waveform or the shade table. The code calls a shade a "tone", so the source files and identifiers
use that word.

## The three pigments and the shades between them

The panel pigment has three native states: black, white and red. The driver moves a pixel with
short voltage pulses. A pulse that stops part way leaves the pixel between two native states, so a
pixel can hold a grey or a muted colour as well as the three natives. The table holds 70 such
shades, measured on the panel from photos of test pages. Each reading first takes out the camera's tone
curve, fitted to grey references on the page, and the uneven light across the photo. It then gives
the shade's share of the light between the nearby black and the nearby paper, per channel.

`main/tonemap.h` holds 72 entries: paper, black and the 70 shades. Each entry has a colour in Oklab
and a pattern. The pattern is the list of drive pulls that produce the shade.
`main/tonemap_entries.inc` holds the entry data. The red reference entry, `S4pRi3R5`, is the
strongest red the panel shows.

## The draw

1. The renderer composites a page into a 24-bit RGB band buffer and a mask that marks the opaque
   pixels of photo images. Each row is then dithered to shade indices in the tone frame
   (`main/tones.cpp`, `tone_dither`). The frame is one byte per pixel, 960 by 672.
2. `tones_draw()` plans the pulls for the shades the frame uses (`main/tone_plan.h`,
   `tone_plan_build`), then plays them.
3. The driver (`components/epd97`) plays the pulls on the panel.

## The dither

Each pixel's colour is first mapped to a target shade colour: the colour is moved onto the red hue
and its chroma is scaled to what the panel can show at its lightness. A pixel whose target is on a
shade takes that shade. A target without chroma never takes a shade whose pattern has r or R.

The mapping starts with a tone curve on the pixel's Oklab lightness: L becomes L to the power
1 / TONEMAP_GAMMA, with 0 and 1 fixed. TONEMAP_GAMMA is a build constant in `main/tonemap.h`, now
1.4, which lightens the mid tones for the panel: L 0.3 draws as 0.42, 0.5 as 0.61 and 0.7 as 0.78.
The chroma keeps its share of what the image can hold at the lightness before the curve. The fixed
point squeeze applies the curve with three scale steps and a polynomial of degree 4, no lookup
table. `make generate` fits the polynomial for the gamma of the build, at a worst miss of 0.00013
of L for 1.4, and refuses a fit that misses by more than 0.0005. After a change of the gamma, run
`make generate`. A fill of the page lands on one shade only at the gamma it was chosen for, so the
fills of the logo and of the low battery notice follow the gamma.

Pixels outside the photo mask, text and fills, take Atkinson error diffusion. Photo pixels take an
ordered dither: a blue noise threshold picks one of a few shades per pixel and no error is carried.
A grey photo pixel mixes the two grey shades on either side of it. The photo mode sets what a
coloured photo pixel does:

- `diffuse`: the mask is ignored and photo pixels take error diffusion too.
- `triangle`: the pixel mixes the three shades that enclose its target in linear light. The mix keeps
  the mean colour, but the shades can be far apart, for example paper dots on a strong red.
- `nearest`, the default: the pixel mixes the three shades of a triangle around its target in a
  plane of lightness and chroma (`main/tone_photo.h`, `main/tone_graph.h`). Each shade owns the
  targets nearer to it than to any other shade, two shades neighbour when their areas share a
  border, and three neighbours whose areas meet at a point form a triangle. The target keeps its
  lightness and chroma. For a target in a triangle, the pixel takes the owner of the target, then
  the neighbour at the least angle from the direction of the target, then the shade that forms a
  triangle with both on the target's side. When the target lies beyond an edge of that triangle,
  the triangle across the edge takes its place, up to 3 times. The weights of the three shades are
  the target's position in the triangle. A target above the top of the triangles, a colour the
  shades cannot mix, takes the focal rule: the point where the line from it to a fixed focal point
  crosses the top, as a mix of the two shades at the ends of that edge. The focal point lies on the
  gray axis at lightness 0.513, and the top, a line of 24 shades, is star shaped from it, so each
  point of the top takes the targets of one direction. The mix then changes smoothly as the target
  moves, with no line where the dither starts or stops, and a target on the top takes itself. The
  distance from target to mix is 0.030 on average and 0.090 at worst, against 0.025 and 0.084 for
  the closest rule, the nearest point of the triangles, which draws wedges and steps. The table
  generator refuses a top that is not star shaped from the focal point. So a mix only holds shades
  that neighbour each other, and paper does not mix with a strong red. The mix is set in lightness,
  not in light.

  Two steps then ease the minor shades, each a blend between the plain mix and a simpler one by a
  smoothstep of a distance, so the mix point moves by at most that distance:
  - solid, on by default: d is the distance of the mix's point, the target or the point of the focal
    rule, from the shade of the largest weight. Within T1 of it the pixel takes that shade alone,
    and at T1 + W1 the mix is plain again. This makes soft discs around the shade points and leaves
    the edges alone. T1 is 8 and W1 is 16, in 1/4096 of lightness.
  - line, off by default: e is the distance of the target from the nearest edge of its triangle.
    Within T2 of it the pixel takes the two shades of that edge alone, at the point of the edge
    nearest the target, and at T2 + W2 the mix is plain again. It draws a band of another texture
    along every edge.
  T 0 and W 0 turn a step off, and both off give the plain mix.

The console command `dither` prints the mode and both steps, and `dither diffuse`, `dither triangle`
or `dither nearest` sets the mode. The next page drawn uses it, and `load N` draws page N again.
The mode is kept across deep sleep and goes back to `nearest` when power is removed. `dither solid
T W` and `dither line T W` set the steps, 0 to 1024 each. They are kept in RAM only, so a deep sleep
or a power up starts again at solid 8 16 and line 0 0.

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
