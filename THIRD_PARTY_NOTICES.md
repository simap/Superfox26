# Third-party notices

Superfox26's own code is under the MIT licence in `LICENSE`. This file lists the third-party parts
in this repository, in the firmware image (`firmware.bin` and the build outputs it merges), and
loaded by the web installer page. Each part keeps its own licence.

A binary distribution of the firmware must carry the copyright notices and licence texts of the
parts in the firmware. Where a notice names individual authors, this file gives the path of the
file that holds the full notice instead of copying it. Paths under `managed_components/` exist
after the first build, when the ESP-IDF component manager downloads those components.

## In the firmware

### litehtml (fork)

- Version: a fork of litehtml at commit 3147342c, branch `epaper`, based on litehtml v0.10
- Where: `components/litehtml/fork` (git submodule, the URL is in `.gitmodules`), built by
  `components/litehtml`, which applies `components/litehtml/patches/xtensa.patch`
- Licence: BSD 3-Clause
- Upstream: https://github.com/litehtml/litehtml
- Notice: the copyright notice and licence text in `components/litehtml/fork/LICENSE`

### Gumbo HTML parser (in litehtml)

- Version: Gumbo 0.14.1, the copy that litehtml carries
- Where: `components/litehtml/fork/src/gumbo`
- Licence: Apache License 2.0
- Upstream: https://github.com/litehtml/litehtml (the original Gumbo is https://github.com/google/gumbo-parser)
- Notice: `components/litehtml/fork/src/gumbo/LICENSE`. The litehtml copy is modified from the original.

### libwebp (decoder)

- Version: 1.6.0, the decoder sources only, unmodified
- Where: `components/webp`
- Licence: BSD 3-Clause, with the WebM additional patent grant
- Upstream: https://chromium.googlesource.com/webm/libwebp
- Notice: Copyright (c) 2010, Google Inc. All rights reserved. The licence text is in
  `components/webp/COPYING`, the patent grant in `components/webp/PATENTS`, the authors in
  `components/webp/AUTHORS`.

### FreeType

- Version: 2.14.3 (ESP-IDF component espressif/freetype 2.14.3~1)
- Where: `managed_components/espressif__freetype`
- Licence: the FreeType License (FTL), chosen from FTL or GPLv2
- Upstream: https://freetype.org, component from https://github.com/espressif/idf-extra-components
- Notice: the FTL requires this credit in the documentation of a binary distribution:
  "Portions of this software are copyright (c) 2026 The FreeType Project (https://freetype.org).
  All rights reserved." The full text is in `managed_components/espressif__freetype/LICENSE`.

### libjpeg-turbo

- Version: 3.2.0 (ESP-IDF component espressif/libjpeg-turbo 3.2.0~1)
- Where: `managed_components/espressif__libjpeg-turbo`
- Licence: the IJG License and the Modified (3-clause) BSD License
- Upstream: https://libjpeg-turbo.org, component from https://github.com/espressif/idf-extra-components
- Notice: the IJG License requires this line in the documentation of a binary distribution:
  "This software is based in part on the work of the Independent JPEG Group." The BSD copyright
  notices and licence texts are in `managed_components/espressif__libjpeg-turbo/LICENSE`.

### libpng

- Version: 1.6.59 (ESP-IDF component espressif/libpng)
- Where: `managed_components/espressif__libpng`
- Licence: PNG Reference Library License version 2
- Upstream: https://github.com/pnggroup/libpng, component from https://github.com/espressif/idf-extra-components
- Notice: `managed_components/espressif__libpng/LICENSE`

### zlib

- Version: 1.3.2 (ESP-IDF component espressif/zlib 1.3.2~2, a dependency of libpng and FreeType)
- Where: `managed_components/espressif__zlib`
- Licence: zlib License
- Upstream: https://zlib.net, component from https://github.com/espressif/idf-extra-components
- Notice: `managed_components/espressif__zlib/LICENSE`. The zlib License asks for no notice in a
  binary distribution.

### littlefs and esp_littlefs

- Version: esp_littlefs 1.21.1 (ESP-IDF component joltwallet/littlefs) with littlefs 2.11
- Where: `managed_components/joltwallet__littlefs`
- Licence: esp_littlefs MIT License, littlefs BSD 3-Clause
- Upstream: https://github.com/joltwallet/esp_littlefs and https://github.com/littlefs-project/littlefs
- Notice: esp_littlefs in `managed_components/joltwallet__littlefs/LICENSE`. littlefs:
  "Copyright (c) 2022, The littlefs authors. Copyright (c) 2017, Arm Limited. All rights reserved."
  with the licence text in `managed_components/joltwallet__littlefs/src/littlefs/LICENSE.md`.

### led_strip

- Version: 3.1.1 (ESP-IDF component espressif/led_strip)
- Where: `managed_components/espressif__led_strip`
- Licence: Apache License 2.0
- Upstream: https://github.com/espressif/idf-extra-components
- Notice: `managed_components/espressif__led_strip/LICENSE`

### ESP-IDF

- Version: v6.0.2
- Where: installed outside the repository. The build links its components into the firmware and
  writes its bootloader.
- Licence: Apache License 2.0 for ESP-IDF itself. Parts in this firmware carry their own
  licences: the C library (Newlib, BSD), FreeRTOS (MIT), lwIP (BSD), Mbed TLS (Apache 2.0),
  wpa_supplicant (BSD), FreeBSD net80211 (BSD), the TLSF allocator (BSD 3-Clause), the HTTP parser
  (MIT, from NGINX) and the Xtensa headers (MIT).
- Upstream: https://github.com/espressif/esp-idf
- Notice: ESP-IDF's list of third-party copyrights and licences,
  https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/COPYRIGHT.html, and the licence
  files of each component in the ESP-IDF tree.

### Liberation fonts

- Version: 2.1.5, subset and renamed to B26 Sans, B26 Serif and B26 Mono, as the Reserved Font
  Name clause requires
- Where: `data/fonts/sans*.ttf`, `serif*.ttf`, `mono*.ttf`, in the data partition of the firmware
- Licence: SIL Open Font License 1.1
- Upstream: https://github.com/liberationfonts/liberation-fonts
- Notice: Digitized data copyright (c) 2010 Google Corporation with Reserved Font Arimo, Tinos and
  Cousine. Copyright (c) 2012 Red Hat, Inc. with Reserved Font Name Liberation. The licence text
  is in `data/fonts/LICENSE-Liberation.txt`, which the build also puts in the data partition.

### GNU Unifont

- Version: 18.0.01, subset
- Where: `data/fonts/fallback.otf`, in the data partition of the firmware
- Licence: SIL Open Font License 1.1, chosen from OFL 1.1 or GPL 2 or later with the font
  embedding exception
- Upstream: https://unifoundry.com/unifont/
- Notice: the copyright notice and licence text in `data/fonts/LICENSE-Unifont.txt`, which the
  build also puts in the data partition

### Superfox26 logo

The logo PNGs in the firmware (`logo/panel_32.png`, `logo/panel_100.png`) contain the Pirata One
outlines and the Hackaday Supercon graphic described under Artwork.

## Loaded by the web installer

### esptool-js

- Version: 0.7.1, the bundle `bundle.js`
- Where: `webinstall/index.html` imports it from the unpkg CDN at run time. It is not in the repository.
- Licence: Apache License 2.0. The bundle includes pako (MIT and zlib) and atob-lite (MIT).
- Upstream: https://github.com/espressif/esptool-js
- Notice: the licence files of the esptool-js package, https://www.npmjs.com/package/esptool-js

## In the repository only

### LodePNG

- Version: 20230410
- Where: `host_tests/tones/lodepng.c` and `lodepng.h`, used by the host probe `probes/image_dither`
  to read and write PNG files
- Licence: zlib License
- Upstream: https://github.com/lvandeve/lodepng
- Notice: the copyright notice and licence at the top of `host_tests/tones/lodepng.h`

### harfbuzzjs (build tool)

- Version: 1.6.4, an npm dependency of `tools/fontpack`, installed by `npm ci`. It subsets the fonts
  and is not part of the firmware.
- Licence: MIT License
- Upstream: https://github.com/harfbuzz/harfbuzzjs

## Artwork

### Pirata One

The "26" in the logo is set in Pirata One and converted to outlines (`logo/superfox26.svg`,
`logo/superfox26_panel.svg`). The font itself is not distributed. Pirata One is under the SIL Open
Font License 1.1 with Reserved Font Name "Pirata", https://fonts.google.com/specimen/Pirata+One.

### Hackaday Supercon graphic

The X and the ring of the logo derive from the Hackaday Supercon event graphic. Superfox26 is not
affiliated with Hackaday.

## References and derived code

The head of `components/epd97/epd97.c` lists the sources of the panel driver. This table repeats
them with their licences.

| Source | URL | Licence | What the driver takes from it |
|---|---|---|---|
| The badge team's ITC97 driver, `firmware/esp32s3-itc97-drag-drop/src/epd97.cpp` at commit 6ba6dd8 | https://github.com/Hack-a-Day/2026-Billboard_Badge | The repository has no licence file. The file carries no licence or copyright notice. | The bus, power, OTP detection, update register, soft start and brownout code of `epd97.c`, much of it unchanged. The pin map, the reset and supply timings and several comments of `epd97_config.h`. |
| The badge's stock MicroPython driver, `firmware/badge/lib/epd.py`, in the same repository | https://github.com/Hack-a-Day/2026-Billboard_Badge | A BSD-style notice in the file, Copyright (c) 2012-2021 Pervasive Displays Inc. | The 0x70 booster setting of driver B and the plane resend that keeps a 0xB0 table latched. No block of code is copied. |
| supercon26-badge-os, `components/os/epd.c` and `docs/DEVICE_INFO.md` | https://github.com/yigitabi5444/supercon26-badge-os | GPL 2.0 | The quick open waits, the 0x03 oscillator value for 23.7 ms frames, the kept supply between sessions and the one frame group of the seq set. No function matches. |
| phthallo/supercon, `firmware/lib/luts.py` | https://github.com/phthallo/supercon | No licence | Which zero count bytes the B1 skips. |

The badge team's driver names the OpenEPaperLink EFR32FG22 driver for this panel as its own origin.
That repository, https://github.com/OpenEPaperLink/Tag_FW_EFR32xG22, has no licence file. The epd.py
driver names Pervasive Displays' EPD_Driver_GU_large, https://github.com/PervasiveDisplays/EPD_Driver_GU_large,
as its origin. That repository has no licence file either.

The firmware's button and LED pins follow `firmware/badge/lib/button.py` and `led_bar.py` of the
badge repository. Those files carry no notice.
