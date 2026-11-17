// browser26: fetches a list of pages in turn, renders each with litehtml into a tone frame and draws
// it on the 9.7 inch three pigment panel through the measured tone map. The URL list lives on the cfg partition and the WiFi
// credentials in NVS, both set over the serial console (console.h).

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "browser.h"
#include "buttons.h"
#include "config.h"
#include "console.h"
#include "epd97.h"
#include "leds.h"
#include "render.h"
#include "sizecache.h"
#include "tones.h"
#include "wifi.h"

static const char* TAG = "main";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "browser26, litehtml " LITEHTML_REV);
    ESP_LOGI(TAG, "free internal %u, PSRAM %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    console_start();
    leds_init();
    cfg_init();
    sizecache_init();
    wifi_init();

    bool panel_ok = epd_begin();
    ESP_LOGI(TAG, "epd_begin: %s, panel %s", panel_ok ? "ok" : "failed", epd_panel_name());
    if(!panel_ok)
    {
        ESP_LOGE(TAG, "no framebuffer, console only");
        return; // the console task keeps running
    }

    if(!tones_init())
    {
        ESP_LOGE(TAG, "tone frame not allocated, console only");
        return;
    }

    if(!render_init())
    {
        ESP_LOGE(TAG, "fonts missing on the data partition, idf.py flash writes build/data.bin with them");
        return;
    }

    browser_start();
    buttons_start();
}
