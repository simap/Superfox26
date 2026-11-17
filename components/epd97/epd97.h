// -----------------------------------------------------------------------------
// epd97 - an easy driver for the 9.7" Pervasive Displays iTC dual-COG panel.
//
// Two panel variants live behind the same FPC and the same 960x672 geometry:
//
//   E2969JS084  "driver 8"  - opens its OTP with 0xA8, takes a 180-byte
//                             waveform table, refresh follows the table
//   E2969JS0B1  "driver B"  - opens its OTP with 0xB9, takes a 400-byte
//                             waveform table, refresh is a fixed ~1 s
//
// Which one is attached is decided at runtime from the OTP, so nothing has to
// be configured for it. Usage is:
//
//     epd_begin();
//     epd_clear();
//     epd_text(40, 40, "hello", EPD_BLACK, 6);
//     refresh_display(EPD_SLOW);   // the panel's own waveform
//     refresh_display(EPD_FAST);   // the short waveform
//
// Both framebuffer planes live in PSRAM and are yours to write directly if the
// drawing helpers are not enough - see epd_plane_bw() / epd_plane_red().
// -----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_W            960
#define EPD_H            672
#define EPD_STRIDE       (EPD_W / 8)              // 120 bytes per line
#define EPD_PLANE_BYTES  (EPD_STRIDE * EPD_H)     // 80640 bytes per plane

// How the image is driven onto the film.
//
//   EPD_SLOW  the panel's own waveform out of its OTP. ~51 s on the 084,
//             ~34 s on the B1. This is the one that restores the film
//             completely - worth running now and then, and worth using for
//             the first image after power-up.
//   EPD_FAST  the short waveform written to register 0xB0. ~3 s on the 084,
//             ~1 s per pull (three pulls) on the B1. With
//             epd_set_allow_fast_red() (three_color on the drop box) red stays
//             on this short waveform for later images too. Otherwise red is
//             merged into the black plane. See EPD_FAST_ALLOW_RED.
//
// After a frame that drew red with allow-fast-red off, the next fast request
// runs once on the panel's own waveform and does not draw red on that pass.
// That clears the red latch, so the image after it is fast again. It does not
// repeat. While allow-fast-red is on, a fast request is never promoted.
typedef enum {
  EPD_FAST = 0,
  EPD_SLOW = 1
} epd_refresh_t;

typedef enum {
  EPD_PANEL_UNKNOWN = 0,
  EPD_PANEL_084,          // E2969JS084, driver 8
  EPD_PANEL_B1            // E2969JS0B1, driver B
} epd_panel_t;

// A pixel is (red bit, bw bit) and the pair picks one of four waveform
// tables, so red is not "black plus red" - it is its own combination.
typedef enum {
  EPD_WHITE = 0,
  EPD_BLACK = 1,
  EPD_RED   = 2
} epd_color_t;

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

// Claim the pins, allocate both planes, power the panel up once and read its
// OTP to find out which variant is attached. Leaves the panel powered down.
// Returns false if the planes could not be allocated or no panel answered.
bool epd_begin(void);

// Draw whatever is in the framebuffer. Returns false if the COG did not come
// up or the refresh timed out.
//
// When the waveform refresh is switched off (epd_set_run_refresh), this still
// clocks both planes out over SPI and returns without waiting on BUSY.
bool refresh_display(epd_refresh_t mode);

// The waveform update waits on BUSY. The drop box leaves this on.
// fast_refresh 0 selects the long LUT, fast_refresh 1 the short one.
// A missing panel's wait stays capped.
void epd_set_run_refresh(bool on);
bool epd_run_refresh(void);

// Fast refresh normally draws red as black ink. On keeps saturated red in
// the red plane while the short waveform is still the one requested.
void epd_set_allow_fast_red(bool on);

// Replace the driver B fast table (the 400 bytes written to 0xB0) for later
// fast refreshes. The table is copied. NULL restores the built-in table and
// ignores len. Otherwise len must be LUTB_BYTES (400), else
// ESP_ERR_INVALID_SIZE. ESP_ERR_INVALID_ARG when any count byte (bytes 1 to 7
// of a group) is zero, because a zero count runs the group for seconds.
// ESP_ERR_INVALID_STATE during a refresh. A session
// held open by keep-awake is reopened by the next fast refresh, because the
// panel keeps the table it latched. The 084 does not use this table.
esp_err_t epd_set_fast_lut(const uint8_t* table, size_t len);

// 0x15 triggers per fast refresh on driver B, EPD_B_REFRESH_PULLS by default.
// 0 restores the default. Values above EPD_B_PULLS_MAX are clamped to it.
void     epd_set_fast_pulls(unsigned n);
unsigned epd_fast_pulls(void);

// A fresh read of the left COG's OTP stream: power-up, reset, the variant's
// read opener, then up to cap bytes (at most EPD_OTP_DUMP_MAX) into dst. The
// bytes are raw, before the driver B remap the probe applies. The first 88
// are the block the probe reads. What follows is whatever the COG clocks out.
// Closes an open session and leaves the panel powered down. Returns the bytes
// written, 0 with no detected panel or during a refresh.
size_t epd_otp_dump(uint8_t* dst, size_t cap);

// What epd_otp_dump_ex() sends between the reset and the read opener. The
// order is 0x70 (unless skip_0x70), then the set_addr writes, then the
// set_ptr writes. Each 0xA7 0x10 and 0xA7 0x00 is followed by the 100 ms wait
// the refresh sequence uses. Register writes go to both COGs. The opener and
// the read go to the left COG only.
typedef struct {
  bool     set_ptr;    // send 0x44 ptr, 0x45 bank, 0xA7 0x10, 0xA7 0x00 before the opener
  uint8_t  ptr;
  uint8_t  bank;
  bool     set_addr;   // send 0xA1 0x00, 0xA7 0x80, 0xA3 (addr >> 8) (addr & 0xFF) before the opener
  uint16_t addr;
  uint8_t  opener;     // command byte for the read, 0xB9 or 0xA8
  bool     skip_0x70;  // do not send 0x70 [71 36] first
} epd_otp_probe_t;

// epd_otp_dump() with the register writes before the opener chosen by the
// caller. Same power-up, reset, read and power-down. Same return values, and
// 0 for a NULL probe.
size_t epd_otp_dump_ex(const epd_otp_probe_t* probe, uint8_t* dst, size_t cap);

// What epd_reg_read() sends between the reset and the read. The order is 0x70
// (unless skip_0x70), then the after_update (and write_lut) or pre_44_06
// writes. Register writes go to both COGs. The index byte and the read go to
// the left COG only.
typedef struct {
  uint8_t index;        // register to read, for example 0xB0 or 0x45
  bool    after_update; // the refresh's update registers first, through the last 0xA7 0x00 and its wait. No geometry registers, no soft start
  bool    write_lut;    // with after_update: then write the driver B fast table to 0xB0, as a fast refresh does
  bool    pre_44_06;    // when after_update is false: send 0x44 0x06 before the read
  bool    skip_0x70;    // do not send 0x70 [71 36] first
  bool    no_dummy;     // read the data from the first clocked byte, no dummy byte
} epd_reg_probe_t;

// Read a COG register back from the left COG: power-up, reset, the writes in
// probe, the index byte with DC low, then up to cap bytes (at most
// EPD_OTP_DUMP_MAX) after one dummy byte (none with no_dummy) into dst.
// Never sends 0x15 and never ramps the booster. Closes an open session and leaves the panel powered
// down. Returns the bytes written, 0 with no detected panel, during a refresh
// or for a NULL probe.
size_t epd_reg_read(const epd_reg_probe_t* probe, uint8_t* dst, size_t cap);

// The 88-byte OTP block the driver holds, in the driver 8 layout (driver B is
// remapped into it). Sets *len to 88. NULL and *len 0 with no detected panel.
const uint8_t* epd_otp_block(size_t* len);

// epd_begin() could not read a panel, so the window registers are a stand-in
// for the 960-wide geometry and nothing is waiting on BUSY.
bool epd_headless(void);

// True while refresh_display() is in progress, including the BUSY wait.
bool epd_refreshing(void);

// Drive the whole panel to blank paper on its own waveform.
//
// What a boot wants before its first picture: e-paper holds whatever the last
// firmware left on it, and a short waveform cannot cross the full distance in
// one pass, so drawing straight over old content leaves it showing through.
// This clears the framebuffer as a side effect, so call it before you draw.
bool epd_clear_screen(void);

// Close an open session and cut the panel supply. Called for you unless
// keep-awake is on. Call it by hand before a long idle. With EPD_KEEP_SUPPLY
// 1 the automatic close keeps the supply on, and only this, the deep sleep
// prepare, an error, the OTP dump and the register read cut it.
void epd_sleep(void);

// Everything epd_sleep() does, plus latching the supply switch off across an
// ESP32 deep sleep. Deep sleep lets every pad go high-Z, and the MOSFET gate
// has no pull-up, so without the latch the panel supply is undefined while the
// chip is asleep. Call this immediately before esp_deep_sleep_start().
//
// The detected panel and its OTP are kept in RTC memory, so the wake after this
// skips the probe - about three seconds and a whole power cycle per wake.
void epd_deep_sleep_prepare(void);

// Hold the COG powered and initialised between images. An image then costs
// only its plane data and the refresh (~2.7 s instead of ~8 s on the 084) at
// the price of the panel keeping its rails charged in between. Off by default,
// which is the vendor flow. The session is torn down every
// EPD_SESSION_IMAGES images either way.
void epd_set_keep_awake(bool on);
bool epd_keep_awake(void);

// -----------------------------------------------------------------------------
// Verb pass, driver B only
// -----------------------------------------------------------------------------
// A session that latches one short table set and is then pulled many times.
// The plane bit pair of each pixel, (red bit << 1) | bw bit, picks a verb:
// 0 hold, 1 push black, 2 pull white, 3 red step. vcom_level is a level code,
// 0 ground, 1 VDH, 2 VDL, 3 VDHR. For unit_frames frames per pull VCOM sits at
// vcom_level and the sources at these levels, then everything is at ground:
//   hold   vcom_level (net zero)
//   push   VDH
//   pull   VDL
//   red    VDHR with vcom_level 0, else ground (net -vcom_level)
// One pull is unit_frames + 39 frames.
//
// While a verb session is open, refresh_display() and epd_clear_screen()
// return false and do nothing. epd_sleep(), epd_otp_dump() and epd_reg_read()
// close it. A pass that drew any red plane bit leaves the panel marked as
// holding red, so the next fast refresh without allow-fast-red runs slow.
#define EPD_VERB_UNIT_MAX   16   // frames at the rail per pull
#define EPD_VERB_PULLS_MAX   8   // pulls per pass call
#define EPD_SEQ_UNIT_MAX     9   // driven frames per paper or ink pull, at most
#define EPD_SEQ_PULL_FRAMES 10   // frames per seq pull, whatever the unit
#define EPD_SEQ_W_FRAMES     9   // w: VDHR frames per pull, whatever the unit
// The phase 1 pull length and drive are set by the shapes in epd97_lut.h,
// which checks them against these at compile time.
#define EPD_PHASE1_PULL_FRAMES 17   // frames per phase 1 pull
#define EPD_PHASE1_DRIVE        8   // VDH or VDL frames of a phase 1 I or P pull

// Which table set a verb session latches. EPD_VERB_TABLE_SEQ is the phase 2
// nudge set in epd97_lut.h: 0 hold, 1 paper VDL, 2 ink VDH, 3 w VDHR, one
// frame a group, paper and ink driven for unit_frames groups, w for
// EPD_SEQ_W_FRAMES, VCOM at ground. One seq pull is EPD_SEQ_PULL_FRAMES
// frames and unit_frames is 1 to EPD_SEQ_UNIT_MAX. vcom_level is range
// checked (0 to 3) then forced to 0 for it.
// EPD_VERB_TABLE_PHASE1 is the single table phase 1 set
// (epd_build_lutb_phase1): 0 S shake, 1 I ink, 2 P paper, 3 M mono shake,
// with no hold code. EPD_PHASE1_PULL_FRAMES frames a pull. unit_frames and
// vcom_level are range checked as for every set (unit_frames 1 to
// EPD_VERB_UNIT_MAX, vcom_level 0 to 3), then forced to EPD_PHASE1_DRIVE
// and 0.
typedef enum {
  EPD_VERB_TABLE_VERB   = 0,
  EPD_VERB_TABLE_SEQ    = 1,
  EPD_VERB_TABLE_PHASE1 = 2
} epd_verb_table_t;

// Power up and latch the verb table, with both planes sent as zero, which
// selects code 0 for every pixel. Nothing is driven until the first pull.
// Closes an open normal session or verb session first. ESP_ERR_NOT_SUPPORTED
// on a panel that is not driver B, ESP_ERR_INVALID_ARG for unit_frames 0 or
// above EPD_VERB_UNIT_MAX or vcom_level above 3, ESP_ERR_INVALID_STATE before
// epd_begin(), with no
// panel or during a refresh. A failed step closes everything and returns
// ESP_FAIL (COG did not come up) or ESP_ERR_TIMEOUT (booster not ready).
esp_err_t epd_verb_open(uint8_t unit_frames, uint8_t vcom_level);
// Same, with the table set chosen. Code 0 is hold on the verb and seq sets
// and S on EPD_VERB_TABLE_PHASE1, and on every set nothing is pulled until
// epd_verb_pass() or epd_verb_pull().
esp_err_t epd_verb_open_table(uint8_t unit_frames, uint8_t vcom_level,
                              epd_verb_table_t table);
// Same, and with_planes true sends epd_plane_bw() and epd_plane_red() at open,
// before the table is latched, instead of zeros. Pair it with
// epd_verb_pull(pulls, false, ...) so the first pull does not resend them.
esp_err_t epd_verb_open_ex(uint8_t unit_frames, uint8_t vcom_level,
                           epd_verb_table_t table, bool with_planes);
// Table set of the open verb session. EPD_VERB_TABLE_VERB when none is open.
epd_verb_table_t epd_verb_table(void);
// epd_verb_pass() with the plane send optional. resend false triggers the
// planes the session already holds. The COG swaps its two plane registers
// after every pull, so a pixel the last pull drove on code 1 is on code 2
// for the next pull, and the reverse. Codes 0 and 3 are unchanged. A pull
// with resend false, and every pull after the first in one call, is then
// correct only when every pixel holds code 0 or 3 and the codes repeat the
// previous pull. On EPD_VERB_TABLE_PHASE1 those codes are S and M.
esp_err_t epd_verb_pull(unsigned pulls, bool resend, uint32_t *panel_ms_total);
// Send epd_plane_bw() and epd_plane_red(), then trigger the latched table
// `pulls` times. Never rewrites the table. *panel_ms_total (may be NULL) gets
// the summed BUSY time. ESP_ERR_INVALID_STATE with no open verb session,
// ESP_ERR_INVALID_ARG for pulls 0 or above EPD_VERB_PULLS_MAX. A pull that
// does not finish within EPD_VERB_BUSY_MS closes the session and returns
// ESP_ERR_TIMEOUT. Sets epd_last_send_ms(), epd_last_panel_ms() and
// epd_last_frame_ms().
esp_err_t epd_verb_pass(unsigned pulls, uint32_t *panel_ms_total);
// Soft shutdown, then the supply cut unless EPD_KEEP_SUPPLY is 1. Does nothing
// when no verb session is open.
void      epd_verb_close(void);
bool      epd_verb_is_open(void);
// unit_frames of the open verb session, 0 when none is open.
uint8_t   epd_verb_unit(void);

// There is no row window. Every pull and every refresh scans the whole
// panel. A DUW and DRFW band narrowed before the trigger burns a line into
// the panel at the band's bottom edge.

// -----------------------------------------------------------------------------
// What is attached, and what the last refresh cost
// -----------------------------------------------------------------------------
epd_panel_t epd_panel(void);
const char* epd_panel_name(void);
uint32_t    epd_last_panel_ms(void);   // BUSY low to BUSY high
uint32_t    epd_last_frame_ms(void);   // the whole call
// Both planes on SPI, window registers included. With epd_set_run_refresh(false)
// refresh_display() sends the planes and stops, so this times the SPI path alone.
uint32_t    epd_last_send_ms(void);
// esp_timer_get_time() taken where epd_last_send_ms() stops, after the last
// transaction of both planes to both COGs. 0 if nothing has been sent.
int64_t     epd_last_send_end_us(void);

// -----------------------------------------------------------------------------
// Framebuffer
// -----------------------------------------------------------------------------
// Two 1-bpp planes, MSB first, x = 0 in bit 7 of byte 0 of the line.
// A set bit in the bw plane is ink; a set bit in the red plane is red. A red
// pixel has the red bit set and the bw bit clear.
uint8_t* epd_plane_bw(void);
uint8_t* epd_plane_red(void);

// -----------------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------------
void epd_clear(void);
void epd_fill(epd_color_t c);
void epd_pixel(int x, int y, epd_color_t c);
void epd_fill_rect(int x, int y, int w, int h, epd_color_t c);
void epd_rect(int x, int y, int w, int h, int thickness, epd_color_t c);
void epd_hline(int x, int y, int w, int thickness, epd_color_t c);
void epd_vline(int x, int y, int h, int thickness, epd_color_t c);

// A 5x7 font, scaled by whole pixels. Returns the x the text ended at.
// Character cell is 6*scale wide and 8*scale tall.
int  epd_text(int x, int y, const char* s, epd_color_t c, int scale);
int  epd_printf(int x, int y, epd_color_t c, int scale, const char* fmt, ...);
int  epd_text_width(const char* s, int scale);

#ifdef __cplusplus
}
#endif
