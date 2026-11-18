# Superfox26

![Superfox26](logo/superfox26.png)

Superfox26 is a web browser for the Supercon X badge. The badge is the Hackaday Supercon 2026
conference badge. It has an ESP32-S3 and a 960 by 672 e-paper panel with black, white and red
pigment. Superfox26 fetches web pages over WiFi, lays each one out with a fork of the litehtml
engine, dithers it to 103 shades the author measured on the panel, and draws it. It steps through a
list of pages. A page in `webinstall/` installs and configures the firmware over Web Serial.

This README covers Superfox26 only. For the badge hardware, the stock firmware and how to restore
it, see the official badge repository,
[Hack-a-Day/2026-Billboard_Badge](https://github.com/Hack-a-Day/2026-Billboard_Badge).

Superfox26 is not affiliated with Hackaday.

## Name

The ESP-IDF project and its top directory are named `browser26`. Build options use the prefix
`B26_`.

## Warnings

A full install overwrites the badge. It erases the `nvs` partition, which holds the WiFi
credentials, and rewrites the `data` partition, which holds the fonts and the page list (see
`partitions.csv`). It replaces the stock firmware and its file system. To return to the stock
firmware, flash it from the official badge repository above.

A reset or a power loss during a panel draw stops the firmware at the next boot. A draw cut part way
leaves the panel with a net DC voltage, and repeated cut draws can damage it. At the next boot the
firmware detects the cut draw, keeps the panel supply off, and holds there. The first LED flashes
red about once a second. Switch the badge off and on to clear it. The mechanism is in
`main/rtcstate.cpp` and `main/main.cpp`.

HTTPS server certificates are not verified. `sdkconfig.defaults` sets `CONFIG_ESP_TLS_INSECURE` and
`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`. Any page the badge fetches is treated as public.

## Install with the web installer

The web installer is the page in `webinstall/`. It installs the firmware and sets WiFi and the page
list over Web Serial. Web Serial needs a Chromium-based browser, such as Chrome or Edge, and a
secure context, so HTTPS or localhost.

`webinstall/firmware.bin` is a build output and is not in the repository. Build and serve the page:

1. Build the merged image: `tools/build_webinstall.sh`. It writes `webinstall/firmware.bin`.
2. Serve the `webinstall/` directory over HTTPS or from localhost, for example
   `python3 -m http.server` run inside `webinstall/`.
3. Open the page, connect the badge with a USB cable, and install.

The page lets the user:

- Install the firmware, full or as an update that keeps the WiFi credentials and the page list.
- Set the WiFi network name and password, scan for networks, and forget the stored network.
- Add, edit, reorder and remove pages, each with a scroll offset and a time.
- Draw a chosen page now, step to the next or previous page, or switch to the built-in list.
- Open a serial log and send a console command.

## Use

After setup the badge fetches each page in the list in turn, draws it, and holds it for the page's
time before the next. A page with no time set holds for the default, one hour
(`B26_CYCLE_SECONDS`).

The buttons:

- User A chooses the previous page.
- User B chooses the next page.
- Menu + and Menu - hold the page on the panel and stop the timer. Press either again to resume.

Each press of User A or User B moves the choice by one page. The LED bar shows the choice as an
offset from the page on the panel. The badge loads the chosen page once the buttons are quiet for
about one and a half seconds. A panel draw already running finishes first. A page the choice moves
past before its draw is abandoned.

On battery, User B and Menu - wake the badge from deep sleep. User B wakes to the next page. Menu -
wakes to the page on the panel. A wake opens the same selection window. A held page keeps the badge
asleep until a button, with no timer wake.

The LED bar shows the current activity:

| The LED bar shows | Meaning |
|---|---|
| Amber, a growing bar | A page is fetched and laid out. |
| A short amber flash | A WiFi connection failed. |
| Teal, a draining bar | The countdown to the next page, on USB power. |
| Teal on the last LED, or a full teal bar | The page is held. Press Menu to resume. |
| A violet, white, blue or green run | The chosen page during the selection window. The run grows from the left end for a forward choice and from the right end for a back choice, one LED per page of offset. Past seven it starts again in the next colour. |
| The two middle LEDs violet | The choice is the page on the panel, so it stays. |
| The chosen run blinking | The badge is loading or drawing the chosen page. |
| The first LED red, a short flash | A press with no page list. |
| The first LED red, flashing about once a second | The badge is halted after a cut draw. Switch it off and on. |
| Dark | Waiting for WiFi, an empty list, or between pages. |

On USB power the badge stays awake, keeps the radio on, and runs the serial console. On battery the
radio is off during the panel draw, and the badge deep sleeps between pages to save the cell. It
wakes on a timer for the next page or on a button. When the battery is low the badge draws a notice
and sleeps until a button wakes it on USB power.

## Build from source

Prerequisites:

- ESP-IDF v6.0.2, on macOS or Linux, installed with the Espressif installer (`eim`) or `install.sh`.
  The scripts in `tools/` find it.
- The Xtensa toolchain, installed by ESP-IDF.

Clone with submodules, since the litehtml fork is a submodule:

```
git clone --recurse-submodules <repository-url>
```

For a clone that already exists:

```
git submodule update --init --recursive
```

Build and flash:

- `tools/idf.sh build` builds the firmware.
- `tools/flash.sh [PORT]` builds and flashes the bootloader, the partition table, the app and the
  data partition image. The port defaults to the badge's first USB serial port.
- `tools/build_webinstall.sh` builds the merged image for the web installer.

The scripts run `idf.py` through the Python interpreter of the ESP-IDF virtual environment. They do
not source `export.sh`. The build generates `sdkconfig` from `sdkconfig.defaults` when `sdkconfig`
does not exist. After editing `sdkconfig.defaults`, delete `sdkconfig` to apply the change.

## Console

The serial console runs on the badge's USB port. Open it with a serial terminal at 115200 baud, or
with `tools/monitor.sh [PORT]`, or from the serial log of the web installer.

Every reply line starts with `OK` for success or `ERR` for a failure. A command can print several
`OK` lines. Event lines start with `EVT` and report a change with no command. A Web Serial page
filters on these three prefixes.

| Command | Arguments | What it does |
|---|---|---|
| `help` | | List the commands. |
| `status` | | Print WiFi, power, the page list source, the page index and state, the build and free heap. |
| `wifi` | | Print the stored network name and whether it is connected. |
| `wifi` | `SSID [PASSWORD]` | Store the network and connect. |
| `wifi clear` | | Remove the stored network. |
| `scan` | | List the 2.4 GHz networks found. |
| `get` | `URL` | Fetch the URL and print the status, the size and the first bytes, up to 64 KB. |
| `urls` | | List the pages, each as index, URL, offset and seconds. |
| `url add` | `URL [OFFSET [SECONDS]]` | Append a page. |
| `url put` | `N URL [OFFSET [SECONDS]]` | Replace page N. |
| `url set` | `N OFFSET [SECONDS]` | Change the offset and time of page N. |
| `url move` | `FROM TO` | Move a page to another position. |
| `url del` | `N` | Remove page N. |
| `url clear` | | Discard the stored list and use the built-in list. |
| `next` | | Draw the next page. |
| `prev` | | Draw the previous page. |
| `load` | `N` | Draw page N. |
| `show lowbatt` | | Draw the low battery notice. |
| `show welcome` | | Draw the welcome notice. |

Event lines:

- `EVT state=<name> index=<n>` the state changed. `index` is the next page.
- `EVT wifi connected=1 ip=<address>` WiFi connected.
- `EVT wifi connected=0` WiFi dropped.

States:

| State | Meaning |
|---|---|
| `boot` | Starting up, before the first page. |
| `wait` | A page is shown, waiting for the next. |
| `hold` | A page is held, the timer is stopped. |
| `render` | Fetching and laying out a page. |
| `refresh` | Drawing the panel. |
| `sleep` | Going into deep sleep. |
| `nowifi` | Waiting for a WiFi connection. |
| `nourls` | The page list is empty. |
| `lowbatt` | The battery is low, the notice is shown. |
| `welcome` | No WiFi network is stored, the welcome notice is shown. |

## Pages

The built-in page list is in `main/urls.h`. It holds some of the author's photos and some example
sites. Each entry has a URL, a scroll offset in pixels from the top of the document, and a time in
seconds. A time of 0 means the default (`B26_CYCLE_SECONDS`).

A user changes the list at run time with the `url` console commands or from the web installer. The
run-time list is stored as `urls.txt` on the data partition. The console `url clear` command, or the
web installer, drops the stored list and returns to the built-in list.

To change the list at build time, edit `main/urls.h`. The built-in list is used while `urls.txt`
does not exist.

## How it works

Superfox26 draws one page as a pipeline:

1. Fetch. `main/fetch.cpp` runs an HTTP GET into memory. It follows redirects and keeps a
   connection open for the next request. A page is read up to 700 KB (`PAGE_HTML_MAX`), an image up
   to 300 KB.
2. Parse and layout. The litehtml fork parses the HTML and lays it out at the 960 px panel width.
   The top 38 px of the panel is a tab bar. It shows a window of up to eight tabs around the
   current page, with a count box for the pages hidden on each side. The rest is the page window.
3. Images. `main/image.cpp` decodes PNG, JPEG, GIF (first frame), BMP and WebP. A collect pass finds
   the images in the window, fetches them, and scales them down.
4. Dither. `main/render.cpp` composites the page into a 24-bit RGB band buffer, 64 rows at a time.
   Each row is error diffusion dithered to shade indices in the tone frame.
5. Draw. `tones_draw()` plays the frame onto the panel.

The shades. The panel pigment has three native states: black, white and red. The driver moves a
pixel with short voltage pulses, and a pulse that stops part way leaves the pixel between two native
states. That gives greys and muted colours as well as the three natives. The author measured 103
such shades on the panel. Each page pixel is dithered to the nearest of the 103.

`main/tonemap.h` holds the 103 shades. Each one has a colour in Oklab and a pattern of drive pulses.
The dither picks the nearest shade for each pixel. `main/tone_plan.h` expands each shade's pattern
into panel pulses and balances the net drive on every pixel to zero, so the panel takes no DC
charge. The driver in `components/epd97` plays the pulses.

The panel drive has limits on how long a pixel may be driven in one direction. A pixel driven too
long on one rail can damage the film, so no shade pattern drives a pixel harder than the panel's own
stock waveform does. The limits are `EPD_PUSH_MAX_VDH`, `EPD_PUSH_MAX_VDL` and `EPD_PUSH_MAX_VDHR`
in `components/epd97/epd97_config.h`. They are enforced by the table generator in
`host_tests/tone_tables`, checked at boot by `tone_plan_init()`, and kept by the planner in
`main/tone_plan.h`. The driver does not check them, so anyone who changes a waveform must keep the
generator and the boot check in the build. See `docs/tones.md`.

## Fonts

The firmware carries three text families and one fallback face. Sans, serif and mono are subset from
Liberation fonts 2.1.5 and renamed B26 Sans, B26 Serif and B26 Mono, under the SIL Open Font License
1.1. The fallback face is GNU Unifont 18.0.01, subset, under the same licence. Each family has
regular, bold, italic and bold italic. The fallback face covers code points the text faces lack.

The font pack is in `data/fonts`, built by `tools/fontpack`. To rebuild it, run `npm ci` and then
`npm run build` in `tools/fontpack`. The build stages the pack into the data partition with
`tools/stage_fonts.py`.

## Limits

- No JavaScript. litehtml is a layout engine and runs no scripts.
- No SVG.
- HTTPS server certificates are not verified.
- WiFi is 2.4 GHz only.
- Images are PNG, JPEG, GIF (first frame only), BMP and WebP. GIF animation is not played.
- A page is read up to 700 KB of HTML. Beyond that it is refused.
- An image file is read up to 300 KB. The in-view images of one page are held up to 1 MB together.
- One stylesheet is read up to 400 KB.
- A WiFi network name is up to 32 bytes and a password up to 63 bytes.

## Repository layout

| Path | What it holds |
|---|---|
| `main/` | The firmware sources. |
| `components/epd97/` | The e-paper panel driver. |
| `components/litehtml/` | The litehtml fork submodule, its build glue and patches. |
| `components/webp/` | The WebP decoder sources. |
| `data/fonts/` | The font pack staged into the data partition. |
| `logo/` | The logo artwork and its render notes. |
| `tools/` | Build and flash scripts, and the font pack tool. |
| `webinstall/` | The web installer page. |
| `probes/` | On-device and host probes for the shade code. |
| `host_tests/` | Host tests. |

Host tests:

| Path | What it tests |
|---|---|
| `host_tests/power/` | The power decisions in `main/power_logic.h`. |
| `host_tests/browser_power/` | The wake, sleep and low battery decisions in `main/browser_logic.h`. |
| `host_tests/leds/` | The LED bar signals in `main/leds_logic.h`. |
| `host_tests/sizecache/` | The image size cache in `main/sizecache.cpp`. |
| `host_tests/tone_fx/` | The fixed-point shade kernels, one subdirectory each. |
| `host_tests/tone_tables/` | Generates and checks `main/tone_tables.inc` from `main/tonemap.h`. |
| `host_tests/tones/` | The dither pipeline, writing result PNGs. |

Probes:

| Path | What it does |
|---|---|
| `probes/bench/` | Times the shade code under QEMU. |
| `probes/oklab/` | Checks the Oklab kernel under QEMU. |
| `probes/search/` | Checks the nearest-shade search under QEMU. |
| `probes/squeeze/` | Checks the chroma squeeze kernel under QEMU. |
| `probes/srgb_bracket/` | Checks the sRGB and bracket kernels under QEMU. |
| `probes/image_dither/` | Dithers an RGB image to a PNG on the host. |
| `probes/solid_red/` | Reports which shades a solid colour dithers to, on the host. |

## Tests

Host tests need clang and make. The dither harness in `host_tests/tones` also uses FreeType through
`pkg-config`, or builds without it with `FREETYPE=0`.

- Logic and kernel tests: run `make test` in the test directory, for example
  `cd host_tests/power && make test`.
- The dither harness: run `make run` in `host_tests/tones`. It writes PNGs and a report.
- The host probes: run `make` in `probes/image_dither` or `probes/solid_red`, then run the built
  binary in `out/`.

The QEMU probes are full ESP-IDF projects. QEMU for Xtensa is needed. Build one with the ESP-IDF
environment, then run its script:

```
source tools/idf_env.sh
cd probes/oklab
$IDF_PY build
./run_qemu.sh
```

The script runs the probe under QEMU and prints the console until the probe's end marker, a crash,
or the time limit.

## Credits and licence

Superfox26 is under the MIT licence. See `LICENSE`.

Third-party parts in the repository, in the firmware image, and loaded by the web installer each
keep their own licence. They are listed in `THIRD_PARTY_NOTICES.md`. The logo and its artwork
credits are in `logo/README.md`.

The panel driver was written for this project with reference to these public repositories, as the
header of `components/epd97/epd97.c` states:

- [Hack-a-Day/2026-Billboard_Badge](https://github.com/Hack-a-Day/2026-Billboard_Badge), the badge
  team's ITC97 driver and the stock MicroPython driver. The badge team's driver names the
  [OpenEPaperLink EFR32FG22 driver](https://github.com/OpenEPaperLink/Tag_FW_EFR32xG22) as its
  origin, and the MicroPython driver names Pervasive Displays'
  [EPD_Driver_GU_large](https://github.com/PervasiveDisplays/EPD_Driver_GU_large).
- [yigitabi5444/supercon26-badge-os](https://github.com/yigitabi5444/supercon26-badge-os).
- [phthallo/supercon](https://github.com/phthallo/supercon).
