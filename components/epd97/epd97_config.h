// -----------------------------------------------------------------------------
// Pin map and tunables for the 9.7" Pervasive Displays iTC panel on an ESP32-S3.
//
// Every tunable is a #ifndef, so it can be overridden with a -D compile
// definition on the epd97 component without touching the file. The push
// limits at the end are not tunables. The pin map, the reset and supply
// timings and several of the comments here follow the epd97_config.h of the
// badge team's ITC97 driver (see the head of epd97.c).
// -----------------------------------------------------------------------------
#pragma once

// --- Pin map -----------------------------------------------------------------
#ifndef EPD_PIN_DC
#define EPD_PIN_DC        10
#endif
#ifndef EPD_PIN_MOSI
#define EPD_PIN_MOSI      47   // SDA, also the line the OTP is read back on
#endif
#ifndef EPD_PIN_SCK
#define EPD_PIN_SCK       21   // SCL
#endif
#ifndef EPD_PIN_CS_L
#define EPD_PIN_CS_L      13   // CS1, the left controller (left half of the panel)
#endif
#ifndef EPD_PIN_CS_R
#define EPD_PIN_CS_R       3   // CS2, the right controller (right half)
#endif
#ifndef EPD_PIN_BUSY_L
#define EPD_PIN_BUSY_L    12   // BUSY of the left controller
#endif
#ifndef EPD_PIN_BUSY_R
#define EPD_PIN_BUSY_R     8   // BUSY of the right controller, -1 if it is not wired
#endif
#ifndef EPD_PIN_RST
#define EPD_PIN_RST       11
#endif
#ifndef EPD_PIN_PWR
#define EPD_PIN_PWR       48   // VCC_EN
#endif
// Which level on VCC_EN powers the panel.
//
// The default is 0. The board switches the panel's VCC with an SK2301AA1SHB,
// a P-channel MOSFET high-side switch driven straight off the GPIO. Pulling
// the gate low turns it on. Set 1 for a plain active-high enable input.
//
// Getting this backwards does not look like a power problem from the outside.
// The OTP read still succeeds, because it runs while the pin happens to be at
// the other level. The refresh only fails at the DC/DC soft start, as
// "booster never reported ready in 10 s".
//
// There is no pull-up on the gate, so between reset and epd_begin() the pin is
// high-Z and the switch is undefined. epd_begin() parks it before it drives
// it, but a 100k to 3V3 on the gate would close that window in hardware.
#ifndef EPD_PWR_ACTIVE_HIGH
#define EPD_PWR_ACTIVE_HIGH 0
#endif

// --- Bus ---------------------------------------------------------------------
#ifndef EPD_SPI_HOST_NUM
#define EPD_SPI_HOST_NUM  SPI2_HOST
#endif
// Bulk plane data only. Register writes and the OTP readback stay bitbanged.
// They are a few dozen bytes and the OTP wants a ~50 kHz clock. The PDi
// reference driver runs the panel at 8 MHz. 20 MHz is PDi's stated maximum for
// this controller family and the clock that yigitabi5444/supercon26-badge-os
// runs on the B1.
#ifndef EPD_SPI_HZ
#define EPD_SPI_HZ        20000000u
#endif

// --- Timing ------------------------------------------------------------------
// Reset shape, from the vendor's b_reset(200,20,200,50,5) for the medium
// controller with margin added. Too short shows as cog_initial failing and the
// OTP reading back corrupt. All times are in ms.
#ifndef EPD_T_RST_PWR
#define EPD_T_RST_PWR    800u   // settle after switching the panel supply on
#endif
#ifndef EPD_T_RST_HI
#define EPD_T_RST_HI      50u
#endif
#ifndef EPD_T_RST_LO
#define EPD_T_RST_LO     100u
#endif
#ifndef EPD_T_RST_SET
#define EPD_T_RST_SET    400u
#endif
// Second reset, after the OTP read.
#ifndef EPD_T_OTP_HI
#define EPD_T_OTP_HI      50u
#endif
#ifndef EPD_T_OTP_LO
#define EPD_T_OTP_LO     100u
#endif
#ifndef EPD_T_OTP_SET
#define EPD_T_OTP_SET    400u
#endif
// Minimum time the panel supply stays off between two controller cycles. The booster
// rails have to bleed down, and a cycle started on top of them stalls the
// update. 1500 ms is reliable. At 800 ms cog_initial fails and the OTP reads
// corrupt. A deep sleep counts as off time.
#ifndef EPD_T_OFF_MIN
#define EPD_T_OFF_MIN   1500u
#endif
#ifndef EPD_T_DISCHARGE
#define EPD_T_DISCHARGE  300u   // held in reset, supply already cut
#endif
// With 1, a session close keeps the panel supply on, as the stock MicroPython
// firmware and yigitabi5444/supercon26-badge-os do. The close is the DC/DC
// soft shutdown, a bounded wait for BUSY, then the controller held in reset. The
// next open skips the EPD_T_OFF_MIN floor and starts at the reset once
// EPD_T_RST_PWR has passed since the supply came on. Error paths, epd_sleep() and epd_deep_sleep_prepare()
// still cut the supply. With 0, every close cuts the supply. The browser26
// build sets 1 in the top level CMakeLists.txt.
#ifndef EPD_KEEP_SUPPLY
#define EPD_KEEP_SUPPLY    0
#endif
// BUSY ceiling after the soft shutdown of a close that keeps the supply on.
// A close that runs past it cuts the supply.
#ifndef EPD_T_SHUTDOWN_BUSY
#define EPD_T_SHUTDOWN_BUSY 2000u
#endif
#ifndef EPD_T_PRE_UPD
#define EPD_T_PRE_UPD    660u   // settle in front of the update registers
#endif
// Wait after the last 0xA7 of the update registers, before the soft start.
// The register table stores it in units of 10 ms, so it is a multiple of 10
// and at most 1270.
#ifndef EPD_T_PRE_SS
#define EPD_T_PRE_SS     500u
#endif
// Quick profile for driver B tone sessions, the waits
// yigitabi5444/supercon26-badge-os uses to open a session in 348 ms: reset
// 2/4/20 ms, a 5 ms settle in front of the registers, 0x05 7D 50 ms, 0x05 00
// 1 ms, load strobes 2/10 ms, and the oscillator register 0x03 00 11 after the
// first strobe for 23.7 ms frames. The slow refresh keeps the vendor waits
// above. The push limits below depend on the 23.7 ms frame, so this is 1.
#ifndef EPD_QUICK_OPEN
#define EPD_QUICK_OPEN     1
#endif
#ifndef EPD_TQ_RST_HI
#define EPD_TQ_RST_HI      2u
#endif
#ifndef EPD_TQ_RST_LO
#define EPD_TQ_RST_LO      4u
#endif
#ifndef EPD_TQ_RST_SET
#define EPD_TQ_RST_SET    20u
#endif
#ifndef EPD_TQ_PRE_UPD
#define EPD_TQ_PRE_UPD     5u
#endif
// A refresh on the panel's own waveform takes about 51 s on the 084 and about
// 35 s on the B1, so the ceiling has to be generous.
#ifndef EPD_IDLE_WAIT_MS
#define EPD_IDLE_WAIT_MS 180000u
#endif
// How long to keep watching for BUSY to drop again after the refresh reports
// done. Some panels run the waveform a second time.
#ifndef EPD_SECOND_PASS_MS
#define EPD_SECOND_PASS_MS 1000u
#endif

// --- Waveform bank -----------------------------------------------------------
// The value of the second 0x45 write of the update registers. On the 084 it
// picks between the OTP's temperature bands. The 9.7" default is 0x86, the 12"
// one 0x8C.
#ifndef EPD_REG45_084
#define EPD_REG45_084 0x86
#endif
#ifndef EPD_REG45_B1
#define EPD_REG45_B1  0x84
#endif

// --- Tone session, driver B --------------------------------------------------
// Frame period of a driver B pull with the quick profile's oscillator setting,
// in microseconds. yigitabi5444/supercon26-badge-os measured 23.7 ms per
// frame. It sets the push limits below and the pull length logged at
// epd_tone_open_table(). A larger value only tightens the limits, which
// host_tests/tone_tables uses to test that the generator refuses a table. A
// smaller value would loosen them, so it is refused below.
#ifndef EPD_B_FRAME_US
#define EPD_B_FRAME_US 23700u
#endif
// BUSY ceiling for one tone pull, in ms. A phase 1 pull is 17 frames, 403 ms
// at EPD_B_FRAME_US. A seq pull is 10 frames, 237 ms. A pull that runs past
// this closes the session and cuts the supply.
#ifndef EPD_TONE_BUSY_MS
#define EPD_TONE_BUSY_MS 5000u
#endif

// --- Push limit, never exceeded ----------------------------------------------
// A hardware safety rule. No pixel is driven on one rail longer than the
// panel's own stock OTP waveform drives a pixel on that rail. The stock
// waveform is the only drive known to be safe for this film, so no tone
// pattern drives harder than it.
//
// A run is the frames a pixel is driven on one rail, ground frames between
// drives not counted. VDH and VDHR drive the same way, so neither ends the
// other's run. A VDL frame ends a VDH, a VDHR and a positive run. A VDH or
// VDHR frame ends a VDL run. The positive run, VDH and VDHR frames together
// until a VDL frame, is held to the VDH limit. Runs continue across pulls and
// across draws.
//
// The stock waveform the B1 runs at room temperature (set 6 of its OTP),
// counted that way, has its longest runs at 66 VDH frames (the end of the
// black table), 82 VDL frames (the paper push of the red table) and 30 VDHR
// frames (the red table). Its frame is 28.8 ms, from the measured 35.4 s of
// the whole refresh. The limits below are the same times in frames of
// EPD_B_FRAME_US.
//
// What keeps a draw within them is the tone planner, main/tone_plan.h. The
// generator of main/tone_tables.inc refuses a pattern that breaks a limit,
// and tone_plan_init() checks the expanded patterns at boot. The driver does
// not check what a pull drives (see the tone section of epd97.h).
//
// The limits are frame counts, so they hold only while the frame is
// EPD_B_FRAME_US. A tone session gets that frame from the oscillator write of
// the quick profile. Without it the B1 runs frames of about 29 ms, and the
// same counts last longer than the stock waveform's runs.
#define EPD_STOCK_FRAME_US   28800u
#define EPD_STOCK_PUSH_VDH   66u
#define EPD_STOCK_PUSH_VDL   82u
#define EPD_STOCK_PUSH_VDHR  30u
#define EPD_PUSH_MAX_VDH   ((EPD_STOCK_PUSH_VDH * EPD_STOCK_FRAME_US) / EPD_B_FRAME_US)   // 80
#define EPD_PUSH_MAX_VDL   ((EPD_STOCK_PUSH_VDL * EPD_STOCK_FRAME_US) / EPD_B_FRAME_US)   // 99
#define EPD_PUSH_MAX_VDHR  ((EPD_STOCK_PUSH_VDHR * EPD_STOCK_FRAME_US) / EPD_B_FRAME_US)  // 36

// These guards fail the build if the push limits are loosened or if a tone
// session's frame is not the 23.7 ms frame the limits are counted in.
#if !EPD_QUICK_OPEN
#error "EPD_PUSH_MAX_* are frames of 23.7 ms, which only the quick profile's oscillator write gives a tone session"
#endif
#if EPD_B_FRAME_US < 23700u
#error "an EPD_B_FRAME_US below the measured 23.7 ms frame loosens the push limits"
#endif
#if EPD_PUSH_MAX_VDH > 80u || EPD_PUSH_MAX_VDL > 99u || EPD_PUSH_MAX_VDHR > 36u
#error "the push limits are never loosened"
#endif

// --- Logging -----------------------------------------------------------------
#ifndef EPD_LOG
#define EPD_LOG 1
#endif
#ifndef EPD_LOG_VERBOSE
#define EPD_LOG_VERBOSE 0
#endif
