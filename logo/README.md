# Superfox26 logo

| File | What it is | Used by |
|---|---|---|
| `superfox26_text.svg` | The editable source. The "26" is live text in Pirata One. | Editing only |
| `superfox26.svg` | The logo with the "26" as outlines, so it renders without the font. | Source of `superfox26.png`, `superfox26_100.png`, `superfox26_32.png` and the 32 and 64 px icons of `webinstall/index.html` |
| `favicon.svg` | The logo without the "26", for 16 px, where the digits do not read. | Source of the 16 px icon of `webinstall/index.html` |
| `superfox26_panel.svg` | `superfox26.svg` in colours that each land on one tone entry of the panel, so its fills draw without dither: `#ea0000` red, `#fcd0d0` light pink, black and paper white. | Source of `panel_32.png` and `panel_200.png` |
| `panel_32.png` | 32 x 32 RGBA render of `superfox26_panel.svg` | Firmware: the left end of the tab bar (`EMBED_FILES` in `main/CMakeLists.txt`) |
| `panel_200.png` | 200 x 200 RGBA render of `superfox26_panel.svg` | Firmware: the welcome notice |
| `superfox26.png` | 512 x 512 RGBA render of `superfox26.svg` | General use, for example a repository or project image |
| `superfox26_100.png`, `superfox26_32.png` | 100 and 32 px palette renders of `superfox26.svg` | General use |
| `favicon.ico` | 16, 32 and 48 px icon | A web host of the installer |

Any SVG renderer with antialiasing and alpha renders these PNGs, for example
`rsvg-convert -w 32 -h 32 superfox26_panel.svg -o panel_32.png`. After a change to
`superfox26_panel.svg`, render both panel PNGs again because the firmware embeds them.

`superfox26_text.svg` loads Pirata One from Google Fonts with a CSS `@import`. Without the import
the "26" shows in a fallback face where the font is not installed, so the import stays.

## Credits

- The "26" is set in Pirata One, under the SIL Open Font License 1.1,
  https://fonts.google.com/specimen/Pirata+One. The outlined files hold glyph outlines only, not
  the font.
- The X and the ring derive from the Hackaday Supercon event graphic. Superfox26 is not affiliated
  with Hackaday.
