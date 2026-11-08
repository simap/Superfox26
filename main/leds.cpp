#include "leds.h"

#include "esp_log.h"
#include "led_strip.h"

static const char* TAG = "leds";

static const int NUM_LEDS  = 8;
static const int LED_GPIO  = 38; // WS2812 data, from the stock firmware's led_bar.py
static const int BRIGHT_PCT = 10;

// full-scale colors, scaled by BRIGHT_PCT at write time
static const uint8_t AMBER[3] = {255, 160, 0};
static const uint8_t TEAL[3]  = {0, 128, 128};

static led_strip_handle_t s_strip;

static void bar(const uint8_t* color, int lit)
{
    if(!s_strip) return;
    if(lit < 0) lit = 0;
    if(lit > NUM_LEDS) lit = NUM_LEDS;
    for(int i = 0; i < NUM_LEDS; i++)
    {
        if(i < lit)
            led_strip_set_pixel(s_strip, i, color[0] * BRIGHT_PCT / 100, color[1] * BRIGHT_PCT / 100,
                                color[2] * BRIGHT_PCT / 100);
        else led_strip_set_pixel(s_strip, i, 0, 0, 0);
    }
    led_strip_refresh(s_strip);
}

void leds_init()
{
    led_strip_config_t cfg = {};
    cfg.strip_gpio_num         = LED_GPIO;
    cfg.max_leds               = NUM_LEDS;
    cfg.led_model              = LED_MODEL_WS2812;
    cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt = {};
    rmt.clk_src                = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz          = 10 * 1000 * 1000;
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
    if(s_strip) led_strip_clear(s_strip);
}

void leds_load(int done, int total)
{
    if(total <= 0) return;
    bar(AMBER, (done * NUM_LEDS + total - 1) / total);
}

void leds_timer(int remaining_ms, int total_ms)
{
    if(total_ms <= 0 || remaining_ms <= 0)
    {
        bar(TEAL, 0);
        return;
    }
    bar(TEAL, (remaining_ms * NUM_LEDS + total_ms - 1) / total_ms);
}
