// browser26: fetches a list of pages in turn, renders each with litehtml into a tone frame and draws
// it on the 9.7 inch three pigment panel through the measured tone map. The URL list lives on the cfg partition and the WiFi
// credentials in NVS, both set over the serial console (console.h).

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "browser.h"
#include "browser_logic.h"
#include "buttons.h"
#include "config.h"
#include "console.h"
#include "epd97.h"
#include "epd97_config.h"
#include "leds.h"
#include "power.h"
#include "render.h"
#include "rtcstate.h"
#include "sizecache.h"
#include "tones.h"
#include "wifi.h"

static const char* TAG = "main";

// Drives the panel supply switch off. The switch has no pull resistor and the pad floats after a reset
// until epd_begin() sets it, so a path that sleeps or halts without epd_begin() calls this first.
// power_sleep() then holds the pad at this level.
static void panel_supply_off()
{
    gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 0 : 1);
    gpio_set_direction((gpio_num_t)EPD_PIN_PWR, GPIO_MODE_OUTPUT);
}

// The browser does not start. On USB the console keeps running. On battery the badge stops WiFi and
// sleeps BROWSER_RETRY_S, then tries again from boot. The browser task and the buttons task do not exist
// yet, so no other task drives the LEDs and the LED call inside power_sleep() is safe from this task.
static void browser_not_started(const power_reading& boot)
{
    if(boot.usb) return;
    wifi_stop();
    panel_supply_off();
    power_sleep(BROWSER_RETRY_S);
}

// A reset cut the last panel draw (rtcstate.h). Another draw could be cut the same way, and repeated cut
// draws can damage the panel, so the firmware stops here on USB and on battery: the panel supply is off,
// nothing else starts, and the first LED flashes red once a second until power is removed.
[[noreturn]] static void halt_after_cut_draw()
{
    ESP_LOGE(TAG, "a reset cut the last panel draw, halted. Remove power to start again");
    panel_supply_off();
    // The hold keeps the supply off through the light sleep between flashes, which saves the cell.
    gpio_hold_en((gpio_num_t)EPD_PIN_PWR);
    esp_sleep_enable_timer_wakeup(950 * 1000);
    for(;;)
    {
        leds_red();
        vTaskDelay(pdMS_TO_TICKS(50));
        leds_off();
        esp_light_sleep_start();
    }
}

extern "C" void app_main(void)
{
    power_init();
    ESP_LOGI(TAG, "browser26, litehtml " LITEHTML_REV);
    ESP_LOGI(TAG, "free internal %u, PSRAM %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    leds_init();
    if(rtcstate_draw_interrupted()) halt_after_cut_draw();
    // A button woke the badge: one LED answers the press at once. A timer wake shows nothing here.
    if(power_wake_cause() == POWER_WAKE_USER_B || power_wake_cause() == POWER_WAKE_MENU_MINUS) leds_load(1, 8);
    console_start();
    cfg_init();
    bool kept = rtcstate_begin();
    sizecache_init(kept);
    // Reads the rail, so it runs before wifi_init() starts the radio. A low battery leaves the radio
    // stopped: the first cycle draws the notice or sleeps and needs no WiFi.
    power_reading boot = browser_init(kept);
    // A brownout reset on battery: the cell cannot carry the load, and a start of WiFi would reset the
    // badge again. No notice is drawn, because a draw at this voltage can be cut. The badge sleeps until
    // a button wakes it. No other task drives the LEDs yet, so power_sleep() is safe from this task.
    if(esp_reset_reason() == ESP_RST_BROWNOUT && !boot.usb)
    {
        ESP_LOGE(TAG, "brownout reset on battery, sleeping until a button wakes the badge");
        panel_supply_off();
        power_sleep(0);
    }
#ifndef TONES_BENCH
    // The benchmark build leaves WiFi off: its tasks and interrupts would land inside the timed spans.
    wifi_init(kept, !boot.low);
#endif
    rtcstate_commit();

    bool panel_ok = epd_begin();
    ESP_LOGI(TAG, "epd_begin: %s, panel %s", panel_ok ? "ok" : "failed", epd_panel_name());
    if(!panel_ok)
    {
        ESP_LOGE(TAG, "no framebuffer, console only");
        browser_not_started(boot);
        return; // the console task keeps running
    }

    if(!tones_init())
    {
        ESP_LOGE(TAG, "tone frame not allocated, console only");
        browser_not_started(boot);
        return;
    }

#ifdef TONES_BENCH
    tones_bench();
    return; // console only, the panel is not drawn
#endif

    if(!render_init())
    {
        ESP_LOGE(TAG, "fonts missing on the data partition, idf.py flash writes build/data.bin with them");
        browser_not_started(boot);
        return;
    }

    browser_start();
    buttons_start();
}
