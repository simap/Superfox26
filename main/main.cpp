// Boot path of the Superfox26 badge browser.
// A subsystem the browser needs that fails to start keeps the browser from running: on USB power
// app_main returns and leaves only the console up, while on battery it stops wifi, turns off the
// panel supply and deep sleeps for BROWSER_RETRY_S, so the next boot retries.
// When a reset cut the previous panel draw, it halts and only removing power starts it again.

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

static void panel_supply_off()
{
    gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 0 : 1);
    gpio_set_direction((gpio_num_t)EPD_PIN_PWR, GPIO_MODE_OUTPUT);
}

static void browser_not_started(const power_reading& boot)
{
    if(boot.usb) return;
    wifi_stop();
    panel_supply_off();
    power_sleep(BROWSER_RETRY_S);
}

// Another draw could be cut the same way, and repeated cut draws can damage the panel
// (rtcstate.h). So the firmware stops here on USB and on battery until power is removed.
[[noreturn]] static void halt_after_cut_draw()
{
    ESP_LOGE(TAG, "a reset cut the last panel draw, halted. Remove power to start again");
    panel_supply_off();
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
    ESP_LOGI(TAG, "Superfox26, litehtml " LITEHTML_REV);
    ESP_LOGI(TAG, "free internal %u, PSRAM %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    leds_init();
    if(rtcstate_draw_interrupted()) halt_after_cut_draw();
    // The wake press shows on the LEDs before the boot ends. browser_start() opens the same selection.
    if(power_wake_cause() == POWER_WAKE_USER_B) leds_select(1);
    else if(power_wake_cause() == POWER_WAKE_MENU_MINUS) leds_select(0);
    console_start();
    cfg_init();
    bool kept = rtcstate_begin();
    sizecache_init(kept);
    power_reading boot = browser_init(kept);
    if(esp_reset_reason() == ESP_RST_BROWNOUT && !boot.usb)
    {
        ESP_LOGE(TAG, "brownout reset on battery, sleeping until a button wakes the badge");
        panel_supply_off();
        power_sleep(0);
    }
    wifi_init(kept, !boot.low);
    rtcstate_commit();

    bool panel_ok = epd_begin();
    ESP_LOGI(TAG, "epd_begin: %s, panel %s", panel_ok ? "ok" : "failed", epd_panel_name());
    if(!panel_ok)
    {
        ESP_LOGE(TAG, "no framebuffer, console only");
        browser_not_started(boot);
        return;
    }

    if(!tones_init())
    {
        ESP_LOGE(TAG, "tone frame not allocated, console only");
        browser_not_started(boot);
        return;
    }

    if(!render_init())
    {
        ESP_LOGE(TAG, "renderer not started: fonts missing on the data partition (idf.py flash writes "
                      "build/data.bin with them) or no memory, see the log above");
        browser_not_started(boot);
        return;
    }

    browser_start();
    buttons_start();
}
