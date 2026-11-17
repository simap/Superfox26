# browser26

ESP-IDF project for the Supercon 2026 badge. Fetches a list of web pages in turn, renders each with
the litehtml fork into a 960 x 672 tone frame and draws it on the three pigment panel through the
measured tone map: 103 measured tones from paper through grays and pinks to black and red, played as
pull patterns through the epd97 driver. One page per minute by default. WiFi credentials and the page list are
set over the USB serial console, so a Web Serial page can configure a badge.

It combines two pieces of work in this tree:

- The epd97 panel driver with its phase 1 and seq waveform tables, copied into
  `components/epd97`, the board settings from its sdkconfig.defaults, the tone map `main/tonemap.h`
  with `main/tonemap_entries.inc`, the squeeze `main/tonemap_squeeze.h` and the draw plan
  `main/tone_plan.cpp`.
- `browsers`: the litehtml fork (`browsers/litehtml/fork`, branch `epaper`) built by path through
  `components/litehtml`, and the three-color container from `browsers/litehtml/harness/device`
  ported to the device in `main/render.cpp`. `browsers/BADGE-INTEGRATION.md` is the design the port
  follows.

## Build and flash

ESP-IDF v6.0.2 through the tools scripts, which run the IDF venv python on idf.py without activating
the environment in your shell.

```
tools/idf.sh build
tools/flash.sh [PORT]             # bootloader, partition table, app and the data partition image
tools/idf.sh -p PORT app-flash    # app only: keeps urls.txt on the data partition
tools/monitor.sh [PORT] [REGEX]   # exits on "PAGE DONE" by default, or on a crash
```

The first build makes a Python venv with littlefs-python for the partition image.

## Storage

| partition | contents | written by |
|---|---|---|
| nvs | WiFi SSID and password | the WiFi driver, from the `wifi` console command |
| data, LittleFS 4.44 MB at 0x390000 | `fonts/*.ttf`, `urls.txt`, `imgsize.bin`, `tabs/<hash>.bin` | fonts by `idf.py flash` (build/data.bin), urls.txt by the console, the rest by the browser |

`urls.txt` has one page per line: `URL OFFSET SECONDS`. OFFSET is the first document row shown,
clamped to the document height. SECONDS is the time from the start of that page to the next, 0 for
the Kconfig default.

`tools/stage_fonts.py` copies Arial.ttf, Arial Bold.ttf, Courier New.ttf and Courier New Bold.ttf from
the macOS supplemental fonts into build/fsroot at build time. `B26_FONT_DIR` and `B26_FONT_SANS`,
`B26_FONT_BOLD`, `B26_FONT_MONO`, `B26_FONT_MONO_BOLD` in the environment select other files. FreeType
opens them as stdio streams, so they stay in flash. Serif maps to sans and italic to upright. Without
`urls.txt` the list is `main/urls.h`.

The data partition overlaps the stock vfs partition of the badge firmware, so flashing this project
overwrites that LittleFS image.

## Console

USB Serial/JTAG, the badge's only port. One command per line, CR or LF. Replies start with `OK` or
`ERR`. Log lines share the port.

```
help
status                  WiFi, IP, page list source, cycle state, heap
wifi                    the stored SSID
wifi SSID [PASSWORD]    store in NVS and connect. Quote an SSID or password with spaces.
scan                    list the 2.4 GHz networks in range, about 3 s
get URL                 fetch up to 64 KB and print status, size, timing, first 200 bytes
urls                    the list, numbered from 1: URL, scroll offset, seconds
url add URL [OFFSET [SECONDS]]   offset in document pixels, seconds 0 = CONFIG_B26_CYCLE_SECONDS
url set N OFFSET [SECONDS]
url del N
url clear               remove urls.txt, back to the built-in list
next                    render the next page now
prev                    render the page before the one shown
```

The panel is drawn only for a rendered page, since a draw takes about 12 s. Without WiFi
or URLs the task waits and logs once. A failed page is logged, the previous page stays on the panel,
and the next page follows after 10 s.

## Tab bar

Rows 0 to 37 of the panel are a tab bar, one tab per page in the list, in two tones: a mid gray row
with light gray tabs, the current tab paper white like the page under it. The border is 1 px black
outside and a 1 px bevel inside, white on the top and left, the row gray on the right. The fills are
grays that land exactly on a tone entry (`tones_solid_gray`), so they render solid. The page window is
the 634 rows below it, and that is also the page's viewport height. Each tab shows the page's favicon
at 32 px and its title. Both come from the page after redirects when it is rendered: the title from
`<title>`, the icon from the `<link>` candidates (icon, shortcut icon, apple-touch-icon,
apple-touch-icon-precomposed, the size nearest 32 px first, SVG skipped), then `favicon.ico`,
`favicon.png` and `apple-touch-icon.png` at the page's origin. The format is sniffed from the bytes:
ICO (the entry nearest 32 px, PNG or BMP with its AND mask), PNG, JPEG, GIF, BMP or WebP at any size,
box filtered to 32x32. A page that is itself an image gets a thumbnail of that image, no fetch. When
every candidate fails, the tab gets a placeholder, a black-bordered box with the host's first letter,
and the candidates are tried again on the next visit. A real icon is never fetched again. Title and
icon are stored in `/data/tabs/<hash of the configured URL>.bin` as text plus a 32x32 32-bit BMP,
written through a temporary file and a rename, so every tab shows from the first boot after a page
has been seen once. An icon stored at another size by an older build is shown and fetched again. A
page never seen shows its host name and no icon.

The bar is HTML (`tabs_html` in tabs.cpp) rendered by the engine into the top rows, with the icons as
data URLs. For it the container gained rounded corners: `draw_solid_fill` fills a rounded rectangle
from the layer's radii, and `draw_borders` draws a ring between the outer and inner rounded boxes in the
colour of the first visible side.

## LEDs and buttons

The eight WS2812 LEDs (GPIO 38) are a bar at 3 percent brightness. Amber while a page loads:
one LED at fetch start, two after the HTML, three after parse, four after layout, five after the
window's images are fetched, then one more per third of the draw, full during the panel refresh. Teal during the countdown to the next
page, draining from eight LEDs to one. Off while waiting for WiFi or URLs.

During the countdown the buttons pick a tab: User A (GPIO 39) moves the selection one tab back,
User B (GPIO 16) one tab forward, wrapping, starting from the page shown. The countdown pauses and
the LEDs turn violet, one LED for the first tab up to all eight for the last. The chosen page loads
1.5 s after the last press. Presses during a load or a draw are ignored.

## Per page

1. `fetch_url` gets the HTML (limit 700 KB) over esp_http_client. TLS does not verify the server.
2. `document::createFromString` with `static_release_after_cascade`. During parse the engine calls
   `import_css` for every stylesheet, fetched over HTTP, and `load_image` for every image. Every image
   gets its natural size from the size cache or from a 16 KB header probe (a Range request, 64 KB once
   more for a JPEG whose frame header comes later), because layout uses it for table and flex sizing
   even when CSS gives the dimensions. No image file is fetched during parse.
   A body that is itself an image file (PNG, JPEG, WebP, GIF or BMP by signature) is wrapped in a
   one-image page: scaled to fit the window with its aspect kept, centred on a black page,
   and the fetched bytes are used as the image, so the 700 KB page limit applies and not the 300 KB
   image limit. The tab title is the file name.
   `check_memory_budget` refuses a phase that would leave under 2.5 MB of PSRAM after the bytes the
   engine declares, because the phases after the last check have no check of their own.
3. `render(960)`, then a collect pass: one `draw` over the window with painting off records which
   images the window needs and down to which row. Those files are fetched (300 KB per image, 1 MB per
   page), decoded at draw time, and freed after the last band that uses them. A file whose size differs
   from the probed or cached one corrects the cache and, when layout used that size, triggers one more
   layout and collect. Off-screen images are never downloaded.
4. 634 rows from the page's offset are drawn in bands of 64 rows into an RGB band buffer and dithered
   row by row into rows 38 to 671 of the tone frame, one tone map entry index per pixel (`tones.cpp`).
   The tab bar is rendered into rows 0 to 37 after the page. Text is antialiased by FreeType and
   blended by coverage, so glyph edges carry real tones.
5. `tones_draw()` plays the frame onto the panel: the pulls of phase 1, then the pulls of phase 2,
   with every pixel playing its entry's code in lockstep. `EPD_KEEP_SUPPLY=1` (top-level
   CMakeLists.txt) keeps the panel supply on across the phase 1 to phase 2 table switch, which saves
   the 1.5 s off floor and the settle of the switch.
6. `reset_string_ids()`, the size cache is saved if it changed, then wait for the rest of the cycle, or
   for `next`.

## Tones

The panel is driven through the measured tone map (`main/tonemap.h` and
`main/tonemap_entries.inc`). Each of the 103 entries is a measured tone in Oklab L, a, b, on the scale
where the black reference is L 0 and the paper L 1, and the pattern of pulls that makes it. A pattern
with no r or R is neutral: black and white only, a and b 0. There are 22 neutral entries.

A pixel's target is its sRGB colour in Oklab, brought into the panel's range by `tonemap_squeeze()`
(`main/tonemap_squeeze.h`), with L held at the black entry's or above. The squeeze keeps only the part
of the chroma along the hue of sRGB red, so a blue or a green becomes the gray of its lightness and link
blue reads as a dark gray. A target with chroma below 0.001 after the squeeze is neutral. A neutral
pixel mixes only the two neutral entries around its L and holds its error inside that pair. Within
0.002 in L of a neutral entry it takes that entry alone and leaves no error, so black text, paper and
solid UI fills stay solid. Any other pixel takes the nearest entry by the Oklab distance with a and b
weighted 2 (`TONEMAP_CIE_CHROMA_WEIGHT`), searched over the 48 nearest neighbours of the entry nearest
its target. A 1024 slot cache keyed on the RGB holds each colour's target and nearest entry. The
decision between the two nearest entries is moved off their midpoint by a 64x64 blue noise table, up
to a quarter of their gap, which breaks Atkinson's regular checkerboards without moving a pixel that
sits on an entry. The carried error is in linear light: target plus carried error minus the chosen
entry, each entry's light taken from its L, a, b. It is clamped to 0.25 of the paper's light per
channel and diffused with the Atkinson kernel, serpentine. A neutral pixel takes the luminance of the
error it receives. Floyd-Steinberg, Jarvis and Stucki are selectable in tones.cpp. The host harness
sweep in `host_tests/tones` compares them.

`host_tests/tones` (`make run`) renders gray and red ramps, two colour wedges, a text block and a
sample image through the same dither on the host and writes PNG previews in the entries' colours plus
a report of entry use, pulls and deviation from the target. It also plays each frame through
`tones_draw()` into a model of the driver and checks every pull against the plan.

Drawing follows `main/tone_plan.h`. `tone_plan_build()` makes the plan of the entries in the frame.
Phase 1 opens `EPD_VERB_TABLE_PHASE1` at unit 1 and plays its pulls of 17 frames, phase 2 opens
`EPD_VERB_TABLE_SEQ` at unit 4 and plays its pulls of 10 frames, at 23.7 ms a frame. At each pull every
pixel's plane bits are its entry's code from `tone_plan_code()`. A pull skips the plane send only when
it is not the first of its phase and every entry in the frame repeats code 0 or 3. The plan pays back
each entry's DC on the same pixel and holds every pixel within the stock waveform's push limits. A
page with reds plays 14 + 27 pulls in the host harness, about 12 s of BUSY time. On a panel without the
tables (not driver B, or its OTP read failed) the frame is reduced to the three stock colours, red for
an entry with chroma above 0.09, else black for L below 0.5, else white, and refreshed with the
panel's own waveform.

Not yet measured on hardware: the dither time per page, the draw time, the look of the neutral gap
between the L 0.5965 and L 0.469 entries, and the colour of antialiased text edges.

The size cache (`sizecache.cpp`) holds up to 512 URL hashes with kind and natural size, 24 bytes each,
least recently used eviction, in `/data/imgsize.bin`. A page seen before needs no image traffic for
layout. Failures are not cached. An image fetch that fails, probe or file, is tried once more after
250 ms unless the server answered 403, 404 or 410.

The fetch layer reads the body itself (open, fetch_headers, read) so a byte cap stops the transfer.
A complete body leaves the connection open for the next request to that host; a capped one drops it.

## Images

| format | decoder | notes |
|---|---|---|
| PNG | libpng (registry) | all color types, 8-bit alpha blended into the band, interlaced files decoded whole |
| JPEG | libjpeg-turbo (registry) | baseline and progressive, DCT scaling then box filter |
| WebP | libwebp 1.6.0 decoder, `components/webp` | lossy, lossless, alpha. Animated files are sized, not drawn. Skipped when the natural-size decode would not leave 1 MB of PSRAM |
| GIF | own LZW decoder, `image_legacy.cpp` | first frame, transparency, interlace, local color tables. Truncated files show the decoded part |
| BMP | `image_legacy.cpp` | 1, 4, 8, 16, 24 and 32 bit, BI_RGB and BI_BITFIELDS, top-down and bottom-up. No RLE |
| SVG | none | skipped before fetch. See the SVG survey summary in the session notes: PlutoSVG is the candidate |

Images are decoded at the drawn size. GIF and BMP refuse files above 16 megapixels.

## Not done

- Progressive JPEG decodes through libjpeg-turbo with its full coefficient buffer. The budget check
  does not cover the decode, so a large progressive image on a heavy page can still run out of PSRAM.
- Not run on hardware yet.
