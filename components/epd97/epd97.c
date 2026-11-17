// -----------------------------------------------------------------------------
// epd97: Pervasive Displays iTC 9.7" dual-COG panel on an ESP32-S3, ESP-IDF.
//
// Ported from the EFR32FG22 OEPL driver (oepl_display_driver_pervasive_itc_
// medium.c), which is where all the measured numbers in the comments come from.
//
// The panel is two COGs behind one FPC, each owning half of a 960 px scan line.
// The left COG takes the left 60 bytes of every line and the right COG the right 60.
// Register writes go to both at once.
//
// Bus arrangement: register writes and the OTP readback are bitbanged, because
// they are a few dozen bytes and the OTP wants a ~50 kHz clock with the data
// pin turned around for the read. The two 80640-byte planes go out over SPI2
// with DMA. The two share MOSI and SCK, so the GPIO matrix is re-pointed
// between them and the bus is never torn down.
// -----------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "soc/spi_periph.h"
#include "soc/gpio_sig_map.h"
#include "soc/rtc_cntl_struct.h"

#include "epd97.h"
#include "epd97_config.h"
#include "epd97_lut.h"

static const char* TAG = "epd";

// The disabled forms keep the arguments compiled, so a value that only feeds a
// log line does not become an unused variable.
#if EPD_LOG
#define LOGF(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define LOGF(...) do { if (0) ESP_LOGI(TAG, __VA_ARGS__); } while (0)
#endif
#if EPD_LOG_VERBOSE
#define VLOGF(...) LOGF(__VA_ARGS__)
#else
#define VLOGF(...) do { if (0) ESP_LOGI(TAG, __VA_ARGS__); } while (0)
#endif

#define EPD_OTP_LEN   88
#define COG_M         0x01
#define COG_S         0x02
#define COG_BOTH      (COG_M | COG_S)
#define HALF_BYTES    (EPD_STRIDE / 2)        // 60
#define HALF_PLANE    (HALF_BYTES * EPD_H)    // 40320, one COG's share of a plane

// The vendor's update register sequence for the 9.7", TCON before STV. Each
// step is a register, its argument, a delay field and the quick-profile delay.
// In the delay field bit 0 says the argument is an OTP offset rather than a
// literal, the upper bits are units of 10 ms. qms is the wait in ms when
// s_quick is set (EPD_QUICK_OPEN in epd97_config.h).
typedef struct { uint8_t cmd; uint8_t arg; uint8_t dly; uint8_t qms; } upd_step_t;

static const upd_step_t k_upd[] = {
  { 0x05, 0x7D, 20u << 1, 50 },
  { 0x05, 0x00,  1u << 1,  1 },
  { 0xC2, 0x3F,  0,        0 },
  { 0xD8, 0x1D,  1,        0 },
  { 0xD6, 0x1E,  1,        0 },
  { 0xA7, 0x10, 10u << 1,  2 },
  { 0xA7, 0x00, 10u << 1, 10 },
  { 0x44, 0x00,  0,        0 },
  { 0x45, 0x80,  0,        0 },
  { 0xA7, 0x10, 10u << 1,  2 },
  { 0xA7, 0x00, 10u << 1, 10 },
  { 0x44, 0x06,  0,        0 },
  { 0x45, 0x86,  0,        0 },              // patched at runtime with the waveform bank
  { 0xA7, 0x10, 10u << 1,  2 },
  { 0xA7, 0x00, EPD_T_PRE_SS << 1, 10 },
  { 0x60, 0x0B,  1,        0 },
  { 0x61, 0x1C,  1,        0 },
  { 0x01, 0x10,  1,        0 },
  { 0x02, 0x11,  1,        0 },
};
#define UPD_STEPS (sizeof(k_upd) / sizeof(k_upd[0]))
// The steps before the four geometry registers that close k_upd. The last of
// them is the final 0xA7 0x00.
#define UPD_PRE_GEOM (UPD_STEPS - 4u)

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
static uint8_t*  s_bw;
static uint8_t*  s_red;
static bool      s_ready;

static spi_device_handle_t s_spi;
static uint8_t   s_cog = COG_BOTH;

static uint8_t   s_otp[EPD_OTP_LEN];
static bool      s_otp_valid;
static bool      s_driver_b;          // true = E2969JS0B1, false = E2969JS084
static epd_panel_t s_panel = EPD_PANEL_UNKNOWN;

// The OTP survives deep sleep in RTC memory. It is a fuse block on a panel that
// is still plugged into the same board, so re-probing it on every wake costs a
// full power cycle and about three seconds for an answer that cannot have
// changed. Only trusted after a deep-sleep wake, so unplugging the panel and
// power-cycling the board re-probes.
#define OTP_RTC_MAGIC 0x69544301u
RTC_DATA_ATTR static uint32_t s_rtc_magic;
RTC_DATA_ATTR static uint8_t  s_rtc_otp[EPD_OTP_LEN];
RTC_DATA_ATTR static uint8_t  s_rtc_driver_b;

static uint8_t   s_lut8[LUT8_BYTES];
static uint8_t   s_lutb[LUTB_BYTES];
static bool      s_lut8_has_red;
// s_lutb holds a table from epd_set_fast_lut() rather than the built-in one.
static bool      s_lutb_custom;
// s_lutb changed after the open session latched its waveform.
static bool      s_lutb_dirty;
static unsigned  s_fast_pulls = EPD_B_REFRESH_PULLS;

static bool      s_frame_has_red;
// Whether the picture currently ON the panel carries red. It has to survive
// deep sleep: what the film is holding is not reset by the ESP32 losing its
// RAM, and a wake that starts out believing the panel is red-free will run a
// fast refresh straight over red that nothing short can remove. RTC memory is
// cleared on a power-on reset, which is the case a cold boot's clearing pass
// covers anyway.
RTC_DATA_ATTR static bool s_prev_has_red;

static bool      s_session_open;
static bool      s_lut_latched;
static epd_refresh_t s_latched_mode = EPD_SLOW;
static uint32_t  s_session_images;
static bool      s_keep_awake;
static bool      s_run_refresh = (EPD_RUN_REFRESH != 0);
static volatile bool s_refreshing;
static bool      s_allow_fast_red;
static bool      s_headless;
static bool      s_otp_standin;
static bool      s_planes_only;

// Verb session. s_session_open and s_lut_latched are also set while it is open,
// so every path that closes a session closes this one.
static uint8_t   s_verb_lut[LUTB_BYTES];
static bool      s_verb_open;
static uint8_t   s_verb_unit;
static bool      s_verb_red;          // a pass in this session sent red plane bits
static epd_verb_table_t s_verb_table;

static uint32_t  s_power_off_ms;
static bool      s_power_off_valid;
// The panel supply switch is on. With EPD_KEEP_SUPPLY it stays on between
// sessions, with the COG held in reset.
static bool      s_supply_on;
static uint32_t  s_last_panel_ms;
static uint32_t  s_last_frame_ms;
static uint32_t  s_last_send_ms;
// Quick waits for this open (EPD_QUICK_OPEN). Set by the fast and verb
// openers on driver B, cleared by cog_stop_ex().
static bool      s_quick;
static int64_t   s_last_send_end_us;  // 0 until the first send

#if EPD_STAGE_INTERNAL
// Internal DMA memory, one COG's half of a plane each. s_stage[1] is NULL when
// the heap had room for only one.
static uint8_t*  s_stage[2];
#else
// One half line. The planes live in PSRAM and the line has to be gathered
// before DMA can send it.
static DMA_ATTR uint8_t s_txbuf[HALF_BYTES];
#endif

// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Scheduler ticks from 10 ms up, a busy wait below that. The tick waits assume
// CONFIG_FREERTOS_HZ 1000. At 100 Hz pdMS_TO_TICKS truncates to 10 ms steps.
static void delay_ms(uint32_t ms)
{
  if (ms >= 10u) vTaskDelay(pdMS_TO_TICKS(ms));
  else           esp_rom_delay_us(ms * 1000u);
}

// -----------------------------------------------------------------------------
// Pins
// -----------------------------------------------------------------------------
static inline void dc_set(int v)  { gpio_set_level((gpio_num_t)EPD_PIN_DC, v); }
static inline void rst_set(int v) { gpio_set_level((gpio_num_t)EPD_PIN_RST, v); }
static inline void clk_set(int v) { gpio_set_level((gpio_num_t)EPD_PIN_SCK, v); }
static inline void sdi_set(int v) { gpio_set_level((gpio_num_t)EPD_PIN_MOSI, v); }

static inline void cog_select(uint8_t which) { s_cog = which; }

static void cs_hi(void)
{
  gpio_set_level((gpio_num_t)EPD_PIN_CS_M, 1);
  gpio_set_level((gpio_num_t)EPD_PIN_CS_S, 1);
}

// Raise the deselected side before lowering the selected one, so the two COGs
// are never both listening.
static void cs_lo(void)
{
  if (!(s_cog & COG_M)) gpio_set_level((gpio_num_t)EPD_PIN_CS_M, 1);
  if (!(s_cog & COG_S)) gpio_set_level((gpio_num_t)EPD_PIN_CS_S, 1);
  if (s_cog & COG_M)    gpio_set_level((gpio_num_t)EPD_PIN_CS_M, 0);
  if (s_cog & COG_S)    gpio_set_level((gpio_num_t)EPD_PIN_CS_S, 0);
}

// Both COGs have to report ready. A panel with only the left COG BUSY wired can
// set EPD_PIN_BUSY_S to -1.
static bool busy_idle(void)
{
  if (!gpio_get_level((gpio_num_t)EPD_PIN_BUSY_M)) return false;
#if EPD_PIN_BUSY_S >= 0
  if (!gpio_get_level((gpio_num_t)EPD_PIN_BUSY_S)) return false;
#endif
  return true;
}

// -----------------------------------------------------------------------------
// Bus: bitbang and hardware SPI over the same two pins
// -----------------------------------------------------------------------------
// Only the two bus pins change hands. DC, RST and the two chip selects are
// configured as outputs once in epd_begin() and stay driven while the panel
// supply is off. Releasing them with the supply off stopped the refresh at the
// DC/DC soft start with BUSY never ready.
static void bus_bitbang(void)
{
  esp_rom_gpio_connect_out_signal(EPD_PIN_MOSI, SIG_GPIO_OUT_IDX, false, false);
  esp_rom_gpio_connect_out_signal(EPD_PIN_SCK,  SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)EPD_PIN_SCK,  GPIO_MODE_OUTPUT);
  clk_set(0);
  sdi_set(0);
}

static void bus_spi(void)
{
  esp_rom_gpio_connect_out_signal(
      EPD_PIN_MOSI, spi_periph_signal[EPD_SPI_HOST_NUM].spid_out, false, false);
  esp_rom_gpio_connect_out_signal(
      EPD_PIN_SCK, spi_periph_signal[EPD_SPI_HOST_NUM].spiclk_out, false, false);
}

// Mode 0 by hand: set the data, raise the clock, lower it. The panel samples on
// the rising edge and the clock idles low.
static void bb_byte(uint8_t v)
{
  for (uint8_t i = 0; i < 8; i++) {
    sdi_set(v & 0x80u);
    clk_set(1);
    clk_set(0);
    v <<= 1;
  }
}

// ~50 kHz. The panel does not follow a full-speed bitbang for the OTP.
static void bb_out_slow(uint8_t v)
{
  for (uint8_t i = 0; i < 8; i++) {
    sdi_set(v & 0x80u);
    esp_rom_delay_us(10);
    clk_set(1);
    esp_rom_delay_us(10);
    clk_set(0);
    esp_rom_delay_us(10);
    v <<= 1;
  }
}

static uint8_t bb_in_slow(void)
{
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; i++) {
    clk_set(1);
    esp_rom_delay_us(10);
    v = (uint8_t)((v << 1) | (gpio_get_level((gpio_num_t)EPD_PIN_MOSI) ? 1u : 0u));
    clk_set(0);
    esp_rom_delay_us(10);
  }
  return v;
}

static void send_index(uint8_t index, const uint8_t* data, uint16_t len)
{
  dc_set(0);
  cs_lo();
  bb_byte(index);
  cs_hi();
  if (len == 0) return;
  dc_set(1);
  cs_lo();
  while (len--) bb_byte(*data++);
  cs_hi();
}

static void send_u8(uint8_t index, uint8_t v)          { send_index(index, &v, 1); }
static void send_u8x2(uint8_t index, uint8_t a, uint8_t b)
{
  uint8_t d[2] = { a, b };
  send_index(index, d, 2);
}
static void send_otp(uint8_t index, uint8_t off, uint8_t n)
{
  send_index(index, &s_otp[off], n);
}

// -----------------------------------------------------------------------------
// Power and reset
// -----------------------------------------------------------------------------
static void power_on(void)
{
  // The booster rails have to bleed down before the next cycle powers up, or
  // the COG sits on the update waiting for its own voltages. This shows up
  // mostly on the second refresh, which is what points at the rails.
  if (s_power_off_valid) {
    uint32_t off = now_ms() - s_power_off_ms;
    if (off < EPD_T_OFF_MIN) {
      VLOGF("panel only off %ums, waiting out %ums",
            (unsigned)off, (unsigned)(EPD_T_OFF_MIN - off));
      delay_ms(EPD_T_OFF_MIN - off);
    }
    s_power_off_valid = false;
  }
  gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 1 : 0);
  s_supply_on = true;
}

static void power_off(void)
{
  gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 0 : 1);
  s_power_off_ms = now_ms();
  s_power_off_valid = true;
  s_supply_on = false;
}

static void cog_reset(void)
{
  power_on();
  delay_ms(EPD_T_RST_PWR);
  rst_set(1);
  delay_ms(EPD_T_RST_HI);
  rst_set(0);
  delay_ms(EPD_T_RST_LO);
  rst_set(1);
  delay_ms(EPD_T_RST_SET);
}

// -----------------------------------------------------------------------------
// OTP: which panel is this?
// -----------------------------------------------------------------------------
// The opener has gone out with the left COG's chip select still low. Turns
// MOSI around, drops the dummy byte when `dummy` is set and reads `n` bytes
// into `dst`.
static void otp_read_tail(uint8_t* dst, size_t n, bool dummy)
{
  delay_ms(1);

  dc_set(1);
  gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)EPD_PIN_MOSI, GPIO_PULLDOWN_ONLY);
  if (dummy) (void)bb_in_slow();
  for (size_t i = 0; i < n; i++) dst[i] = bb_in_slow();
  cs_hi();
  gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
  gpio_set_pull_mode((gpio_num_t)EPD_PIN_MOSI, GPIO_FLOATING);
  sdi_set(0);
  cog_select(COG_BOTH);
}

// `n` bytes after the dummy byte into `dst`. The probe reads EPD_OTP_LEN.
// epd_otp_dump() reads further to see what the stream holds past the block.
static void otp_read_raw(bool use_b9, uint8_t* dst, size_t n)
{
  cog_select(COG_M);
  cog_reset();

  if (use_b9) {
    // Driver B opens the session with 0x70 before the OTP read. A panel that
    // never gets that register draws about 2 A during refresh, so it is
    // configuring the booster. It goes to both COGs. Only the read itself is
    // left COG only, because both COGs would otherwise drive the bus.
    cog_select(COG_BOTH);
    send_u8x2(0x70, 0x71, 0x36);
    cog_select(COG_M);
    dc_set(0);
    cs_lo();
    bb_out_slow(0xB9);
  } else {
    send_u8(0xA1, 0x00);
    send_u8(0xA7, 0x80);
    send_u8x2(0xA3, 0x00, 0x40);
    dc_set(0);
    cs_lo();
    bb_out_slow(0xA8);
  }
  otp_read_tail(dst, n, true);
}

// The 0xA7 steps of k_upd wait 10 units of 10 ms.
#define OTP_A7_WAIT_MS 100u

// otp_read_raw() with the writes before the opener taken from `p`. Used only
// by epd_otp_dump_ex(). Panel detection uses otp_read_raw().
static void otp_read_probe(const epd_otp_probe_t* p, uint8_t* dst, size_t n)
{
  cog_select(COG_M);
  cog_reset();

  cog_select(COG_BOTH);
  if (!p->skip_0x70) send_u8x2(0x70, 0x71, 0x36);
  if (p->set_addr) {
    send_u8(0xA1, 0x00);
    send_u8(0xA7, 0x80);
    send_u8x2(0xA3, (uint8_t)(p->addr >> 8), (uint8_t)(p->addr & 0xFFu));
  }
  if (p->set_ptr) {
    send_u8(0x44, p->ptr);
    send_u8(0x45, p->bank);
    send_u8(0xA7, 0x10);
    delay_ms(OTP_A7_WAIT_MS);
    send_u8(0xA7, 0x00);
    delay_ms(OTP_A7_WAIT_MS);
  }
  cog_select(COG_M);
  dc_set(0);
  cs_lo();
  bb_out_slow(p->opener);
  otp_read_tail(dst, n, true);
}

// Stage 0 of the DC/DC ramp must not be 0xFF. That ramps the booster 127x and
// hangs BUSY. All four stages are checked: the booster loop runs the OTP repeat
// counts verbatim, so a garbled block has to be rejected here and not clamped
// later.
static bool otp_sane(void)
{
  for (uint8_t stage = 0; stage < 4; stage++) {
    uint8_t sb = s_otp[0x20 + 8 * stage];
    uint8_t rp = (uint8_t)(sb & 0x7F);
    if (sb == 0xFF || rp == 0 || rp > 40) {
      LOGF("OTP ramp stage %u implausible: 0x%02X", (unsigned)stage, (unsigned)sb);
      return false;
    }
  }
  // Both variants are checked in the driver-8 layout (driver B is remapped into
  // it first). DUW sits at 0x16..0x1B and DRFW at 0x0C..0x0F, and for a 960 px
  // dual COG both carry x_end = 0x3B, 60 bytes per half.
  if (s_otp[0x17] != 0x3B || s_otp[0x0D] != 0x3B) {
    LOGF("OTP window check failed: DUW xe=0x%02X DRFW xe=0x%02X",
         (unsigned)s_otp[0x17], (unsigned)s_otp[0x0D]);
    return false;
  }
  return true;
}

static bool otp_looks_like_driver_b(void)
{
  return (s_otp[0x16] == 0x3B && s_otp[0x0D] == 0x3B && (s_otp[0x28] & 0x80) != 0);
}

static void otp_remap_b_to_8(void)
{
  for (uint8_t i = 0x1E; i > 0x12; i--) s_otp[i] = s_otp[i - 1];
  for (uint8_t i = 0; i < 32; i++)      s_otp[0x20 + i] = s_otp[0x28 + i];
  s_otp[0] = 0xA5;
}

static void otp_stash(void)
{
  memcpy(s_rtc_otp, s_otp, EPD_OTP_LEN);
  s_rtc_driver_b = s_driver_b ? 1 : 0;
  s_rtc_magic = OTP_RTC_MAGIC;
}

// Recover the probe result from a previous wake, if this boot is one.
static bool otp_restore(void)
{
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) return false;
  if (s_rtc_magic != OTP_RTC_MAGIC) return false;
  memcpy(s_otp, s_rtc_otp, EPD_OTP_LEN);
  s_driver_b = (s_rtc_driver_b != 0);
  s_panel = s_driver_b ? EPD_PANEL_B1 : EPD_PANEL_084;
  s_otp_valid = true;
  return true;
}

// The OTP is a fuse block: it cannot change between refreshes, and reading it
// costs about 2.8 s because it is clocked out at 50 kHz and tried twice, once
// per variant. So it is read once and kept.
static int otp_read(void)
{
  // The stand-in block is only there so a planes-only transfer has window
  // registers to send. A real waveform must not ramp the booster from it.
  if (s_otp_valid && s_otp_standin && s_run_refresh && !s_headless) {
    s_otp_valid = false;
    s_otp_standin = false;
  }
  if (s_otp_valid) {
    VLOGF("OTP from cache (%s)",
          s_otp_standin ? "stand-in" : (s_driver_b ? "driver B" : "driver 8"));
    return 0;
  }

  // Driver B first, then driver 8. That is the order the stock firmware uses.
  otp_read_raw(true, s_otp, EPD_OTP_LEN);
  if (otp_looks_like_driver_b()) {
    otp_remap_b_to_8();
    s_driver_b = true;
    if (!otp_sane()) {
      LOGF("driver-B remap failed the soft-start sanity check");
      return -1;
    }
    s_panel = EPD_PANEL_B1;
    s_otp_valid = true;
    LOGF("E2969JS0B1 (driver B) detected");
    otp_stash();
    return 0;
  }

  otp_read_raw(false, s_otp, EPD_OTP_LEN);
  s_driver_b = false;
  if (s_otp[0] != 0xA5 || !otp_sane()) {
    LOGF("driver-8 OTP read unusable (hdr=0x%02X)", (unsigned)s_otp[0]);
    return -2;
  }
  s_panel = EPD_PANEL_084;
  s_otp_valid = true;
  LOGF("E2969JS084 (driver 8) detected");
  otp_stash();
  return 0;
}

#if EPD_KEEP_SUPPLY && !EPD_SEND_0x70_EVERY_POWERUP
#error "EPD_KEEP_SUPPLY needs EPD_SEND_0x70_EVERY_POWERUP: an open on a kept supply starts at the reset, and driver B needs 0x70 after every reset"
#endif

static int cog_initial(void)
{
  // With the OTP cached, otp_read() returns without ever reaching cog_reset(),
  // so the power-up and the rail settle have to happen here instead. A supply
  // kept on by cog_close() needs neither.
  const bool cached = s_otp_valid;
  const bool warm = EPD_KEEP_SUPPLY && s_supply_on;
  if (cached && !warm) {
    power_on();
    delay_ms(EPD_T_RST_PWR);
  }
  if (warm) LOGF("supply kept on, off floor and %u ms settle skipped", (unsigned)EPD_T_RST_PWR);
  int rc = otp_read();
  if (rc != 0) {
    LOGF("OTP failed (%d), aborting so the booster is not left on", rc);
    return rc;
  }
  // Second reset after the OTP read, as the stock firmware does. The quick
  // profile uses the yig-os reset shape. cog_close() leaves RST low on a kept
  // supply, and the pulse starts from high.
  if (warm) rst_set(1);
  delay_ms(s_quick ? EPD_TQ_RST_HI : EPD_T_OTP_HI);
  rst_set(0);
  delay_ms(s_quick ? EPD_TQ_RST_LO : EPD_T_OTP_LO);
  rst_set(1);
  delay_ms(s_quick ? EPD_TQ_RST_SET : EPD_T_OTP_SET);
#if EPD_SEND_0x70_EVERY_POWERUP
  // The probe's 0x70 does not survive the supply cut in cog_stop(), so driver
  // B gets it again after the last reset. It has to precede the window
  // registers in write_plane().
  if (s_driver_b) {
    cog_select(COG_BOTH);
    send_u8x2(0x70, 0x71, 0x36);
  }
#endif
  return 0;
}

// -----------------------------------------------------------------------------
// Plane transfer
// -----------------------------------------------------------------------------
// `orsrc`, when given, is merged in bit by bit. That is how a fast refresh
// draws red as ink: the red plane is OR'ed into the black one on the way out
// and the red plane itself goes out empty. A NULL `fb` sends zeros.
#if EPD_STAGE_INTERNAL

// One COG's half of a plane, line after line. `half` is 0 for the left COG and
// HALF_BYTES for the right COG.
static void stage_half(uint8_t* dst, const uint8_t* fb, const uint8_t* orfb, size_t half)
{
  for (int y = 0; y < EPD_H; y++) {
    const size_t off = (size_t)y * EPD_STRIDE + half;
    uint8_t* d = dst + (size_t)y * HALF_BYTES;
    if (fb) memcpy(d, fb + off, HALF_BYTES);
    else    memset(d, 0, HALF_BYTES);
    if (orfb) {
      for (unsigned i = 0; i < HALF_BYTES; i++) d[i] |= orfb[off + i];
    }
  }
}

// The S3 GPSPI takes at most SPI_LL_DATA_MAX_BIT_LEN bits (32768 bytes) per
// transaction, so a 40320-byte half goes out as two chunks. The COG keeps its
// RAM pointer across chip select, so the chunks and the chip select swap per
// plane are invisible to it.
#define SPI_CHUNK (HALF_PLANE / 2)   // 20160

static void send_chunks(const uint8_t* buf, size_t len)
{
  while (len) {
    const size_t n = len > SPI_CHUNK ? SPI_CHUNK : len;
    spi_transaction_t t = { .length = n * 8u, .tx_buffer = buf };
    esp_err_t e = spi_device_polling_transmit(s_spi, &t);
    if (e != ESP_OK) {
      LOGF("spi transmit failed: %s", esp_err_to_name(e));
      return;
    }
    buf += n;
    len -= n;
  }
}

static void send_plane_data(const uint8_t* fb, const uint8_t* orfb)
{
  uint8_t* a = s_stage[0];
  uint8_t* b = s_stage[1] ? s_stage[1] : s_stage[0];

  stage_half(a, fb, orfb, 0);
  cog_select(COG_M);
  cs_lo();
  if (b != a) {
    // The right half is gathered while the first chunk of the left half is on
    // the wire. polling_end runs only after a successful polling_start.
    spi_transaction_t t = { .length = SPI_CHUNK * 8u, .tx_buffer = a };
    esp_err_t e = spi_device_polling_start(s_spi, &t, portMAX_DELAY);
    stage_half(b, fb, orfb, HALF_BYTES);
    if (e == ESP_OK) spi_device_polling_end(s_spi, portMAX_DELAY);
    else LOGF("spi start failed: %s", esp_err_to_name(e));
    send_chunks(a + SPI_CHUNK, HALF_PLANE - SPI_CHUNK);
  } else {
    // One buffer. It is read until the last chunk ends, so gather after.
    send_chunks(a, HALF_PLANE);
    stage_half(b, fb, orfb, HALF_BYTES);
  }

  cog_select(COG_S);
  cs_lo();
  send_chunks(b, HALF_PLANE);
}

#else

static void spi_half(uint8_t which, const uint8_t* src, const uint8_t* orsrc)
{
  memcpy(s_txbuf, src, HALF_BYTES);
  if (orsrc) {
    for (unsigned i = 0; i < HALF_BYTES; i++) s_txbuf[i] |= orsrc[i];
  }
  cog_select(which);
  cs_lo();
  spi_transaction_t t = { .length = HALF_BYTES * 8u, .tx_buffer = s_txbuf };
  spi_device_polling_transmit(s_spi, &t);
}

// Every line goes out as 60 bytes to the left COG, chip select swapped, 60 bytes
// to the right COG.
static void send_plane_data(const uint8_t* fb, const uint8_t* orfb)
{
  static const uint8_t zeros[HALF_BYTES] = { 0 };

  for (int y = 0; y < EPD_H; y++) {
    const size_t off = (size_t)y * EPD_STRIDE;
    const uint8_t* line = fb   ? (fb + off)   : zeros;
    const uint8_t* orl  = orfb ? (orfb + off) : NULL;
    spi_half(COG_M, line, orl);
    spi_half(COG_S, fb ? (line + HALF_BYTES) : zeros,
                    orl ? (orl + HALF_BYTES) : NULL);
  }
}

#endif

// 0x10 is the black/white plane, 0x13 the red one. The window registers come
// out of the OTP and have to precede the first plane. A NULL framebuffer sends
// a plane of zeros, which is what the red plane is on a frame with no red. The
// COG wants both planes either way. `orfb` is merged in per line if given.
static void write_plane(uint8_t index, const uint8_t* fb, const uint8_t* orfb)
{
  if (index == 0x10) {
    // Driver B sets DCTL before the window registers, exactly as its stock
    // trace does. Driver 8 sends 0x01 at the end of the update sequence. A
    // resend inside a latched session leaves DCTL alone: the stock plane
    // resend (epd.py _send_images) writes only DUW, DRFW, RAM_RW and data, and
    // that is the only resend seen to keep a 0xB0 table latched.
    if (s_driver_b && !s_lut_latched) send_u8(0x01, s_otp[0x10]);
    send_otp(0x13, 0x16, 6);   // DUW
    send_otp(0x90, 0x0C, 4);   // DRFW
    send_otp(0x12, 0x13, 3);   // RAM_RW
  } else {
    send_otp(0x12, 0x13, 3);
    index = 0x11;
  }

  dc_set(0);
  cs_lo();
  bb_byte(index);
  cs_hi();
  dc_set(1);

  bus_spi();
  send_plane_data(fb, orfb);
  bus_bitbang();
  cs_hi();
  cog_select(COG_BOTH);
}

// Both planes, timed for epd_last_send_ms().
static void send_planes(bool have_red, bool draw_red)
{
  const int64_t t0 = esp_timer_get_time();
  write_plane(0x10, s_bw, (have_red && !draw_red) ? s_red : NULL);
  write_plane(0x13, draw_red ? s_red : NULL, NULL);
  s_last_send_end_us = esp_timer_get_time();
  s_last_send_ms = (uint32_t)((s_last_send_end_us - t0) / 1000);
}

// -----------------------------------------------------------------------------
// Refresh
// -----------------------------------------------------------------------------
static void stage_delay(uint8_t d)
{
  if (d & 0x80) delay_ms((d & 0x7F) ? (d & 0x7F) : 1);
  else          esp_rom_delay_us(10u * (uint32_t)(d & 0x7F));
}

// Literally the vendor loop: the repeat count is whatever the OTP says, no
// skipping and no cap. Ramping the booster differently is what stresses a
// panel, so the OTP is validated up front instead and a garbled block never
// gets this far.
static void dcdc_soft_start(void)
{
  unsigned rounds = 0;
  for (uint8_t stage = 0; stage < 4; stage++) {
    uint8_t off = (uint8_t)(0x20 + 8 * stage);
    uint8_t rep = (uint8_t)(s_otp[off] & 0x7F);
    rounds += rep;
    if (s_otp[off] & 0x80) {
      uint8_t ph0 = s_otp[off + 1];
      uint8_t ph1 = s_otp[off + 2];
      for (uint8_t i = 0; i < rep; i++) {
        send_u8(0x09, s_otp[off + 5]);
        ph0 = (uint8_t)(ph0 + s_otp[off + 3]);
        ph1 = (uint8_t)(ph1 + s_otp[off + 4]);
        send_u8x2(0x51, ph0, ph1);
        send_u8(0x09, s_otp[off + 6]);
        stage_delay(s_otp[off + 7]);
      }
    } else {
      for (uint8_t i = 0; i < rep; i++) {
        send_u8(0x09, s_otp[off + 1]);
        stage_delay(s_otp[off + 3]);
        send_u8(0x09, s_otp[off + 2]);
        stage_delay(s_otp[off + 4]);
      }
    }
  }
  VLOGF("soft start done, %u booster rounds", rounds);
}

// The long LUT holds the panel rails up for about half a minute. On this
// board that dips VDD far enough to fire the brownout detector, which
// reboots the chip. Hold the detector off for the whole waveform, including
// the gaps between BUSY waits.
static uint32_t s_bod_brown;
static uint32_t s_bod_int_ena;
static bool s_bod_held;

static void bod_hold_off(void)
{
  if (s_bod_held) return;
  s_bod_brown = RTCCNTL.brown_out.val;
  s_bod_int_ena = RTCCNTL.int_ena.val;
  RTCCNTL.brown_out.rst_ena = 0;
  RTCCNTL.brown_out.ana_rst_en = 0;
  RTCCNTL.brown_out.ena = 0;
  RTCCNTL.int_ena.rtc_brown_out = 0;
  RTCCNTL.int_clr.rtc_brown_out = 1;
  s_bod_held = true;
}

static void bod_restore(void)
{
  if (!s_bod_held) return;
  RTCCNTL.int_clr.rtc_brown_out = 1;
  RTCCNTL.brown_out.val = s_bod_brown;
  RTCCNTL.int_ena.val = s_bod_int_ena;
  s_bod_held = false;
}

static int wait_busy(unsigned want_idle, uint32_t timeout_ms, const char* tag)
{
  // No COG answered the probe. A floating BUSY line must not hold the caller
  // for the full waveform timeout.
  if (s_headless && timeout_ms > 2000u) timeout_ms = 2000u;
  // esp_task_wdt_reset() logs an error on every call from a task the watchdog
  // does not watch, so it is only called for a subscribed task.
#if CONFIG_ESP_TASK_WDT_INIT
  const bool feed_wdt = (esp_task_wdt_status(NULL) == ESP_OK);
#else
  const bool feed_wdt = false;
#endif
  uint32_t waited = 0;
  LOGF("BUSY '%s' wait up to %us (want_idle=%u, pin=%u)",
       tag, (unsigned)(timeout_ms / 1000u), want_idle, (unsigned)busy_idle());
  int result = -1;
  while (waited < timeout_ms) {
    if (want_idle ? busy_idle() : !busy_idle()) {
      result = (int)waited;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    waited += 10;
    // Nothing is logged inside this loop. A console write can block while no
    // host drains the USB port, and this loop runs for up to 180 s.
    if (feed_wdt) (void)esp_task_wdt_reset();
  }
  if (result < 0) {
    LOGF("BUSY wait '%s' timed out after %ums (want_idle=%u, pin=%u)",
         tag, (unsigned)timeout_ms, want_idle, (unsigned)busy_idle());
  }
  return result;
}

// Trigger the update again without touching any register the waveform lives in.
// Everything the panel needs is already loaded: the planes, the table and the
// booster rails. idle_ms bounds the wait for the pull to finish.
static int retrigger(uint32_t idle_ms)
{
  send_u8(0x15, 0x3C);
  if (wait_busy(0, 2000, "post-0x15 busy") < 0) return -2;
  int waited = wait_busy(1, idle_ms, "refresh done");
  if (waited < 0) return -3;
  s_last_panel_ms = (uint32_t)waited;
  return 0;
}

// The settle and the first `steps` entries of k_upd. do_refresh() sends all of
// them. epd_reg_read() stops before the geometry registers.
static void send_upd(size_t steps)
{
  const uint8_t reg45 = s_driver_b ? (uint8_t)EPD_REG45_B1 : (uint8_t)EPD_REG45_084;

  delay_ms(s_quick ? EPD_TQ_PRE_UPD : EPD_T_PRE_UPD);

  for (size_t i = 0; i < steps; i++) {
    uint8_t d = k_upd[i].dly;
    if (s_driver_b && (k_upd[i].cmd == 0xC2 || k_upd[i].cmd == 0x01)) continue;

    uint8_t arg = (d & 1) ? s_otp[k_upd[i].arg] : k_upd[i].arg;
    // The second 0x45 of the pair carries the waveform bank. The first stays
    // at the driver's own 0x80.
    if (k_upd[i].cmd == 0x45 && k_upd[i].arg != 0x80) arg = reg45;

    send_u8(k_upd[i].cmd, arg);
    if (s_quick) {
      if (k_upd[i].qms) delay_ms(k_upd[i].qms);
    } else if (d >> 1) {
      delay_ms((uint32_t)(d >> 1) * 10u);
    }

    // 0x03 is the oscillator. The stock sequence only sends it on driver 8.
    // Driver B gets the yig-os value in the quick profile: 23.7 ms frames
    // instead of 29 ms.
    if (i == 6) {
      if (!s_driver_b)  send_u8x2(0x03, 0x00, s_otp[0x12]);
      else if (s_quick) send_u8x2(0x03, 0x00, 0x11);
    }
  }
}

static int do_refresh(epd_refresh_t mode, bool need_red_lut)
{
  send_upd(UPD_STEPS);

  // After the register table and before the soft start: everything above would
  // have overwritten 0xB0, and nothing below touches it. Both chip selects are
  // low here, so both COGs get the same table in one go.
  if (mode == EPD_FAST) {
    if (s_driver_b) {
      send_index(0xB0, s_lutb, (uint16_t)LUTB_BYTES);
      s_lutb_dirty = false;
    } else {
      // Built here and not once at startup: which table this frame needs
      // depends on whether it carries red, and on whether the one before it did.
      epd_build_lut8(s_lut8, need_red_lut, need_red_lut && !s_frame_has_red);
      s_lut8_has_red = need_red_lut;
      send_index(0xB0, s_lut8, (uint16_t)LUT8_BYTES);
    }
  }

  dcdc_soft_start();

  // Stock fires 0x15 about 100 ms after the last soft-start write. The 084 is
  // ready by then. The B1 still holds BUSY low because its DC/DC ramp is not
  // finished, and starting the update into a booster that is still ramping is
  // the intermittent "sits still for a minute, then draws" symptom.
  delay_ms(100);
  if (!busy_idle()) {
    int ready = wait_busy(1, 10000, "pre-0x15 ready");
    if (ready < 0) LOGF("booster never reported ready in 10 s, going anyway");
    else           VLOGF("booster ready %d ms after the soft start", ready);
  }

  send_u8(0x15, 0x3C);
  if (wait_busy(0, 2000, "post-0x15 busy") < 0) {
    LOGF("BUSY never went low after 0x15");
    return -2;
  }
  int waited = wait_busy(1, EPD_IDLE_WAIT_MS, "refresh done");
  if (waited < 0) return -3;

  // Watch a little longer: some panels drop BUSY again for a second waveform
  // pass, which is what a refresh that seems to sit still and then suddenly
  // draws actually is.
  for (uint32_t i = 0; i < EPD_SECOND_PASS_MS / 10u; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!busy_idle()) {
      int again = wait_busy(1, EPD_IDLE_WAIT_MS, "second pass");
      if (again > 0) waited += again;
      break;
    }
  }
  s_last_panel_ms = (uint32_t)waited;
  return 0;
}

// The DC/DC soft shutdown. cut true then cuts the supply. cut false leaves
// the supply on and the COG in reset with RST low, as the stock firmware and
// yig-os close a session. A BUSY that stays low past EPD_T_SHUTDOWN_BUSY
// turns a kept supply into a cut.
static void cog_stop_ex(bool cut)
{
  send_u8(0x09, 0x7F);
  delay_ms(20);
  send_u8(0x05, 0x7D);
  send_u8(0x09, 0x00);
  delay_ms(200);
  if (!cut && wait_busy(1, EPD_T_SHUTDOWN_BUSY, "dc-dc off") < 0) cut = true;
  dc_set(0);
  cs_lo();
  if (!cut) {
    rst_set(0);
    cs_hi();
    s_quick = false;
    return;
  }
  delay_ms(150);
  rst_set(0);
  power_off();
  // Held in reset while the rails come down.
  delay_ms(EPD_T_DISCHARGE);
  cs_hi();
  rst_set(1);
  s_quick = false;
}

// Error paths and the final shutdown.
static void cog_stop(void) { cog_stop_ex(true); }

// The end of a session that completed.
static void cog_close(void) { cog_stop_ex(EPD_KEEP_SUPPLY == 0); }

// Cuts a supply that cog_close() left on. The COG is already shut down and
// held in reset, so only the supply cut and the discharge remain.
static void supply_cut(void)
{
  if (!s_supply_on) return;
  dc_set(0);
  cs_lo();
  rst_set(0);
  power_off();
  delay_ms(EPD_T_DISCHARGE);
  cs_hi();
  rst_set(1);
}

// -----------------------------------------------------------------------------
// Public
// -----------------------------------------------------------------------------
static bool frame_has_red(void)
{
  const uint32_t* p = (const uint32_t*)s_red;
  for (size_t i = 0; i < EPD_PLANE_BYTES / 4; i++) if (p[i]) return true;
  return false;
}

// Which waveform a frame runs and whether it draws red. Same rules as
// esp32s3-itc97-drag-drop/src/epd_policy.c. With allow-fast-red on, a fast
// request is never promoted: promoting it would draw the red again, leave
// s_prev_has_red set, and force every later red image onto the full waveform.
static void refresh_choice(bool requested_fast, bool have_red,
                           bool* drive_fast, bool* draw_red)
{
  *drive_fast = requested_fast;
  *draw_red = have_red &&
      (!requested_fast || (EPD_FAST_ALLOW_RED != 0) || s_allow_fast_red);
  if (requested_fast && s_allow_fast_red) {
    *drive_fast = true;
  } else if (requested_fast && s_prev_has_red && !s_headless) {
    *drive_fast = false;
    *draw_red = false;
  }
}

// Enough of a geometry block for write_plane() to emit the window registers a
// 960-wide dual COG expects. Not a captured OTP: the booster repeat counts
// stay zero so this must never be handed to dcdc_soft_start().
static void otp_standin(void)
{
  memset(s_otp, 0, sizeof s_otp);
  s_otp[0x00] = 0xA5;
  s_otp[0x0D] = 0x3B;   // DRFW x_end, 60 bytes per half
  s_otp[0x17] = 0x3B;   // DUW x_end
  s_driver_b = false;
  s_panel = EPD_PANEL_UNKNOWN;
  s_otp_valid = true;
  s_otp_standin = true;
}

// Clock both planes out and return. No 0x15, no BUSY poll. The first call
// still powers and resets the COG so the bytes land on a live panel when one
// is wired. That path uses fixed delays, not the refresh timeout.
static bool send_planes_only(uint32_t t_frame, bool have_red, bool draw_red)
{
  if (!s_spi) {
    LOGF("SPI was not set up, planes not sent");
    return false;
  }
  if (!s_session_open) {
    // Nothing is attached, so there are no booster rails to bleed down.
    if (s_headless) s_power_off_valid = false;
    bus_bitbang();
    if (cog_initial() != 0) {
      LOGF("cog_initial failed, sending the planes anyway");
      if (!s_otp_valid) otp_standin();
    }
    s_session_open = true;
    s_session_images = 0;
  } else {
    bus_bitbang();
  }

  send_planes(have_red, draw_red);
  s_session_images++;
  s_planes_only = true;
  s_last_panel_ms = 0;
  s_last_frame_ms = now_ms() - t_frame;
  LOGF("planes on SPI, %u bytes x2 in %u ms, frame %u ms, BUSY not waited",
       (unsigned)EPD_PLANE_BYTES, (unsigned)s_last_send_ms,
       (unsigned)s_last_frame_ms);
  return true;
}

bool epd_begin(void)
{
  ESP_LOGI(TAG, "9.7\" iTC, %ux%u, planes %u bytes each",
           (unsigned)EPD_W, (unsigned)EPD_H, (unsigned)EPD_PLANE_BYTES);

  s_bw  = (uint8_t*)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  s_red = (uint8_t*)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_bw || !s_red) {
    // No PSRAM: 161 kB of internal RAM is a squeeze but not impossible.
    LOGF("no PSRAM, falling back to internal RAM");
    free(s_bw); free(s_red);
    s_bw  = (uint8_t*)malloc(EPD_PLANE_BYTES);
    s_red = (uint8_t*)malloc(EPD_PLANE_BYTES);
  }
  if (!s_bw || !s_red) {
    LOGF("could not allocate the framebuffer");
    return false;
  }
  epd_clear();

#if EPD_STAGE_INTERNAL
  s_stage[0] = (uint8_t*)heap_caps_malloc(HALF_PLANE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  s_stage[1] = (uint8_t*)heap_caps_malloc(HALF_PLANE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!s_stage[0]) {
    s_stage[0] = s_stage[1];
    s_stage[1] = NULL;
  }
  if (!s_stage[0]) {
    LOGF("could not allocate the SPI staging buffer");
    return false;
  }
  if (!s_stage[1]) LOGF("one SPI staging buffer, halves not overlapped");
#endif

  // Park the supply switch BEFORE the pin becomes an output: the OUT register
  // is written while it is still high-Z, so the first level the gate ever sees
  // driven is the off one. Without this the pin leaves gpio_config at 0, and
  // on a P-channel high-side switch 0 is ON.
  gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 0 : 1);

  const uint64_t out_mask =
      (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_PWR) |
      (1ULL << EPD_PIN_CS_M) | (1ULL << EPD_PIN_CS_S);
  gpio_config_t oc = { .pin_bit_mask = out_mask, .mode = GPIO_MODE_OUTPUT };
  gpio_config(&oc);

  // Release the deep-sleep latch only now, with the pad already configured and
  // the OUT register already holding the off level, so the level never lapses
  // between the latch letting go and this driving it.
  gpio_hold_dis((gpio_num_t)EPD_PIN_PWR);
  gpio_deep_sleep_hold_dis();

  uint64_t in_mask = (1ULL << EPD_PIN_BUSY_M);
#if EPD_PIN_BUSY_S >= 0
  in_mask |= (1ULL << EPD_PIN_BUSY_S);
#endif
  gpio_config_t ic = {
    .pin_bit_mask = in_mask,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&ic);

  power_off();
  s_power_off_valid = false;   // nothing to bleed down on a cold start
  rst_set(1);
  cs_hi();
  dc_set(1);

  // One transaction carries one COG's half of a plane.
  spi_bus_config_t bus = {
    .mosi_io_num = EPD_PIN_MOSI,
    .miso_io_num = -1,
    .sclk_io_num = EPD_PIN_SCK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = HALF_PLANE,
  };
  if (spi_bus_initialize(EPD_SPI_HOST_NUM, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    LOGF("spi_bus_initialize failed");
    return false;
  }
  spi_device_interface_config_t dev = {
    .clock_speed_hz = EPD_SPI_HZ,
    .mode = 0,                  // the panel samples on the rising edge
    .spics_io_num = -1,         // both chip selects are driven by hand
    .queue_size = 1,
  };
  if (spi_bus_add_device(EPD_SPI_HOST_NUM, &dev, &s_spi) != ESP_OK) {
    LOGF("spi_bus_add_device failed");
    return false;
  }
  bus_bitbang();

  epd_build_lut8(s_lut8, false, false);
  if (!s_lutb_custom) epd_build_lutb(s_lutb);

  // One power-up to find out what is attached, unless a previous wake already
  // answered that. The OTP is cached from here on either way.
  if (otp_restore()) {
    LOGF("panel from RTC memory, probe skipped");
  } else if (cog_initial() != 0) {
    cog_stop();
    otp_standin();
    s_headless = true;
    LOGF("no panel answered. SPI plane transfer still runs, waveform refresh %s",
         s_run_refresh ? "on" : "off");
  } else {
    cog_stop();
  }
  s_ready = true;
  if (!s_headless) {
    LOGF("ready, panel %s%s", epd_panel_name(),
         s_run_refresh ? "" : ", waveform refresh off");
  }
  return true;
}

bool refresh_display(epd_refresh_t mode)
{
  if (!s_ready) {
    LOGF("refresh_display() before a successful epd_begin()");
    return false;
  }
  // A verb or seq session holds its own table latched. Close it so this
  // refresh opens a session on the waveform it needs. cal reopens on demand.
  if (s_verb_open) {
    LOGF("closing the open verb session for the refresh");
    epd_verb_close();
  }
  s_refreshing = true;
  const uint32_t t_frame = now_ms();

  const bool have_red = frame_has_red();
  bool drive_fast;
  bool draw_red;
  refresh_choice(mode == EPD_FAST, have_red, &drive_fast, &draw_red);

  if (!s_run_refresh) {
    bool planes_ok = send_planes_only(t_frame, have_red, draw_red);
    s_refreshing = false;
    return planes_ok;
  }

  // Planes-only leaves the session open with no latched waveform. A real
  // panel has to start a fresh session, and must not ramp its booster off a
  // stand-in OTP. With nothing attached the stand-in stays: the fast
  // sequence still runs, and the BUSY cap in wait_busy() keeps it short.
  if (s_planes_only) {
    s_lut_latched = false;
    s_planes_only = false;
    if (!s_headless) {
      s_session_open = false;
      if (s_otp_standin) {
        s_otp_valid = false;
        s_otp_standin = false;
      }
    }
  }

  s_frame_has_red = draw_red;

  epd_refresh_t drive = drive_fast ? EPD_FAST : EPD_SLOW;
  if (mode == EPD_FAST && drive == EPD_SLOW) {
    LOGF("red on the panel, this fast refresh runs slow to clear it");
  }

  // Red stages when this frame draws red, or when the previous one did and
  // this fast frame still has to erase it. refresh_choice() clears draw_red
  // on a promoted pass, and the latch below stores that.
  const bool need_red_lut = draw_red || s_prev_has_red;

  // The panel latches one waveform per session and will not take a second one,
  // so anything that changes the waveform has to close the session first.
  if (s_session_open &&
      (drive != s_latched_mode ||
       (drive == EPD_FAST && !s_driver_b && need_red_lut != s_lut8_has_red) ||
      (drive == EPD_FAST && s_driver_b && s_lutb_dirty))) {
    VLOGF("the waveform changed, reopening the session");
    cog_close();
    s_session_open = false;
    s_lut_latched = false;
  }

  if (!s_session_open) {
    bus_bitbang();
    s_quick = EPD_QUICK_OPEN && s_driver_b && drive == EPD_FAST;
    if (cog_initial() != 0) {
      LOGF("cog_initial failed");
      cog_stop();
      s_refreshing = false;
      return false;
    }
    s_session_open = true;
    s_lut_latched = false;
    s_session_images = 0;
  } else {
    VLOGF("reusing the open session, image %u",
          (unsigned)s_session_images + 1u);
  }

  send_planes(have_red, draw_red);
  LOGF("planes on SPI, %u bytes x2 in %u ms",
       (unsigned)EPD_PLANE_BYTES, (unsigned)s_last_send_ms);
  LOGF("%s waveform armed", drive == EPD_FAST ? "fast" : "slow");

  // The first image of a session goes through the full sequence, which is what
  // latches the waveform. Later images only need the planes and a trigger:
  // rewriting 0xB0 inside a session does not re-latch, it drops the panel back
  // onto its own waveform.
  bod_hold_off();
  int rc = s_lut_latched ? retrigger(EPD_IDLE_WAIT_MS) : do_refresh(drive, need_red_lut);

  // Only driver B needs more than one pull. Its refresh runs for a fixed time
  // whatever the table asks for, so a single pass cannot carry enough dose to
  // clear the previous frame. On the 084 the duration follows the table and
  // repeating it only multiplies the time.
  if (rc == 0 && drive == EPD_FAST && s_driver_b) {
    // Each pull overwrites s_last_panel_ms. Sum them so the reported panel
    // time covers the whole refresh.
    uint32_t total = s_last_panel_ms;
    for (unsigned pull = 1; rc == 0 && pull < s_fast_pulls; pull++) {
      rc = retrigger(EPD_IDLE_WAIT_MS);
      total += s_last_panel_ms;
    }
    s_last_panel_ms = total;
  }

  s_latched_mode = drive;
  s_lut_latched = (rc == 0) && (drive == EPD_FAST);
  s_prev_has_red = draw_red;
  s_session_images++;

  const bool keep = s_keep_awake && s_lut_latched && (rc == 0) &&
                    (s_session_images < (uint32_t)EPD_SESSION_IMAGES);
  if (!keep) {
    s_session_open = false;
    s_lut_latched = false;
    if (rc == 0) cog_close();
    else         cog_stop();
  }
  bod_restore();

  s_last_frame_ms = now_ms() - t_frame;
  // The planes are already out. A missing panel that never releases BUSY is
  // not a failed picture: the waveform was requested and the wait was capped.
  const bool headless_gave_up = s_headless && rc != 0;
  LOGF("%s refresh %s, panel %u ms, send %u ms, frame %u ms, red %s%s",
       (drive == mode) ? (drive == EPD_FAST ? "fast" : "slow")
                       : "slow (promoted)",
       rc == 0 ? "done" : (headless_gave_up ? "no panel" : "FAILED"),
       (unsigned)s_last_panel_ms, (unsigned)s_last_send_ms,
       (unsigned)s_last_frame_ms,
       draw_red ? "drawn" : (have_red ? "as ink" : "none"),
       keep ? ", COG kept up" : "");
  s_refreshing = false;
  return rc == 0 || headless_gave_up;
}

static void verb_close(bool cut);

void epd_sleep(void)
{
  verb_close(true);
  if (s_session_open) {
    s_session_open = false;
    s_lut_latched = false;
    cog_stop();
  }
  supply_cut();
}

bool epd_clear_screen(void)
{
  if (s_verb_open) {
    LOGF("closing the open verb session for the clear");
    epd_verb_close();
  }
  LOGF("clearing the panel");
  epd_clear();
  return refresh_display(EPD_SLOW);
}

void epd_deep_sleep_prepare(void)
{
  epd_sleep();
  // Deep sleep lets every pad go high-Z. The MOSFET gate has no pull-up, so a
  // floating gate leaves the panel supply undefined for the whole sleep, and a
  // sleeping badge must not power its panel. GPIO48 is a digital pad, so the
  // hold needs gpio_deep_sleep_hold_en() as well.
  gpio_hold_en((gpio_num_t)EPD_PIN_PWR);
  gpio_deep_sleep_hold_en();
  LOGF("supply latched off, panel ready for deep sleep");
}

void epd_set_keep_awake(bool on) { s_keep_awake = on; }
bool epd_keep_awake(void)        { return s_keep_awake; }

void epd_set_run_refresh(bool on) { s_run_refresh = on; }
bool epd_run_refresh(void)        { return s_run_refresh; }
void epd_set_allow_fast_red(bool on) { s_allow_fast_red = on; }

esp_err_t epd_set_fast_lut(const uint8_t* table, size_t len)
{
  if (s_refreshing) return ESP_ERR_INVALID_STATE;
  if (table == NULL) {
    epd_build_lutb(s_lutb);
    s_lutb_custom = false;
  } else {
    if (len != LUTB_BYTES) return ESP_ERR_INVALID_SIZE;
    const int zero = epd_lutb_zero_count_at(table);
    if (zero >= 0) {
      LOGF("fast table rejected, zero count byte at offset %d (table %d group %d byte %d)",
           zero, zero / 80, (zero / 8) % 10, zero % 8);
      return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_lutb, table, LUTB_BYTES);
    s_lutb_custom = true;
  }
  // A latched session keeps the table it latched. Rewriting 0xB0 inside it
  // drops the panel back onto its own waveform, so the next fast refresh
  // reopens the session instead.
  s_lutb_dirty = true;
  return ESP_OK;
}

void epd_set_fast_pulls(unsigned n)
{
  if (n == 0u) n = (unsigned)EPD_B_REFRESH_PULLS;
  if (n > (unsigned)EPD_B_PULLS_MAX) n = (unsigned)EPD_B_PULLS_MAX;
  s_fast_pulls = n;
}
unsigned epd_fast_pulls(void) { return s_fast_pulls; }

// -----------------------------------------------------------------------------
// Verb pass
// -----------------------------------------------------------------------------
// A pass that sent red plane bits may have moved red pigment, so the panel is
// then treated as holding red.
static void verb_close(bool cut)
{
  if (!s_verb_open) return;
  bod_hold_off();
  cog_stop_ex(cut);
  bod_restore();
  if (s_verb_red) s_prev_has_red = true;
  s_verb_open = false;
  s_verb_unit = 0;
  s_verb_red = false;
  s_verb_table = EPD_VERB_TABLE_VERB;
  s_session_open = false;
  s_lut_latched = false;
  LOGF("verb session closed%s", s_supply_on ? ", supply kept on" : "");
}

void epd_verb_close(void) { verb_close(EPD_KEEP_SUPPLY == 0); }

esp_err_t epd_verb_open(uint8_t unit_frames, uint8_t vcom_level)
{
  return epd_verb_open_table(unit_frames, vcom_level, EPD_VERB_TABLE_VERB);
}

esp_err_t epd_verb_open_table(uint8_t unit_frames, uint8_t vcom_level,
                              epd_verb_table_t table)
{
  return epd_verb_open_ex(unit_frames, vcom_level, table, false);
}

// Preconditions every verb open shares.
static esp_err_t verb_check(uint8_t unit_frames)
{
  if (!s_ready || s_headless || s_refreshing) return ESP_ERR_INVALID_STATE;
  if (!s_driver_b || s_otp_standin) return ESP_ERR_NOT_SUPPORTED;
  if (unit_frames == 0u || unit_frames > EPD_VERB_UNIT_MAX) return ESP_ERR_INVALID_ARG;
  return ESP_OK;
}

static const char* verb_table_name(epd_verb_table_t table)
{
  switch (table) {
    case EPD_VERB_TABLE_SEQ:    return "seq";
    case EPD_VERB_TABLE_PHASE1: return "phase1";
    default:                    return "verb";
  }
}

esp_err_t epd_verb_open_ex(uint8_t unit_frames, uint8_t vcom_level,
                           epd_verb_table_t table, bool with_planes)
{
  const esp_err_t pre = verb_check(unit_frames);
  if (pre != ESP_OK) return pre;
  if (vcom_level > 3u) return ESP_ERR_INVALID_ARG;
  if (table != EPD_VERB_TABLE_VERB && table != EPD_VERB_TABLE_SEQ &&
      table != EPD_VERB_TABLE_PHASE1) {
    return ESP_ERR_INVALID_ARG;
  }

  uint32_t frames;
  if (table == EPD_VERB_TABLE_SEQ) {
    vcom_level = 0;
    if (unit_frames > EPD_SEQ_UNIT_MAX) return ESP_ERR_INVALID_ARG;
    epd_build_lutb_seq(s_verb_lut, unit_frames);
    frames = epd_lutb_seq_frames(s_verb_lut);
  } else if (table == EPD_VERB_TABLE_PHASE1) {
    vcom_level = 0;
    unit_frames = EPD_PHASE1_DRIVE;
    epd_build_lutb_phase1(s_verb_lut);
    frames = epd_lutb_phase1_frames(s_verb_lut);
    if (frames != (uint32_t)EPD_PHASE1_PULL_FRAMES) {
      LOGF("phase 1 table plays %u frames, the planner expects %u, not sent", (unsigned)frames,
           (unsigned)EPD_PHASE1_PULL_FRAMES);
      return ESP_ERR_INVALID_STATE;
    }
  } else {
    epd_build_lutb_verb(s_verb_lut, unit_frames, vcom_level);
    frames = epd_lutb_verb_frames(s_verb_lut);
  }
  if (frames == 0u) {
    LOGF("verb table has a zero or uneven count, not sent");
    return ESP_ERR_INVALID_STATE;
  }

  // The panel latches one table per session, so any open session ends here.
  if (s_verb_open) {
    LOGF("closing the open verb session to latch unit %u", (unsigned)unit_frames);
    epd_verb_close();
  } else if (s_session_open) {
    LOGF("closing the open %s session for the verb table",
         s_planes_only ? "planes-only" : "refresh");
    cog_close();
    s_session_open = false;
    s_lut_latched = false;
  }
  s_planes_only = false;

  s_refreshing = true;
  const uint32_t t0 = now_ms();
  bus_bitbang();
  s_quick = EPD_QUICK_OPEN && s_driver_b;
  if (cog_initial() != 0) {
    LOGF("verb open: cog_initial failed");
    cog_stop();
    s_refreshing = false;
    return ESP_FAIL;
  }

  // Both planes zero select code 0 for every pixel. That is hold on the verb
  // and seq sets and S on the phase 1 set, and nothing is driven before the
  // first pull. write_plane() sends the window registers ahead of the bw plane.
  // with_planes sends the caller's planes here instead, before the latch.
  write_plane(0x10, with_planes ? s_bw : NULL, NULL);
  write_plane(0x13, with_planes ? s_red : NULL, NULL);
  send_upd(UPD_STEPS);
  // The update registers overwrite 0xB0, and nothing after this touches it.
  send_index(0xB0, s_verb_lut, (uint16_t)LUTB_BYTES);

  bod_hold_off();
  dcdc_soft_start();
  delay_ms(100);
  if (!busy_idle() && wait_busy(1, 10000, "verb pre-0x15 ready") < 0) {
    LOGF("verb open: booster never reported ready in 10 s");
    cog_stop();
    bod_restore();
    s_refreshing = false;
    return ESP_ERR_TIMEOUT;
  }
  bod_restore();

  s_session_open = true;
  s_lut_latched = true;
  s_session_images = 0;
  s_verb_open = true;
  s_verb_unit = unit_frames;
  s_verb_red = with_planes && frame_has_red();
  s_verb_table = table;
  s_refreshing = false;
  LOGF("%s session open, unit %u frames, pull %u frames = %u ms at %u.%u ms per frame, open %u ms",
       verb_table_name(table),
       (unsigned)unit_frames, (unsigned)frames,
       (unsigned)((frames * EPD_B_FRAME_US) / 1000u),
       (unsigned)(EPD_B_FRAME_US / 1000u), (unsigned)((EPD_B_FRAME_US % 1000u) / 100u),
       (unsigned)(now_ms() - t0));
  if (table == EPD_VERB_TABLE_SEQ) {
    LOGF("seq tables: 0 hold, 1 paper VDL, 2 ink VDH, %u driven of %u frames. "
         "3 w: %u VDHR then ground. One frame a group, phase C only",
         (unsigned)unit_frames, (unsigned)EPD_SEQ_PULL_FRAMES, (unsigned)EPD_SEQ_W_FRAMES);
    return ESP_OK;
  }
  if (table == EPD_VERB_TABLE_PHASE1) {
    LOGF("phase 1 tables: 0 S shake 6 VDH 6 VDL, 1 I %u VDH, 2 P %u VDL, 3 M 8 VDL 4 VDH, "
         "no hold, slotted, %u frames a pull",
         (unsigned)EPD_PHASE1_DRIVE, (unsigned)EPD_PHASE1_DRIVE, (unsigned)EPD_PHASE1_PULL_FRAMES);
    return ESP_OK;
  }
  // Net field per verb, source minus VCOM, as epd_build_lutb_verb() sets it.
  static const char* const k_rail[4] = { "GND", "VDH", "VDL", "VDHR" };
  const char* vc = k_rail[vcom_level];
  if (vcom_level == 0u) {
    LOGF("vcom GND: hold 0, push VDH, pull VDL, red step VDHR");
  } else {
    LOGF("vcom %s: hold 0, push VDH-%s, pull VDL-%s, weak pull -%s", vc, vc, vc, vc);
  }
  return ESP_OK;
}

esp_err_t epd_verb_pass(unsigned pulls, uint32_t *panel_ms_total)
{
  return epd_verb_pull(pulls, true, panel_ms_total);
}

esp_err_t epd_verb_pull(unsigned pulls, bool resend, uint32_t *panel_ms_total)
{
  if (panel_ms_total) *panel_ms_total = 0;
  if (!s_verb_open || s_refreshing) return ESP_ERR_INVALID_STATE;
  if (pulls == 0u || pulls > EPD_VERB_PULLS_MAX) return ESP_ERR_INVALID_ARG;

  s_refreshing = true;
  const uint32_t t_frame = now_ms();
  bus_bitbang();
  if (resend) {
    if (frame_has_red()) s_verb_red = true;
    // Both planes go out unmerged: draw_red sends the red plane as it is.
    send_planes(false, true);
  } else {
    s_last_send_ms = 0;
  }

  // Triggers only, as a kept session does after its first image. 0xB0 is not
  // written again: a rewrite inside a session drops the panel onto its own
  // waveform.
  bod_hold_off();
  uint32_t total = 0;
  unsigned done = 0;
  int rc = 0;
  for (unsigned i = 0; i < pulls; i++) {
    rc = retrigger(EPD_VERB_BUSY_MS);
    if (rc != 0) break;
    total += s_last_panel_ms;
    done++;
  }
  s_last_panel_ms = total;
  if (panel_ms_total) *panel_ms_total = total;
  s_session_images++;

  if (rc != 0) {
    LOGF("verb pull %u of %u did not finish, closing the session",
         done + 1u, pulls);
    verb_close(true);
  }
  bod_restore();

  s_last_frame_ms = now_ms() - t_frame;
  s_refreshing = false;
  LOGF("verb pass %u of %u pulls, panel %u ms, send %u ms, frame %u ms",
       done, pulls, (unsigned)total, (unsigned)s_last_send_ms,
       (unsigned)s_last_frame_ms);
  return rc == 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool    epd_verb_is_open(void) { return s_verb_open; }
uint8_t epd_verb_unit(void)    { return s_verb_unit; }
epd_verb_table_t epd_verb_table(void) { return s_verb_open ? s_verb_table : EPD_VERB_TABLE_VERB; }

// The variant's own read. Driver 8 gets its unlock and address writes on both
// COGs here, where the probe sends them to the left COG only.
size_t epd_otp_dump(uint8_t* dst, size_t cap)
{
  epd_otp_probe_t p = { 0 };
  if (s_driver_b) {
    p.opener = 0xB9;
  } else {
    p.set_addr = true;
    p.addr = 0x0040;
    p.opener = 0xA8;
    p.skip_0x70 = true;
  }
  return epd_otp_dump_ex(&p, dst, cap);
}

size_t epd_otp_dump_ex(const epd_otp_probe_t* probe, uint8_t* dst, size_t cap)
{
  if (!s_ready || s_headless || s_refreshing || !s_otp_valid || s_otp_standin) return 0;
  if (probe == NULL || dst == NULL || cap == 0u) return 0;
  if (cap > (size_t)EPD_OTP_DUMP_MAX) cap = (size_t)EPD_OTP_DUMP_MAX;

  // The read needs a fresh power-up and reset, so an open session ends here.
  verb_close(true);
  if (s_session_open) {
    s_session_open = false;
    s_lut_latched = false;
    cog_stop();
  }
  supply_cut();
  bus_bitbang();
  otp_read_probe(probe, dst, cap);
  cog_stop();
  LOGF("OTP dump, %u bytes from the left COG, opener 0x%02X",
       (unsigned)cap, (unsigned)probe->opener);
  return cap;
}

// Same power-up, reset and read as otp_read_probe(), with the register index
// as the opener. Never sends 0x15 and never ramps the booster.
size_t epd_reg_read(const epd_reg_probe_t* probe, uint8_t* dst, size_t cap)
{
  if (!s_ready || s_headless || s_refreshing || !s_otp_valid || s_otp_standin) return 0;
  if (probe == NULL || dst == NULL || cap == 0u) return 0;
  if (cap > (size_t)EPD_OTP_DUMP_MAX) cap = (size_t)EPD_OTP_DUMP_MAX;

  verb_close(true);
  if (s_session_open) {
    s_session_open = false;
    s_lut_latched = false;
    cog_stop();
  }
  supply_cut();
  bus_bitbang();
  cog_select(COG_M);
  cog_reset();

  cog_select(COG_BOTH);
  if (!probe->skip_0x70) send_u8x2(0x70, 0x71, 0x36);
  // The guard above requires the cached block, so this sends nothing.
  if (otp_read() != 0) {
    cog_stop();
    return 0;
  }
  cog_select(COG_BOTH);
  if (probe->after_update) {
    send_upd(UPD_PRE_GEOM);
    // The fast refresh's 0xB0 write from do_refresh(), to both COGs.
    if (probe->write_lut) send_index(0xB0, s_lutb, (uint16_t)LUTB_BYTES);
  } else if (probe->pre_44_06) {
    send_u8(0x44, 0x06);
  }

  cog_select(COG_M);
  dc_set(0);
  cs_lo();
  bb_out_slow(probe->index);
  otp_read_tail(dst, cap, !probe->no_dummy);
  cog_stop();
  LOGF("register 0x%02X read, %u bytes from the left COG%s%s",
       (unsigned)probe->index, (unsigned)cap,
       probe->after_update ? " after the update registers" : "",
       (probe->after_update && probe->write_lut) ? " and 0xB0" : "");
  return cap;
}

const uint8_t* epd_otp_block(size_t* len)
{
  const bool have = s_otp_valid && !s_otp_standin;
  if (len) *len = have ? (size_t)EPD_OTP_LEN : 0u;
  return have ? s_otp : NULL;
}

bool epd_headless(void)           { return s_headless; }
bool epd_refreshing(void)         { return s_refreshing; }

epd_panel_t epd_panel(void)      { return s_panel; }
const char* epd_panel_name(void)
{
  switch (s_panel) {
    case EPD_PANEL_084: return "E2969JS084 (driver 8)";
    case EPD_PANEL_B1:  return "E2969JS0B1 (driver B)";
    default:            return "unknown";
  }
}
uint32_t epd_last_panel_ms(void) { return s_last_panel_ms; }
uint32_t epd_last_frame_ms(void) { return s_last_frame_ms; }
uint32_t epd_last_send_ms(void)  { return s_last_send_ms; }
int64_t  epd_last_send_end_us(void) { return s_last_send_end_us; }

uint8_t* epd_plane_bw(void)  { return s_bw; }
uint8_t* epd_plane_red(void) { return s_red; }
