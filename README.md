# browser26

ESP-IDF project for the Supercon 2026 badge. Fetches a list of web pages in turn, renders each with
the litehtml fork into the 960 x 672 black, white and red planes of the epd97 driver and refreshes the
panel on its own waveform. One page per minute by default. WiFi credentials and the page list are
set over the USB serial console, so a Web Serial page can configure a badge.

It combines two pieces of work in this tree:

- The epd97 panel driver and framebuffer, copied as is into `components/epd97`, with the
  board settings from its sdkconfig.defaults. Only the planes and `refresh_display(EPD_SLOW)` are used.
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
```

The panel is refreshed only for a rendered page, since a refresh takes 32 s or more. Without WiFi
or URLs the task waits and logs once. A failed page is logged, the previous page stays on the panel,
and the next page follows after 10 s.

## Tab bar

Rows 0 to 43 of the panel are a tab bar, one tab per page in the list, the current page filled black.
The page window is the 628 rows below it, and that is also the page's viewport height. Each tab shows
the page's favicon at 16 px and its title. Both come from the page when it is rendered: the title from
`<title>`, the icon from the best `<link rel=icon>` (16 or 32 px raster first, apple-touch-icon last,
never SVG), else `/favicon.ico`. ICO files are parsed for their PNG or BMP entry. Title and icon are
stored in `/data/tabs/<url hash>.bin` as text plus a 16x16 32-bit BMP, so every tab shows from the
first boot after a page has been seen once. A page never seen shows its host name and no icon.

The bar is HTML (`tabs_html` in tabs.cpp) rendered by the engine into the top rows, with the icons as
data URLs. For it the container gained rounded corners: `draw_solid_fill` fills a rounded rectangle
from the layer's radii, and `draw_borders` draws a ring between the outer and inner rounded boxes in the
colour of the first visible side.

## LEDs and buttons

The eight WS2812 LEDs (GPIO 38) are a bar at 10 percent brightness. Amber while a page loads:
one LED at fetch start, two after the HTML, three after parse, four after layout, five after the
window's images are fetched, then one more per third of the draw, full during the panel refresh. Teal during the countdown to the next
page, draining from eight LEDs to one. Off while waiting for WiFi or URLs.

User A (GPIO 39) and User B (GPIO 16) start the next page during the countdown. Presses during
a load or a refresh are ignored.

## Per page

1. `fetch_url` gets the HTML (limit 700 KB) over esp_http_client. TLS does not verify the server.
2. `document::createFromString` with `static_release_after_cascade`. During parse the engine calls
   `import_css` for every stylesheet, fetched over HTTP, and `load_image` for every image. Every image
   gets its natural size from the size cache or from a 16 KB header probe (a Range request, 64 KB once
   more for a JPEG whose frame header comes later), because layout uses it for table and flex sizing
   even when CSS gives the dimensions. No image file is fetched during parse.
   A body that is itself an image file (PNG, JPEG, WebP, GIF or BMP by signature) is wrapped in a
   one-image page: scaled to fit the window with its aspect kept, centred, with a 3 px black border,
   and the fetched bytes are used as the image, so the 700 KB page limit applies and not the 300 KB
   image limit. The tab title is the file name.
   `check_memory_budget` refuses a phase that would leave under 2.5 MB of PSRAM after the bytes the
   engine declares, because the phases after the last check have no check of their own.
3. `render(960)`, then a collect pass: one `draw` over the window with painting off records which
   images the window needs and down to which row. Those files are fetched (300 KB per image, 1 MB per
   page), decoded at draw time, and freed after the last band that uses them. A file whose size differs
   from the probed or cached one corrects the cache and, when layout used that size, triggers one more
   layout and collect. Off-screen images are never downloaded.
4. 628 rows from the page's offset are drawn in bands of 64 rows into an RGB band buffer and
   quantized row by row into panel rows 44 to 671 of the bw and red planes. The tab bar is rendered
   into rows 0 to 43 after the page. The quantizer is the one from branch `dither` of
   the device container: Atkinson error diffusion by default, Floyd-Steinberg and 8x8 Bayer selectable
   by the `DITHER_MODE` constant in render.cpp. Blue noise is not ported; it needs a precomputed 4 KB tile.
5. `refresh_display(EPD_SLOW)`. Red needs the panel's own waveform, 34 to 51 s depending on the panel.
6. `reset_string_ids()`, the size cache is saved if it changed, then wait for the rest of the cycle, or
   for `next`.

The size cache (`sizecache.cpp`) holds up to 512 URL hashes with kind and natural size, 24 bytes each,
least recently used eviction, in `/data/imgsize.bin`. A page seen before needs no image traffic for
layout. Failures are not cached. An image fetch that fails, probe or file, is tried once more after
500 ms unless the server answered 403, 404 or 410.

The fetch layer reads the body itself (open, fetch_headers, read) so a byte cap stops the transfer.
A complete body leaves the connection open for the next request to that host; a capped one drops it.

## Images

| format | decoder | notes |
|---|---|---|
| PNG | libpng (registry) | all color types, alpha to a dithered mask, interlaced files decoded whole |
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
