// -----------------------------------------------------------------------------
// epd97: driver for the 9.7 inch Pervasive Displays iTC panel on an ESP32-S3.
// The panel carries two controllers behind one FPC and shows black, white and
// red. This header is the module's interface.
//
// Two panel variants share the FPC and the 960x672 geometry:
//   E2969JS084  "driver 8"  opens its OTP with 0xA8
//   E2969JS0B1  "driver B"  opens its OTP with 0xB9
//
// epd_begin() reads the OTP to find which variant is attached, so nothing has
// to be configured for it. Both variants run the slow refresh, the panel's own
// waveform. Only driver B takes the drive tables of a tone session.
//
// Call epd_begin() once. Then write the two framebuffer planes with
// epd_plane_bw() and epd_plane_red() and call refresh_display(EPD_SLOW), or
// draw tones through a tone session. Call epd_sleep() after the last image.
//
// Terms used in this driver:
//   the controller  the panel's driver chip, a COG (chip on glass). The panel
//                   has two. The left controller drives the left 480 columns
//                   and the right the right 480. The code calls them COG_L and
//                   COG_R. Register writes go to both.
//   OTP             the controller's one time programmable memory. It holds the
//                   window geometry, the booster ramp and the panel's own
//                   waveform.
//   VDH, VDL, VDHR  the source rails. VDH drives a pixel toward black. VDL
//                   drives it toward white. VDHR is the reduced positive rail
//                   that moves the red pigment without bringing black up. A
//                   pixel at ground is not driven.
//   frame           the controller's drive step, 23.7 ms in a tone session.
//   session         one power-up of the controller with one waveform latched.
//                   It ends with the DC/DC soft shutdown.
//   code            the plane bit pair of a pixel, (red bit << 1) | bw bit, 0
//                   to 3. It picks which of the four drive tables of the
//                   latched set drives the pixel.
//   pull            one trigger (register 0x15) of the latched set. Every pixel
//                   plays the frames of its code's drive table once.
//   pass            a send of both planes followed by one or more pulls.
//   phase 1, phase 2  a tone draw plays all its pulls on the phase 1 set, then
//                   all its pulls on the seq set (phase 2).
//   S I P M         the phase 1 codes 0 to 3: shake, ink, paper, mono shake.
//   h p i w         the phase 2 codes 0 to 3: hold, paper nudge (VDL), ink
//                   nudge (VDH) and w (VDHR).
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

// The refresh refresh_display() runs. EPD_SLOW is the panel's own waveform
// out of its OTP, about 51 s on the 084 and about 35 s on the B1. It is the
// only refresh this driver runs.
typedef enum {
  EPD_SLOW = 1
} epd_refresh_t;

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

// Claim the pins, allocate both planes, power the panel up once and read its
// OTP to find out which variant is attached. Leaves the panel powered down.
// Returns false if the planes could not be allocated or the SPI bus could not
// be set up. With no panel attached it returns true. The driver then runs
// headless, sends the planes on every refresh and caps its BUSY waits.
bool epd_begin(void);

// Draw the framebuffer on the panel's own waveform. Closes an open tone
// session first. Returns false if the controller did not come up, the plane send
// failed or the refresh timed out. Headless, the refresh gives up at the first
// BUSY wait and still returns true.
bool refresh_display(epd_refresh_t mode);

// Close an open session and cut the panel supply. Call it after the last
// image. With EPD_KEEP_SUPPLY 1 the close at the end of a refresh keeps the
// supply on, and only this, epd_deep_sleep_prepare() and an error cut it.
void epd_sleep(void);

// Everything epd_sleep() does, plus latching the supply switch off across an
// ESP32 deep sleep. Deep sleep lets every pad go high-Z, and the MOSFET gate
// has no pull-up, so without the latch the panel supply is undefined while the
// chip is asleep. Call this immediately before esp_deep_sleep_start().
//
// The detected panel and its OTP are kept in RTC memory, so the wake after this
// skips the detection, which costs about three seconds and a power cycle.
void epd_deep_sleep_prepare(void);

// -----------------------------------------------------------------------------
// Tone sessions, driver B only
// -----------------------------------------------------------------------------
// A tone session latches one table set and is then pulled many times, with
// new planes sent between pulls. The two sets, their codes and their frames
// are described in epd97_lut.h:
//
//   EPD_TONE_TABLE_PHASE1  codes S I P M, EPD_PHASE1_PULL_FRAMES frames a
//                          pull. No hold code.
//   EPD_TONE_TABLE_SEQ     codes h p i w, EPD_SEQ_PULL_FRAMES frames a pull.
//                          unit_frames (1 to EPD_SEQ_UNIT_MAX) is the length
//                          of a p or i drive.
//
// VCOM is at ground on both sets.
//
// The controller swaps its two plane registers after every pull. A pixel that played
// code 1 plays code 2 at the next pull, and the reverse. Codes 0 and 3 do not
// change. A pull without a plane send is therefore correct only when every
// pixel holds code 0 or 3 and repeats its code of the pull before.
//
// The driver does not check the push limits of epd97_config.h. The caller
// (main/tone_plan.h) keeps every pixel within them. Legal calls can break
// them. On the seq set a w pixel gets EPD_SEQ_W_FRAMES VDHR frames a pull, so
// five w pulls in a row exceed EPD_PUSH_MAX_VDHR.
//
// While a tone session is open, refresh_display() and epd_sleep() close it
// first. There is no row window. Every pull and every refresh scans the whole
// panel. Narrowing the DUW and DRFW windows to a band before a trigger burns
// a line into the panel at the band's bottom edge.
#define EPD_TONE_PULLS_MAX   8   // pulls per epd_tone_pass() or epd_tone_pull() call

typedef enum {
  EPD_TONE_TABLE_NONE   = 0,   // no tone session open
  EPD_TONE_TABLE_SEQ    = 1,
  EPD_TONE_TABLE_PHASE1 = 2
} epd_tone_table_t;

// Power up and latch the table set, with both planes sent as zero, which
// selects code 0 for every pixel. Nothing is driven until the first pull.
// Closes an open tone session first. unit_frames applies to the seq set only.
// vcom_level is not used, both sets hold VCOM at ground, and it must be 0 to
// 3.
//
// ESP_ERR_INVALID_STATE before epd_begin(), with no panel or during a
// refresh. ESP_ERR_NOT_SUPPORTED on a panel that is not driver B.
// ESP_ERR_INVALID_ARG for an unknown set, vcom_level above 3, or a seq
// unit_frames of 0 or above EPD_SEQ_UNIT_MAX. A failed step closes everything
// and returns ESP_FAIL (controller did not come up or the plane send failed) or
// ESP_ERR_TIMEOUT (booster not ready).
esp_err_t epd_tone_open_table(uint8_t unit_frames, uint8_t vcom_level,
                              epd_tone_table_t table);

// Send epd_plane_bw() and epd_plane_red(), then pull the latched set `pulls`
// times. Never rewrites the table. *panel_ms_total (may be NULL) gets the
// summed BUSY time. ESP_ERR_INVALID_STATE with no open tone session,
// ESP_ERR_INVALID_ARG for pulls 0 or above EPD_TONE_PULLS_MAX. A pull that
// does not finish within EPD_TONE_BUSY_MS, or a failed plane send, closes the
// session and returns ESP_ERR_TIMEOUT or ESP_FAIL.
esp_err_t epd_tone_pass(unsigned pulls, uint32_t *panel_ms_total);

// epd_tone_pass() with the plane send optional. resend false pulls the plane
// registers as the last pull left them.
esp_err_t epd_tone_pull(unsigned pulls, bool resend, uint32_t *panel_ms_total);

bool epd_tone_is_open(void);
// Table set of the open tone session, EPD_TONE_TABLE_NONE when none is open.
epd_tone_table_t epd_tone_table(void);
// unit_frames of the open tone session. It is the seq unit, EPD_PHASE1_DRIVE
// on the phase 1 set, and 0 when none is open.
uint8_t epd_tone_unit(void);

// -----------------------------------------------------------------------------
// What is attached
// -----------------------------------------------------------------------------
// "E2969JS084 (driver 8)", "E2969JS0B1 (driver B)" or "unknown".
const char* epd_panel_name(void);

// -----------------------------------------------------------------------------
// Framebuffer
// -----------------------------------------------------------------------------
// Two 1-bpp planes, MSB first, x = 0 in bit 7 of byte 0 of the line. For a
// slow refresh a set bit in the bw plane is ink and a set bit in the red plane
// is red. A red pixel has the red bit set and the bw bit clear. In a tone
// session the two bits are the pixel's code.
uint8_t* epd_plane_bw(void);
uint8_t* epd_plane_red(void);

// Clear both planes to zero, which is paper on a slow refresh and code 0 in a
// tone session.
void epd_clear(void);

#ifdef __cplusplus
}
#endif
