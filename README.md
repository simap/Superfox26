# browser26

ESP-IDF project for the Supercon 2026 badge. Fetches a list of web pages in turn, renders each with
the litehtml fork into a 960 x 672 tone frame and draws it on the three pigment panel through the
measured tone map: 103 measured tones from paper through grays and pinks to black and red, played as
pull patterns through the epd97 driver. One page per hour by default. WiFi credentials and the page list are
set over the USB serial console, so a Web Serial page can configure a badge.

It combines two pieces of work in this tree:

- The epd97 panel driver with its phase 1 and seq waveform tables, copied into
  `components/epd97`, the board settings from its sdkconfig.defaults, the tone map `main/tonemap.h`
  with `main/tonemap_entries.inc`, the squeeze `main/tonemap_squeeze.h` and the draw plan
  `main/tone_plan.cpp`.
- `browsers`: the litehtml fork, https://github.com/simap/litehtml branch `epaper`, a submodule at
  `components/litehtml/fork` built through `components/litehtml`, and the three-color container
  from `browsers/litehtml/harness/device`
  ported to the device in `main/render.cpp`. `browsers/BADGE-INTEGRATION.md` is the design the port
  follows.

## Build and flash

The litehtml fork is a git submodule. Clone with `git clone --recursive`, or run
`git submodule update --init` in an existing clone.

ESP-IDF v6.0.2 through the tools scripts, which run the IDF venv python on idf.py without activating
the environment in your shell.

```
tools/idf.sh build
tools/flash.sh [PORT]             # bootloader, partition table, app and the data partition image
tools/idf.sh -p PORT app-flash    # app only: keeps urls.txt on the data partition
tools/monitor.sh [PORT] [REGEX]   # exits on "PAGE DONE" by default, or on a crash
```

The first build makes a Python venv with littlefs-python for the partition image.

### Web installer

`webinstall/index.html` flashes a badge and sets its WiFi network and page list from a browser, over
Web Serial (Chrome or Edge) or WebUSB. It needs two files on one web server: the page and
`webinstall/firmware.bin`, the merged flash image. The page loads esptool-js 0.7.1 from unpkg.com.

```
tools/build_webinstall.sh                  # build, then write webinstall/firmware.bin
python3 -m http.server -d webinstall 8000  # then open http://localhost:8000
```

The browser gives a page the serial port only on https or on localhost. A full install writes the
bootloader, the partition table and every partition, which erases nvs and the page list. An update
writes the bootloader, the partition table and the app. The page does not verify the written data.

## Storage

| partition | contents | written by |
|---|---|---|
| nvs | WiFi SSID and password | the WiFi driver, from the `wifi` console command |
| data, LittleFS 4.44 MB at 0x390000 | `fonts/` (the font pack, 0.99 MB), `urls.txt`, `tabs/<hash>.bin` | fonts by `idf.py flash` (build/data.bin), urls.txt by the console, the rest by the browser |

`urls.txt` has one page per line: `URL OFFSET SECONDS`. OFFSET is the first document row shown,
clamped to the document height. SECONDS is the time from the start of that page to the next, 0 for
the Kconfig default. Without `urls.txt` the list is `main/urls.h`.

### Fonts

The font pack in `data/fonts` is checked in, so the firmware build needs no Node.
`tools/stage_fonts.py` copies it into build/fsroot at build time. It holds 13 faces: sans, serif
and mono in regular, bold, italic and bold italic, subsets of the Liberation fonts 2.1.5 renamed B26
Sans, B26 Serif and B26 Mono (their licence reserves the name Liberation), and `fallback.otf`, a subset
of GNU Unifont 18.0.01 for the code points the text faces lack. The licence texts are in the pack.
`manifest.json` records each file's source, hash, code points and options.

The text faces hold Basic Latin, Latin-1, Latin Extended-A and the web typography of
`tools/fontpack/fontpack.json`, with TrueType hinting kept and the layout tables dropped. The
fallback holds Latin Extended-B, IPA, Greek, Cyrillic, Armenian, Hebrew, Arabic, Latin Extended
Additional, U+2000 to U+2BFF (punctuation, arrows, math, box drawing, shapes), kana and fullwidth
forms. FreeType opens a face on first use, as a stdio stream, so the outlines stay in flash. A missing
face falls back to another of its class, then to sans regular. Only `sans.ttf` is required. A page
without a font-family rule gets the serif faces (`get_default_font_name` in `main/render.cpp`).
Glyph rasterising and the text weight settings are in `main/text_raster.h`.

`tools/fontpack` builds the pack: a TypeScript tool for Node 23.6 or later, with HarfBuzz's subsetter
(harfbuzzjs) as its one dependency. It downloads the pinned sources into `tools/fontpack/cache` and
checks their SHA-256.

```
cd tools/fontpack
npm ci
node fontpack.ts build    # writes data/fonts from fontpack.json
npm test                  # fails when data/fonts differs from what fontpack.json builds
```

`B26_FONT_DIR` in the environment when a build directory is configured picks another pack directory,
for example one built from your own copy of `fontpack.json` with `--config`. The firmware opens the
faces by the file names of `main/fonts.cpp`.

The data partition overlaps the stock vfs partition of the badge firmware, so flashing this project
overwrites that LittleFS image.

## Console

USB Serial/JTAG, the badge's only port. One command per line, CR or LF. Replies start with `OK` or
`ERR`. Log lines share the port.

The badge also prints an event line on its own when something changes, so a page on the port does
not need to poll:

```
EVT state=STATE index=N         the browser's state changed. Both fields are as in status
EVT wifi connected=1 ip=ADDRESS WiFi got an address
EVT wifi connected=0            WiFi lost its link
```

```
help
status                  WiFi, IP, radio on or off, rail voltage, USB or battery, wake cause,
                        page list source, cycle state, build date and ELF hash, heap
wifi                    the stored SSID
wifi SSID [PASSWORD]    store in NVS and connect, or at the next page while the radio is off.
                        Quote an SSID or password with spaces.
scan                    list the 2.4 GHz networks in range, about 3 s
get URL                 fetch up to 64 KB and print status, size, timing, first 200 bytes.
                        Refused while the radio is off or WiFi is not connected
urls                    the list, numbered from 1: URL, scroll offset, seconds
url add URL [OFFSET [SECONDS]]   offset in document pixels, seconds 0 = CONFIG_B26_CYCLE_SECONDS
url put N URL [OFFSET [SECONDS]] replace page N
url set N OFFSET [SECONDS]
url move FROM TO        move a page to another place in the list
url del N
url clear               remove urls.txt, back to the built-in list
next                    render the next page now
prev                    render the page before the one shown
load N                  render page N now
```

The panel is drawn only for a rendered page or the low battery notice, since a draw takes about 21 s on a badge (measured 2026-11-18, from the end of the page load).
On USB the radio stays on. On battery the radio is off during the draw. On USB, without WiFi or URLs
the task waits and logs once. A failed page is logged, the previous page stays on the panel, and the
next page follows after 10 s. On battery these three cases sleep 300 s (Power).

The rail read of `status` can fail while the radio is on, because the rail sensor is on ADC2, which
WiFi uses. The line then shows `rail_mv=failed`.

## Power

The badge has no USB detect signal. A reading of the system rail (`BAT_SENS`, GPIO 14) decides USB or
battery. Readings are taken at boot before WiFi starts, after each panel draw, and at the end of a
cycle without a page. On USB that last reading is taken when the reading before is more than 60 s
old, so a badge that is unplugged while it has no WiFi, no URLs or only failing pages goes to sleep
within about a minute. The rail sensor is on ADC2, which WiFi uses, so a read with the radio on can
fail, and a failed read counts as USB. A battery result from a read with the radio on is read again
with the radio off before the badge acts on it. Thresholds (`main/power_logic.h`):

| rail | meaning |
|---|---|
| above 4350 mV | USB. A full cell gives 4200 mV at most |
| below 3400 mV on battery | battery low |
| 3600 mV on battery | a low battery is ok again from here |

On USB the badge stays awake with the radio on, and the LEDs count down to the next page. WiFi power
save is off on USB and on battery.

On battery the radio goes off before the draw, and the badge deep sleeps after the draw for the rest
of the page time. No WiFi connection within 20 s or an empty URL list sleeps 300 s and then tries
the same page again. After a failed page the next page loads at once. When every page of the list has failed, the badge
sleeps 300 s. In each case the page on
the panel stays. A badge that is unplugged starts to sleep after its next draw. A badge that is
plugged in while awake stays awake.

Wake buttons, on battery:

- timer: the next page.
- User B: the tab selection opens at the next tab.
- Menu -: the tab selection opens at the tab on the panel.

A button wake lights one LED at once. The tab selection is the same as in the countdown on USB: all
four buttons work while it is open, User A and User B step the tab, the LEDs show it, and the page
loads once the buttons have been quiet for 1.5 s. WiFi connects in that time.

A press of User A or User B during a render or a draw is kept. When the draw ends the selection opens
with those steps applied, on USB in the countdown and on battery before any sleep.

A wake button that is held down when the badge goes to sleep is not armed for that sleep. A sleep that
waits for a button first waits up to 10 s for a held button to be released. If both stay down, the
timer wakes the badge after an hour instead.

Low battery: the badge draws a notice ("Battery low, connect USB, then press User B or Menu -") once and
sleeps until a button wakes it. Plugging in USB does not wake the badge. With a low battery at boot the
radio does not start, so a wake while the battery is still low goes back to sleep without a draw and
without WiFi. On USB, or once the rail reaches 3600 mV, the next cycle draws a page over the notice.

Welcome: with no WiFi network stored, the badge draws a welcome notice once. It tells the user to
connect USB, press User B and set the network on the serial console. On battery the badge then sleeps
until a button wakes it. On USB it stays awake and waits for the console. The notice shows the 100 px
logo to the left of its heading, from `logo/panel_100.png`.

The page index, the page on the panel, the two low battery flags and the welcome flag are kept in RTC memory across
deep sleep and across a reset. A power on starts at the first page. The page index moves on before a
page renders, so after a crash the reset continues with the page after the one that crashed.

Cut draw: a panel draw that stops part way leaves the panel with a net DC voltage, and repeated cut
draws can damage it. A word in RTC memory marks a draw as running and as complete. When a reset, a
brownout or a crash cuts a draw, the next boot finds the word at running and halts, on USB and on
battery: the panel supply stays off, nothing else starts, and the first LED flashes red once a second.
The halt lasts until power is removed, with the power switch or an empty battery. A firmware flash
resets the badge and does not clear the word, so a flash during a draw also ends in this halt. The CPU
light sleeps between the flashes.

Brownout: after a brownout reset on battery the badge draws nothing and starts no WiFi. It sleeps until
a button wakes it.

Limits:

- User A and Menu + cannot wake the badge. On battery they work only in the tab selection, after User B
  or Menu - woke the badge.
- A weak USB supply with no host (rail near 4.3 V) reads as battery, and the badge then sleeps between
  pages.
- A failed rail read counts as USB, so the badge then never sleeps.

`sdkconfig.defaults` holds `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y`, which makes the
bootloader skip the app image check on a wake from deep sleep. The project has no OTA and no secure
boot. The defaults apply only when the build creates a new `sdkconfig`. A tree that already has one
needs the line set there, through `idf.py menuconfig` or by hand. The setting is in the bootloader, so
it takes effect only after `tools/flash.sh` writes the bootloader. `app-flash` does not.

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

The Superfox26 logo is at the left end of the bar, at 32 px on the row gray, and the tabs share the
width to its right, from `logo/panel_32.png`.

The two panel PNGs are built into the app (`EMBED_FILES` in main/CMakeLists.txt) and reach the engine
as data URLs. They are renders of `logo/superfox26_panel.svg`, the logo in colours that each land on
one tone entry, so its fills draw without dither: `#ea0000` for the red, `#fcd0d0` for the pink, black
and paper white. Their edges are antialiased, with alpha. `probes/image_dither` runs the dither on an
image and `probes/solid_red` on a solid fill.

The bar is HTML (`tabs_html` in tabs.cpp) rendered by the engine into the top rows, with the icons as
data URLs. For it the container gained rounded corners: `draw_solid_fill` fills a rounded rectangle
from the layer's radii, and `draw_borders` draws a ring between the outer and inner rounded boxes in the
colour of the first visible side.

## LEDs and buttons

The eight WS2812 LEDs (GPIO 38) are a bar at 3 percent brightness. Amber while a page loads:
one LED at fetch start, two after the HTML, three after parse, four after layout, five after the
window's images are fetched, then one more per third of the draw, full during the panel refresh. Teal during the countdown to the next
page, draining from eight LEDs to one. Off while waiting for WiFi or URLs. Off in deep sleep. On
battery there is no countdown.

Four buttons, by their silk names: User A (GPIO 39), Menu + (GPIO 40), Menu - (GPIO 15) and User B
(GPIO 16). On USB, during the countdown, User A and User B pick a tab: User A moves the selection one
tab back, User B one tab forward, wrapping, starting from the page shown. The countdown pauses and
the LEDs turn violet, one LED for the first tab up to all eight for the last. The chosen page loads
1.5 s after the last press. Presses during a load or a draw are ignored. Menu + and Menu - have no
action and are logged only. Menu - and User B wake the badge from deep sleep (Power).

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
6. `reset_string_ids()`, then on USB wait for the rest of the cycle, or for `next`. On battery deep
   sleep for the rest of the cycle.

WiFi is off from the end of step 4 to the end of step 5.

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
Phase 1 opens `EPD_TONE_TABLE_PHASE1` at unit 1 and plays its pulls of 17 frames, phase 2 opens
`EPD_TONE_TABLE_SEQ` at unit 4 and plays its pulls of 10 frames, at 23.7 ms a frame. At each pull every
pixel's plane bits are its entry's code from `tone_plan_code()`. A pull skips the plane send only when
it is not the first of its phase and every entry in the frame repeats code 0 or 3. The plan pays back
each entry's DC on the same pixel and holds every pixel within the stock waveform's push limits. A
page with reds plays 14 + 27 pulls in the host harness, about 12 s of BUSY time. On a panel without the
tables (not driver B, or its OTP read failed) the frame is reduced to the three stock colours, red for
an entry with chroma above 0.09, else black for L below 0.5, else white, and refreshed with the
panel's own waveform.

On a badge the whole draw takes about 21 s, from the end of the page load to the finished panel
(2026-11-18). That is more than the 12 s of BUSY time of the pulls. The split between the dither, the
session opens and the pulls is not measured: the `draw ok` log line gives the pulls and the total.

Not yet measured on hardware: the dither time per page, the look of the neutral gap
between the L 0.5965 and L 0.469 entries, and the colour of antialiased text edges.

The size cache (`sizecache.cpp`) holds up to 512 URL hashes with kind and natural size, 12 bytes each,
in 128 sets of 4 with random replacement within a set. The 6144 byte table is in RTC slow memory
(`RTC_NOINIT_ATTR`). Deep sleep, a software reset and a panic restart keep it. The magic word of
`rtcstate.cpp` covers all RTC state of the browser. When it is wrong at boot, as after a power on, the
table starts empty. A page seen before needs no image traffic for layout. Failures are not cached. An image fetch that fails, probe or file, is tried once more after
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
