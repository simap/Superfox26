#include "leds.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

#include "leds_logic.h"

static const char* TAG = "leds";

static const int64_t FLASH_US = 300 * 1000;

// Raw levels by leds_color.
static const uint8_t RGB[][3] = {
    {0, 0, 0}, {2, 1, 0}, {0, 1, 1}, {1, 0, 2}, {1, 1, 1}, {0, 0, 2}, {1, 0, 0}, {0, 2, 0},
};

static led_strip_handle_t s_strip;
static SemaphoreHandle_t  s_lock;
static leds_layers        s_layers;
static bool               s_busy = true;
static int64_t            s_select_t0;
static int64_t            s_flash_until;
static bool               s_stopped;
static leds_frame         s_shown;
static bool               s_shown_valid;

namespace
{
struct guard
{
    guard() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~guard() { xSemaphoreGive(s_lock); }
};
} // namespace

// Under the lock. Writes the strip only when the frame changes.
static void show()
{
    if(!s_strip || s_stopped) return;
    const int64_t now = esp_timer_get_time();
    leds_layers   l   = s_layers;
    l.flash           = now < s_flash_until;
    l.select_off      = l.select && s_busy && leds_blink_off(now - s_select_t0);
    const leds_frame f = leds_compose(l);
    if(s_shown_valid && leds_same(f, s_shown)) return;
    for(int i = 0; i < LEDS_N; i++)
    {
        const uint8_t* c = RGB[f.px[i]];
        led_strip_set_pixel(s_strip, i, c[0], c[1], c[2]);
    }
    led_strip_refresh(s_strip);
    s_shown       = f;
    s_shown_valid = true;
}

static void base(leds_color color, int lit, bool timer)
{
    if(!s_lock) return;
    guard g;
    s_layers.base_color = color;
    s_layers.base_lit   = lit;
    s_layers.base_timer = timer;
    show();
}

void leds_init()
{
    s_lock = xSemaphoreCreateMutex();
    if(!s_lock)
    {
        ESP_LOGE(TAG, "no lock");
        return;
    }
    led_strip_config_t cfg = {};
    cfg.strip_gpio_num         = LEDS_DATA_GPIO;
    cfg.max_leds               = LEDS_N;
    cfg.led_model              = LED_MODEL_WS2812;
    cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt = {};
    rmt.clk_src                = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz          = 10 * 1000 * 1000;
    rmt.mem_block_symbols      = 240;
    esp_err_t err              = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "led_strip_new_rmt_device: %s", esp_err_to_name(err));
        s_strip = nullptr;
        return;
    }
    led_strip_clear(s_strip);
}

void leds_off()
{
    base(LEDS_DARK, 0, false);
}

void leds_red()
{
    base(LEDS_RED, 1, false);
}

void leds_amber()
{
    base(LEDS_AMBER, 1, false);
}

void leds_load(int done, int total)
{
    if(total <= 0) return;
    base(LEDS_AMBER, leds_ceil_lit(done, total), false);
}

void leds_timer(int remaining_ms, int total_ms)
{
    base(LEDS_TEAL, leds_ceil_lit(remaining_ms, total_ms), true);
}

void leds_select(int offset)
{
    if(!s_lock) return;
    guard g;
    s_layers.select = true;
    s_layers.offset = offset;
    s_select_t0     = esp_timer_get_time();
    show();
}

void leds_select_end()
{
    if(!s_lock) return;
    guard g;
    s_layers.select = false;
    show();
}

void leds_hold(bool on)
{
    if(!s_lock) return;
    guard g;
    s_layers.hold = on;
    show();
}

void leds_busy(bool busy)
{
    if(!s_lock) return;
    guard g;
    s_busy = busy;
    show();
}

void leds_flash()
{
    if(!s_lock) return;
    guard g;
    s_flash_until = esp_timer_get_time() + FLASH_US;
    show();
}

void leds_tick()
{
    if(!s_lock) return;
    guard g;
    show();
}

void leds_stop()
{
    if(!s_lock) return;
    guard g;
    if(s_strip && !s_stopped) led_strip_clear(s_strip);
    s_stopped = true;
}
