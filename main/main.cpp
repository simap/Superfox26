// Boot path of the Superfox26 badge browser.
// A subsystem the browser needs that fails to start keeps the browser from running. The panel
// supply goes off, then on USB power app_main returns and leaves only the console up, while on
// battery it stops wifi and deep sleeps for BROWSER_RETRY_S, so the next boot retries.
// When a reset cut the previous panel draw, it halts and only removing power starts it again.
// A critical battery reading sleeps before anything else starts.

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

// The pin is driven as well, because before epd_begin() or epd_supply_on() the driver has not
// configured it.
static void panel_supply_off()
{
    epd_sleep();
    gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 0 : 1);
    gpio_set_direction((gpio_num_t)EPD_PIN_PWR, GPIO_MODE_OUTPUT);
}

static void browser_not_started(const power_reading& boot)
{
    panel_supply_off();
    if(boot.usb) return;
    wifi_stop();
    power_sleep(BROWSER_RETRY_S);
}

[[noreturn]] static void critical_sleep()
{
    ESP_LOGE(TAG, "battery critical, sleeping %d s or until a button", POWER_CRITICAL_CHECK_S);
    power_keep_critical(true);
    panel_supply_off();
    power_sleep(POWER_CRITICAL_CHECK_S);
}

// On a button wake the LEDs, which the panel supply feeds, show the selection from the start of the
// boot. Returns true when the load of the supply pulls the battery low.
static bool early_panel_supply(bool was_low, const power_reading& before)
{
    if(!power_early_supply(power_wake_cause(), before)) return false;
    epd_supply_on();
    vTaskDelay(pdMS_TO_TICKS(POWER_SUPPLY_LOAD_MS));
    power_supply_after after = power_supply_check(power_read(was_low));
    if(after == POWER_SUPPLY_CRITICAL) critical_sleep();
    if(after == POWER_SUPPLY_KEEP) return false;
    ESP_LOGW(TAG, "the panel supply pulls the battery low, supply off");
    epd_sleep();
    return true;
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

    bool          kept    = rtcstate_begin();
    bool          was_low = browser_was_low(kept);
    power_reading rail    = power_read(was_low);
    if(rail.critical) critical_sleep();
    power_keep_critical(false);

    leds_init();
    if(rtcstate_draw_interrupted()) halt_after_cut_draw();
    bool low = early_panel_supply(was_low, rail);
    // The wake press shows on the LEDs before the boot ends. browser_start() opens the same selection.
    if(power_wake_cause() == POWER_WAKE_USER_B) leds_select(1);
    else if(power_wake_cause() == POWER_WAKE_MENU_MINUS) leds_select(0);
    console_start();
    cfg_init();
    sizecache_init(kept);
    power_reading boot = browser_init(kept, low);
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
