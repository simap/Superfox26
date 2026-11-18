// -----------------------------------------------------------------------------
// epd97: Pervasive Displays iTC 9.7" dual-COG panel on an ESP32-S3, ESP-IDF.
//
// Written for this project, with reference to these sources:
//   Hack-a-Day/2026-Billboard_Badge
//     (https://github.com/Hack-a-Day/2026-Billboard_Badge), the badge team's
//     ITC97 driver, firmware/esp32s3-itc97-drag-drop/src/epd97.cpp at commit
//     6ba6dd8. The bus, power, OTP detection, update register, soft start and
//     brownout code of this file come from it, much of it unchanged. Its
//     header names the OpenEPaperLink EFR32FG22 driver for this panel as its
//     own origin.
//   The badge's stock MicroPython driver in the same repository,
//     firmware/badge/lib/epd.py, itself ported from PervasiveDisplays
//     EPD_Driver_GU_large. It gives the 0x70 booster setting of driver B and
//     the plane resend that keeps a 0xB0 table latched.
//   yigitabi5444/supercon26-badge-os
//     (https://github.com/yigitabi5444/supercon26-badge-os),
//     components/os/epd.c and docs/DEVICE_INFO.md. It gives the quick open
//     waits, the 0x03 oscillator value for 23.7 ms frames, the kept supply
//     between sessions and the one frame group of the seq set.
//   phthallo/supercon (https://github.com/phthallo/supercon),
//     firmware/lib/luts.py. It shows which zero count bytes the B1 skips.
// The comments name the source where a value or a behaviour comes from one.
//
// The panel is two controllers behind one FPC, each owning half of a 960 px
// scan line. The left controller takes the left 60 bytes of every line and the
// right controller the right 60. Register writes go to both at once.
//
// The register writes and the OTP readback are bitbanged. They are a few dozen
// bytes, and the OTP wants a ~50 kHz clock with the data pin turned around for
// the read. The two 80640 byte planes go out over SPI2 with DMA. The two share
// MOSI and SCK, so the GPIO matrix is re-pointed between them and the bus is
// never torn down.
// -----------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
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

#if EPD_LOG
#define LOGF(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define LOGF(...) do { if (0) ESP_LOGI(TAG, __VA_ARGS__); } while (0)
#endif
#if EPD_LOG
#define DLOGF(...) ESP_LOGD(TAG, __VA_ARGS__)
#else
#define DLOGF(...) do { if (0) ESP_LOGD(TAG, __VA_ARGS__); } while (0)
#endif
#if EPD_LOG_VERBOSE
#define VLOGF(...) LOGF(__VA_ARGS__)
#else
#define VLOGF(...) do { if (0) ESP_LOGI(TAG, __VA_ARGS__); } while (0)
#endif

#if EPD_T_PRE_SS % 10u != 0u || EPD_T_PRE_SS / 10u > 127u
#error "EPD_T_PRE_SS is stored in units of 10 ms in a 7 bit field"
#endif

#define EPD_OTP_LEN   88
#define COG_L         0x01
#define COG_R         0x02
#define COG_BOTH      (COG_L | COG_R)
#define HALF_BYTES    (EPD_STRIDE / 2)        // 60
#define HALF_PLANE    (HALF_BYTES * EPD_H)    // 40320, one controller's share of a plane

typedef enum {
  EPD_PANEL_UNKNOWN = 0,
  EPD_PANEL_084,          // E2969JS084, driver 8
  EPD_PANEL_B1            // E2969JS0B1, driver B
} epd_panel_t;

// The vendor's update register sequence for the 9.7", TCON before STV. Each
// step is a register, its argument, a delay field and the quick profile delay.
// In the delay field bit 0 says the argument is an OTP offset rather than a
// literal, and the upper bits are the wait in units of 10 ms. qms is the wait
// in ms when s_quick is set (EPD_QUICK_OPEN in epd97_config.h).
typedef struct { uint8_t cmd; uint8_t arg; uint8_t dly; uint8_t qms; } upd_step_t;

static const upd_step_t k_upd[] = {
  { 0x05, 0x7D, 20u << 1, 50 },
  { 0x05, 0x00,  1u << 1,  1 },
  { 0xC2, 0x3F,  0,        0 },
  { 0xD8, 0x1D,  1,        0 },
  { 0xD6, 0x1E,  1,        0 },
  { 0xA7, 0x10, 10u << 1,  2 },
  { 0xA7, 0x00, 10u << 1, 10 },   // the oscillator write follows this step
  { 0x44, 0x00,  0,        0 },
  { 0x45, 0x80,  0,        0 },
  { 0xA7, 0x10, 10u << 1,  2 },
  { 0xA7, 0x00, 10u << 1, 10 },
  { 0x44, 0x06,  0,        0 },
  { 0x45, 0x86,  0,        0 },              // patched at runtime with the waveform bank
  { 0xA7, 0x10, 10u << 1,  2 },
  { 0xA7, 0x00, (EPD_T_PRE_SS / 10u) << 1, 10 },
  { 0x60, 0x0B,  1,        0 },
  { 0x61, 0x1C,  1,        0 },
  { 0x01, 0x10,  1,        0 },
  { 0x02, 0x11,  1,        0 },
};
#define UPD_STEPS (sizeof(k_upd) / sizeof(k_upd[0]))
// Index of the k_upd step after which the oscillator register is written.
#define UPD_OSC_AFTER 6u

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
// is still plugged into the same board, so reading it again on every wake costs
// a full power cycle and about three seconds for an answer that cannot have
// changed. It is only trusted after a deep-sleep wake, so unplugging the panel
// and power-cycling the board reads it again.
#define OTP_RTC_MAGIC 0x69544301u
RTC_DATA_ATTR static uint32_t s_rtc_magic;
RTC_DATA_ATTR static uint8_t  s_rtc_otp[EPD_OTP_LEN];
RTC_DATA_ATTR static uint8_t  s_rtc_driver_b;

static volatile bool s_refreshing;
// No controller answered at epd_begin(). The OTP is a stand-in and the BUSY
// waits are capped.
static bool      s_headless;

static uint8_t   s_tone_lut[LUTB_BYTES];
static bool      s_tone_open;
static uint8_t   s_tone_unit;
static epd_tone_table_t s_tone_table = EPD_TONE_TABLE_NONE;

static uint32_t  s_power_off_ms;
static bool      s_power_off_valid;
// The panel supply switch is on. With EPD_KEEP_SUPPLY it stays on between
// sessions, with the controller held in reset.
static bool      s_supply_on;
static uint32_t  s_last_panel_ms;
static bool      s_quick;

// Internal DMA memory, one controller's half of a plane each. The planes live
// in PSRAM, and a controller's half is 60 bytes in every 120 byte line, so each
// half is gathered here and goes out as one transfer. s_stage[1] is NULL when
// the heap had room for only one.
static uint8_t*  s_stage[2];

// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// The tick waits assume CONFIG_FREERTOS_HZ 1000. At 100 Hz pdMS_TO_TICKS
// truncates to 10 ms steps.
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
  gpio_set_level((gpio_num_t)EPD_PIN_CS_L, 1);
  gpio_set_level((gpio_num_t)EPD_PIN_CS_R, 1);
}

// Raise the deselected side before lowering the selected one, so the two
// controllers are never both listening by accident.
static void cs_lo(void)
{
  if (!(s_cog & COG_L)) gpio_set_level((gpio_num_t)EPD_PIN_CS_L, 1);
  if (!(s_cog & COG_R)) gpio_set_level((gpio_num_t)EPD_PIN_CS_R, 1);
  if (s_cog & COG_L)    gpio_set_level((gpio_num_t)EPD_PIN_CS_L, 0);
  if (s_cog & COG_R)    gpio_set_level((gpio_num_t)EPD_PIN_CS_R, 0);
}

// Both controllers have to report ready. A panel with only the left
// controller's BUSY wired can set EPD_PIN_BUSY_R to -1.
static bool busy_idle(void)
{
  if (!gpio_get_level((gpio_num_t)EPD_PIN_BUSY_L)) return false;
#if EPD_PIN_BUSY_R >= 0
  if (!gpio_get_level((gpio_num_t)EPD_PIN_BUSY_R)) return false;
#endif
  return true;
}

// -----------------------------------------------------------------------------
// Bus: bitbang and hardware SPI over the same two pins
// -----------------------------------------------------------------------------
// Only the two bus pins change hands. DC, RST and the two chip selects are
// configured as outputs once in epd_begin() and stay driven while the panel
// supply is off. Releasing them with the supply off stops the refresh at the
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

// SPI mode 0 by hand. The panel samples on the rising edge and the clock idles
// low.
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
  // the controller sits on the update waiting for its own voltages. It shows up
  // mostly on the second refresh.
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
// The opener left the left controller's chip select low. This read turns MOSI
// around and drops the leading dummy byte the controller sends before the OTP.
static void otp_read_tail(uint8_t* dst, size_t n)
{
  delay_ms(1);

  dc_set(1);
  gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)EPD_PIN_MOSI, GPIO_PULLDOWN_ONLY);
  (void)bb_in_slow();
  for (size_t i = 0; i < n; i++) dst[i] = bb_in_slow();
  cs_hi();
  gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
  gpio_set_pull_mode((gpio_num_t)EPD_PIN_MOSI, GPIO_FLOATING);
  sdi_set(0);
  cog_select(COG_BOTH);
}

static void otp_read_raw(bool use_b9, uint8_t* dst, size_t n)
{
  cog_select(COG_L);
  cog_reset();

  if (use_b9) {
    // Driver B opens the session with 0x70 before the OTP read. A panel that
    // never gets that register draws about 2 A during refresh, so it is
    // configuring the booster. It goes to both controllers. Only the read
    // itself is left controller only, because both would otherwise drive the
    // bus.
    cog_select(COG_BOTH);
    send_u8x2(0x70, 0x71, 0x36);
    cog_select(COG_L);
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
  otp_read_tail(dst, n);
}

// Stage 0 of the DC/DC ramp must not be 0xFF. That ramps the booster 127x and
// hangs BUSY. All four stages are checked, because the booster loop runs the
// OTP repeat counts verbatim. A garbled block has to be rejected here and not
// clamped later.
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
  // Both variants are checked in the driver 8 layout (driver B is remapped into
  // it first). DUW sits at 0x16..0x1B and DRFW at 0x0C..0x0F, and for a 960 px
  // two-controller panel both carry x_end = 0x3B, 60 bytes per half.
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

// Moves a driver B block into the driver 8 layout the rest of the driver reads.
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

// The OTP is a fuse block and cannot change between refreshes. Reading it
// costs about 2.8 s, because it is clocked out at 50 kHz and tried twice, once
// per variant. So it is read once and kept.
static int otp_read(void)
{
  if (s_otp_valid) {
    VLOGF("OTP from cache (%s)",
          s_headless ? "stand-in" : (s_driver_b ? "driver B" : "driver 8"));
    return 0;
  }

  // Driver B first, then driver 8. That is the order the stock firmware uses.
  otp_read_raw(true, s_otp, EPD_OTP_LEN);
  if (otp_looks_like_driver_b()) {
    otp_remap_b_to_8();
    s_driver_b = true;
    if (!otp_sane()) {
      LOGF("driver B remap failed the soft start sanity check");
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
    LOGF("driver 8 OTP read unusable (hdr=0x%02X)", (unsigned)s_otp[0]);
    return -2;
  }
  s_panel = EPD_PANEL_084;
  s_otp_valid = true;
  LOGF("E2969JS084 (driver 8) detected");
  otp_stash();
  return 0;
}

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
  // profile uses the reset shape of yigitabi5444/supercon26-badge-os.
  // cog_close() leaves RST low on a kept supply, and the pulse starts from
  // high.
  if (warm) rst_set(1);
  delay_ms(s_quick ? EPD_TQ_RST_HI : EPD_T_OTP_HI);
  rst_set(0);
  delay_ms(s_quick ? EPD_TQ_RST_LO : EPD_T_OTP_LO);
  rst_set(1);
  delay_ms(s_quick ? EPD_TQ_RST_SET : EPD_T_OTP_SET);
  // 0x70 configures the driver B booster. Without it the panel draws about
  // 2 A during the refresh. A supply cut clears it, so driver B gets it after
  // the last reset of every open, also on a kept supply. It has to precede
  // the window registers in write_plane().
  if (s_driver_b) {
    cog_select(COG_BOTH);
    send_u8x2(0x70, 0x71, 0x36);
  }
  return 0;
}

// -----------------------------------------------------------------------------
// Plane transfer
// -----------------------------------------------------------------------------
static void stage_half(uint8_t* dst, const uint8_t* fb, size_t half)
{
  for (int y = 0; y < EPD_H; y++) {
    const size_t off = (size_t)y * EPD_STRIDE + half;
    uint8_t* d = dst + (size_t)y * HALF_BYTES;
    if (fb) memcpy(d, fb + off, HALF_BYTES);
    else    memset(d, 0, HALF_BYTES);
  }
}

// The S3 GPSPI takes at most SPI_LL_DATA_MAX_BIT_LEN bits (32768 bytes) per
// transaction, so a 40320 byte half goes out as two chunks. The controller
// keeps its RAM pointer across chip select, so the chunks and the chip select
// swap per plane are invisible to it.
#define SPI_CHUNK (HALF_PLANE / 2)   // 20160

static bool send_chunks(const uint8_t* buf, size_t len)
{
  while (len) {
    const size_t n = len > SPI_CHUNK ? SPI_CHUNK : len;
    spi_transaction_t t = { .length = n * 8u, .tx_buffer = buf };
    esp_err_t e = spi_device_polling_transmit(s_spi, &t);
    if (e != ESP_OK) {
      LOGF("spi transmit failed: %s", esp_err_to_name(e));
      return false;
    }
    buf += n;
    len -= n;
  }
  return true;
}

static bool send_plane_data(const uint8_t* fb)
{
  uint8_t* a = s_stage[0];
  uint8_t* b = s_stage[1] ? s_stage[1] : s_stage[0];
  bool ok;

  stage_half(a, fb, 0);
  cog_select(COG_L);
  cs_lo();
  if (b != a) {
    spi_transaction_t t = { .length = SPI_CHUNK * 8u, .tx_buffer = a };
    esp_err_t e = spi_device_polling_start(s_spi, &t, portMAX_DELAY);
    stage_half(b, fb, HALF_BYTES);
    if (e == ESP_OK) {
      ok = spi_device_polling_end(s_spi, portMAX_DELAY) == ESP_OK;
    } else {
      LOGF("spi start failed: %s", esp_err_to_name(e));
      ok = send_chunks(a, SPI_CHUNK);
    }
    ok = ok && send_chunks(a + SPI_CHUNK, HALF_PLANE - SPI_CHUNK);
  } else {
    ok = send_chunks(a, HALF_PLANE);
    stage_half(b, fb, HALF_BYTES);
  }
  if (!ok) return false;

  cog_select(COG_R);
  cs_lo();
  return send_chunks(b, HALF_PLANE);
}

// The bw plane goes out with data command 0x10, the red plane with 0x11. The
// window registers come out of the OTP and have to precede the first plane.
// The controller wants both planes either way.
static bool write_plane(bool red, const uint8_t* fb)
{
  uint8_t cmd;
  if (!red) {
    // Driver B sets DCTL (0x01) before the window registers, as the stock
    // firmware does. Driver 8 sends 0x01 at the end of the update sequence. A
    // resend inside an open tone session leaves DCTL alone. The stock plane
    // resend (epd.py, _send_images) writes only DUW, DRFW, RAM_RW and data, and
    // it is the only resend seen to keep a 0xB0 table latched.
    if (s_driver_b && !s_tone_open) send_u8(0x01, s_otp[0x10]);
    send_otp(0x13, 0x16, 6);   // DUW
    send_otp(0x90, 0x0C, 4);   // DRFW
    send_otp(0x12, 0x13, 3);   // RAM_RW
    cmd = 0x10;
  } else {
    send_otp(0x12, 0x13, 3);   // RAM_RW
    cmd = 0x11;
  }

  dc_set(0);
  cs_lo();
  bb_byte(cmd);
  cs_hi();
  dc_set(1);

  bus_spi();
  const bool ok = send_plane_data(fb);
  bus_bitbang();
  cs_hi();
  cog_select(COG_BOTH);
  return ok;
}

static bool send_planes(const uint8_t* bw, const uint8_t* red, uint32_t* ms)
{
  const int64_t t0 = esp_timer_get_time();
  const bool ok = write_plane(false, bw) && write_plane(true, red);
  if (ms) *ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
  return ok;
}

// -----------------------------------------------------------------------------
// Refresh
// -----------------------------------------------------------------------------
static void stage_delay(uint8_t d)
{
  if (d & 0x80) delay_ms((d & 0x7F) ? (d & 0x7F) : 1);
  else          esp_rom_delay_us(10u * (uint32_t)(d & 0x7F));
}

// The vendor loop as it is. The repeat count is whatever the OTP says, with
// no skipping and no cap. Ramping the booster differently is what stresses a
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

// The panel's own waveform holds the panel rails up for about half a minute.
// On this board that dips VDD far enough to fire the brownout detector, which
// reboots the chip. The detector is held off for the whole waveform, including
// the gaps between BUSY waits, and for every tone pull.
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
  // No controller answered at epd_begin(). A floating BUSY line must not hold
  // the caller for the full waveform timeout.
  if (s_headless && timeout_ms > 2000u) timeout_ms = 2000u;
#if CONFIG_ESP_TASK_WDT_INIT
  const bool feed_wdt = (esp_task_wdt_status(NULL) == ESP_OK);
#else
  const bool feed_wdt = false;
#endif
  uint32_t waited = 0;
  DLOGF("BUSY '%s' wait up to %us (want_idle=%u, pin=%u)",
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

// One pull. It triggers the update again without touching the registers the
// waveform lives in. The planes, the drive table and the booster rails are
// already loaded.
static int retrigger(uint32_t idle_ms)
{
  send_u8(0x15, 0x3C);
  if (wait_busy(0, 2000, "post-0x15 busy") < 0) return -2;
  int waited = wait_busy(1, idle_ms, "refresh done");
  if (waited < 0) return -3;
  s_last_panel_ms = (uint32_t)waited;
  return 0;
}

static void send_upd(void)
{
  const uint8_t reg45 = s_driver_b ? (uint8_t)EPD_REG45_B1 : (uint8_t)EPD_REG45_084;

  delay_ms(s_quick ? EPD_TQ_PRE_UPD : EPD_T_PRE_UPD);

  for (size_t i = 0; i < UPD_STEPS; i++) {
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
    // Driver B gets the value of yigitabi5444/supercon26-badge-os in the quick
    // profile, which gives 23.7 ms frames instead of about 29 ms. The push
    // limits depend on it (epd97_config.h).
    if (i == UPD_OSC_AFTER) {
      if (!s_driver_b)  send_u8x2(0x03, 0x00, s_otp[0x12]);
      else if (s_quick) send_u8x2(0x03, 0x00, 0x11);
    }
  }
}

static int do_refresh(void)
{
  send_upd();
  dcdc_soft_start();

  // Stock fires 0x15 about 100 ms after the last soft start write. The 084 is
  // ready by then. The B1 still holds BUSY low because its DC/DC ramp is not
  // finished, and starting the update into a booster that is still ramping
  // makes the panel sit still for a minute before it draws.
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

  // Watch a little longer. Some panels drop BUSY again and run the waveform a
  // second time, which looks like a refresh that sits still and then draws.
  // A second run that does not finish fails the refresh.
  for (uint32_t i = 0; i < EPD_SECOND_PASS_MS / 10u; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!busy_idle()) {
      int again = wait_busy(1, EPD_IDLE_WAIT_MS, "second pass");
      if (again < 0) return -3;
      waited += again;
      break;
    }
  }
  s_last_panel_ms = (uint32_t)waited;
  return 0;
}

// The DC/DC soft shutdown. cut true then cuts the supply. cut false leaves
// the supply on and the controller in reset with RST low, as the stock firmware and
// yigitabi5444/supercon26-badge-os close a session. A BUSY that stays low past
// EPD_T_SHUTDOWN_BUSY turns a kept supply into a cut.
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

static void cog_stop(void) { cog_stop_ex(true); }

static void cog_close(void) { cog_stop_ex(EPD_KEEP_SUPPLY == 0); }

// Cuts a supply that cog_close() left on. The controller is already shut down
// and held in reset, so only the supply cut and the discharge remain.
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

// Enough of an OTP block for write_plane() to emit the window registers a
// 960 wide two-controller panel expects, for a board with no panel. It is not a
// captured OTP. The booster repeat counts are zero, so dcdc_soft_start() sends
// nothing.
static void otp_standin(void)
{
  memset(s_otp, 0, sizeof s_otp);
  s_otp[0x00] = 0xA5;
  s_otp[0x0D] = 0x3B;   // DRFW x_end, 60 bytes per half
  s_otp[0x17] = 0x3B;   // DUW x_end
  s_driver_b = false;
  s_panel = EPD_PANEL_UNKNOWN;
  s_otp_valid = true;
}

// -----------------------------------------------------------------------------
// Public
// -----------------------------------------------------------------------------
bool epd_begin(void)
{
  ESP_LOGI(TAG, "9.7\" iTC, %ux%u, planes %u bytes each",
           (unsigned)EPD_W, (unsigned)EPD_H, (unsigned)EPD_PLANE_BYTES);

  s_bw  = (uint8_t*)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  s_red = (uint8_t*)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_bw || !s_red) {
    // Without PSRAM the two planes take 161 kB of internal RAM. That is tight
    // but possible.
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

  // Park the supply switch before the pin becomes an output. The OUT register
  // is written while it is still high-Z, so the first level the gate ever sees
  // driven is the off one. Without this the pin leaves gpio_config at 0, and
  // on a P-channel high-side switch 0 is on.
  gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 0 : 1);

  const uint64_t out_mask =
      (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_PWR) |
      (1ULL << EPD_PIN_CS_L) | (1ULL << EPD_PIN_CS_R);
  gpio_config_t oc = { .pin_bit_mask = out_mask, .mode = GPIO_MODE_OUTPUT };
  gpio_config(&oc);

  // Release the deep-sleep latch only now, with the pad already configured and
  // the OUT register already holding the off level, so the level never lapses
  // between the latch letting go and this driving it.
  gpio_hold_dis((gpio_num_t)EPD_PIN_PWR);
  gpio_deep_sleep_hold_dis();

  uint64_t in_mask = (1ULL << EPD_PIN_BUSY_L);
#if EPD_PIN_BUSY_R >= 0
  in_mask |= (1ULL << EPD_PIN_BUSY_R);
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

  // One transaction carries one controller's half of a plane.
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

  if (otp_restore()) {
    LOGF("panel from RTC memory, detection skipped");
  } else if (cog_initial() != 0) {
    cog_stop();
    otp_standin();
    s_headless = true;
    LOGF("no panel answered. Refreshes send the planes and give up at the first BUSY wait");
  } else {
    cog_stop();
  }
  s_ready = true;
  if (!s_headless) LOGF("ready, panel %s", epd_panel_name());
  return true;
}

static void tone_close(bool cut);

bool refresh_display(epd_refresh_t mode)
{
  (void)mode;   // EPD_SLOW is the only refresh
  if (!s_ready) {
    LOGF("refresh_display() before a successful epd_begin()");
    return false;
  }
  // A tone session holds its own table latched. Close it so this refresh
  // opens a session on the panel's own waveform.
  if (s_tone_open) {
    LOGF("closing the open tone session for the refresh");
    tone_close(EPD_KEEP_SUPPLY == 0);
  }
  s_refreshing = true;
  const uint32_t t_frame = now_ms();

  bus_bitbang();
  s_quick = false;
  if (cog_initial() != 0) {
    LOGF("cog_initial failed");
    cog_stop();
    s_refreshing = false;
    return false;
  }

  uint32_t send_ms = 0;
  if (!send_planes(s_bw, s_red, &send_ms)) {
    LOGF("plane send failed, refresh not started");
    cog_stop();
    s_refreshing = false;
    return false;
  }
  LOGF("planes on SPI, %u bytes x2 in %u ms", (unsigned)EPD_PLANE_BYTES, (unsigned)send_ms);

  bod_hold_off();
  const int rc = do_refresh();
  if (rc == 0) cog_close();
  else         cog_stop();
  bod_restore();

  // A missing panel that never releases BUSY is not a failed picture. The
  // planes are out, the waveform was requested and the wait was capped.
  const bool headless_gave_up = s_headless && rc != 0;
  LOGF("slow refresh %s, panel %u ms, send %u ms, frame %u ms",
       rc == 0 ? "done" : (headless_gave_up ? "no panel" : "FAILED"),
       (unsigned)s_last_panel_ms, (unsigned)send_ms, (unsigned)(now_ms() - t_frame));
  s_refreshing = false;
  return rc == 0 || headless_gave_up;
}

void epd_sleep(void)
{
  tone_close(true);
  supply_cut();
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

// -----------------------------------------------------------------------------
// Tone session
// -----------------------------------------------------------------------------
static void tone_close(bool cut)
{
  if (!s_tone_open) return;
  bod_hold_off();
  cog_stop_ex(cut);
  bod_restore();
  s_tone_open = false;
  s_tone_unit = 0;
  s_tone_table = EPD_TONE_TABLE_NONE;
  LOGF("tone session closed%s", s_supply_on ? ", supply kept on" : "");
}

esp_err_t epd_tone_open_table(uint8_t unit_frames, uint8_t vcom_level,
                              epd_tone_table_t table)
{
  if (!s_ready || s_headless || s_refreshing) return ESP_ERR_INVALID_STATE;
  if (!s_driver_b) return ESP_ERR_NOT_SUPPORTED;
  if (vcom_level > 3u) return ESP_ERR_INVALID_ARG;

  uint32_t frames;
  const char* name;
  if (table == EPD_TONE_TABLE_SEQ) {
    if (unit_frames == 0u || unit_frames > EPD_SEQ_UNIT_MAX) return ESP_ERR_INVALID_ARG;
    epd_build_lutb_seq(s_tone_lut, unit_frames);
    frames = epd_lutb_seq_frames(s_tone_lut);
    name = "seq";
  } else if (table == EPD_TONE_TABLE_PHASE1) {
    unit_frames = EPD_PHASE1_DRIVE;
    epd_build_lutb_phase1(s_tone_lut);
    frames = epd_lutb_phase1_frames(s_tone_lut);
    if (frames != (uint32_t)EPD_PHASE1_PULL_FRAMES) {
      LOGF("phase 1 table plays %u frames, the planner expects %u, not sent", (unsigned)frames,
           (unsigned)EPD_PHASE1_PULL_FRAMES);
      return ESP_ERR_INVALID_STATE;
    }
    name = "phase1";
  } else {
    return ESP_ERR_INVALID_ARG;
  }
  if (frames == 0u) {
    LOGF("tone table has a zero or uneven count, not sent");
    return ESP_ERR_INVALID_STATE;
  }

  // The panel latches one table per session, so an open session ends here.
  if (s_tone_open) {
    LOGF("closing the open tone session to latch unit %u", (unsigned)unit_frames);
    tone_close(EPD_KEEP_SUPPLY == 0);
  }

  s_refreshing = true;
  const uint32_t t0 = now_ms();
  bus_bitbang();
  s_quick = EPD_QUICK_OPEN && s_driver_b;
  if (cog_initial() != 0) {
    LOGF("tone open: cog_initial failed");
    cog_stop();
    s_refreshing = false;
    return ESP_FAIL;
  }

  // Both planes zero select code 0 for every pixel, and nothing is driven
  // before the first pull. write_plane() sends the window registers ahead of
  // the bw plane.
  if (!send_planes(NULL, NULL, NULL)) {
    LOGF("tone open: plane send failed");
    cog_stop();
    s_refreshing = false;
    return ESP_FAIL;
  }
  send_upd();
  // The update registers overwrite 0xB0, and nothing after this touches it.
  send_index(0xB0, s_tone_lut, (uint16_t)LUTB_BYTES);

  bod_hold_off();
  dcdc_soft_start();
  delay_ms(100);
  if (!busy_idle() && wait_busy(1, 10000, "tone pre-0x15 ready") < 0) {
    LOGF("tone open: booster never reported ready in 10 s");
    cog_stop();
    bod_restore();
    s_refreshing = false;
    return ESP_ERR_TIMEOUT;
  }
  bod_restore();

  s_tone_open = true;
  s_tone_unit = unit_frames;
  s_tone_table = table;
  s_refreshing = false;
  LOGF("%s session open, unit %u frames, pull %u frames = %u ms at %u.%u ms per frame, open %u ms",
       name, (unsigned)unit_frames, (unsigned)frames,
       (unsigned)((frames * EPD_B_FRAME_US) / 1000u),
       (unsigned)(EPD_B_FRAME_US / 1000u), (unsigned)((EPD_B_FRAME_US % 1000u) / 100u),
       (unsigned)(now_ms() - t0));
  return ESP_OK;
}

esp_err_t epd_tone_pass(unsigned pulls, uint32_t *panel_ms_total)
{
  return epd_tone_pull(pulls, true, panel_ms_total);
}

esp_err_t epd_tone_pull(unsigned pulls, bool resend, uint32_t *panel_ms_total)
{
  if (panel_ms_total) *panel_ms_total = 0;
  if (!s_tone_open || s_refreshing) return ESP_ERR_INVALID_STATE;
  if (pulls == 0u || pulls > EPD_TONE_PULLS_MAX) return ESP_ERR_INVALID_ARG;

  s_refreshing = true;
  const uint32_t t_frame = now_ms();
  bus_bitbang();
  uint32_t send_ms = 0;
  if (resend && !send_planes(s_bw, s_red, &send_ms)) {
    LOGF("tone plane send failed, closing the session");
    tone_close(true);
    s_refreshing = false;
    return ESP_FAIL;
  }

  // Triggers only. 0xB0 is not written again, because a rewrite inside a
  // session drops the panel back onto its own waveform.
  bod_hold_off();
  uint32_t total = 0;
  unsigned done = 0;
  int rc = 0;
  for (unsigned i = 0; i < pulls; i++) {
    rc = retrigger(EPD_TONE_BUSY_MS);
    if (rc != 0) break;
    total += s_last_panel_ms;
    done++;
  }
  if (panel_ms_total) *panel_ms_total = total;

  if (rc != 0) {
    LOGF("tone pull %u of %u did not finish, closing the session",
         done + 1u, pulls);
    tone_close(true);
  }
  bod_restore();

  s_refreshing = false;
  LOGF("tone pass %u of %u pulls, panel %u ms, send %u ms, frame %u ms",
       done, pulls, (unsigned)total, (unsigned)send_ms,
       (unsigned)(now_ms() - t_frame));
  return rc == 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool    epd_tone_is_open(void) { return s_tone_open; }
uint8_t epd_tone_unit(void)    { return s_tone_unit; }
epd_tone_table_t epd_tone_table(void) { return s_tone_open ? s_tone_table : EPD_TONE_TABLE_NONE; }

// -----------------------------------------------------------------------------
// Panel and framebuffer
// -----------------------------------------------------------------------------
const char* epd_panel_name(void)
{
  switch (s_panel) {
    case EPD_PANEL_084: return "E2969JS084 (driver 8)";
    case EPD_PANEL_B1:  return "E2969JS0B1 (driver B)";
    default:            return "unknown";
  }
}

uint8_t* epd_plane_bw(void)  { return s_bw; }
uint8_t* epd_plane_red(void) { return s_red; }

void epd_clear(void)
{
  memset(s_bw, 0, EPD_PLANE_BYTES);
  memset(s_red, 0, EPD_PLANE_BYTES);
}
