// Page cycle task. Waits are task-notification waits, so the console's "next" ends them early.
// The panel is refreshed only for a rendered page: a refresh costs 32 s or more, so status (no WiFi,
// no URLs, a failed page) goes to the log and the console's status command, and the last page stays
// on the panel.

#include "browser.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "config.h"
#include "epd97.h"
#include "leds.h"
#include "render.h"
#include "wifi.h"

static const char* TAG = "browser";

// Layout plus the TLS handshakes inside createFromString. High-water mark measured at 15.2 KB for
// layout alone on Wikipedia; TLS adds about 10 KB. The stack is internal RAM: with it in PSRAM,
// HTTP fetches from this task stalled (connections opened, then no data for the whole timeout)
// while the same fetch from an internal-stack task completed in 0.7 s.
static const uint32_t RENDER_STACK = 40 * 1024;
// Pause after a failed page before the next one.
static const int64_t FAIL_WAIT_MS = 10 * 1000;

namespace
{
TaskHandle_t        s_task;
volatile int        s_index = 0;
const char* volatile s_state = "wait";

void refresh()
{
    s_state    = "refresh";
    int64_t t0 = esp_timer_get_time();
    bool    ok = refresh_display(EPD_SLOW);
    ESP_LOGI(TAG, "refresh %s, panel %lu ms, total %lld ms", ok ? "ok" : "failed",
             (unsigned long)epd_last_panel_ms(), (esp_timer_get_time() - t0) / 1000);
}

// Sleeps up to ms, less when browser_next() is called. True when it was.
bool wait_ms(int64_t ms)
{
    if(ms <= 0) return false;
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) != 0;
}

// The countdown: teal bar draining over total_ms, LEDs updated every 250 ms. Ends early on
// browser_next(). A notification left over from before the wait is dropped first, so a button
// pressed during the render does not skip the countdown.
void countdown(int64_t total_ms)
{
    ulTaskNotifyTake(pdTRUE, 0);
    int64_t t_end = esp_timer_get_time() + total_ms * 1000;
    for(;;)
    {
        int64_t remaining_ms = (t_end - esp_timer_get_time()) / 1000;
        if(remaining_ms <= 0) break;
        leds_timer(static_cast<int>(remaining_ms), static_cast<int>(total_ms));
        if(wait_ms(std::min<int64_t>(remaining_ms, 250))) break;
    }
}

void browser_task(void*)
{
    const char* logged = nullptr; // blocker already logged, so it is logged once per change
    for(;;)
    {
        std::vector<page_entry> urls = cfg_urls();
        const char*             blocker = nullptr;
        if(!wifi_is_connected()) blocker = "nowifi";
        else if(urls.empty()) blocker = "nourls";
        if(blocker)
        {
            s_state = blocker;
            leds_off();
            if(logged != blocker)
            {
                char ssid[33];
                bool have = wifi_get_ssid(ssid, sizeof(ssid));
                if(blocker == std::string("nowifi"))
                    ESP_LOGW(TAG, "waiting for WiFi: %s%s", have ? "connecting to " : "none stored, console: wifi SSID PASSWORD",
                             have ? ssid : "");
                else
                    ESP_LOGW(TAG, "waiting for URLs, console: url add https://...");
                logged = blocker;
            }
            wait_ms(1000);
            continue;
        }
        logged = nullptr;

        int64_t t_start = esp_timer_get_time();
        if(s_index >= static_cast<int>(urls.size())) s_index = 0;
        page_entry  page = urls[s_index];
        std::string url  = page.url;
        int         secs = page.seconds > 0 ? page.seconds : CONFIG_B26_CYCLE_SECONDS;
        ESP_LOGI(TAG, "PAGE %d of %u: %s offset %d for %d s", s_index + 1, (unsigned)urls.size(), url.c_str(),
                 page.offset, secs);
        ESP_LOGI(TAG, "free internal %u, PSRAM %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        s_state = "render";
        leds_load(1, 8); // fetch starts; render_page advances the bar from here
        render_stats st;
        std::string  err;
        bool         ok = render_page(url, page.offset, epd_plane_bw(), epd_plane_red(), st, err);
        if(ok && !render_tabs(urls, static_cast<size_t>(s_index), epd_plane_bw(), epd_plane_red()))
            ESP_LOGW(TAG, "tab bar did not render");
        if(ok)
            ESP_LOGI(TAG,
                     "rendered: html %u bytes, doc_h %d, offset %d, fetch %d ms, parse %d ms, layout %d ms, "
                     "images %d ms, draw %d ms, images %u refs %u in view %u failed, PSRAM low %u",
                     (unsigned)st.html_bytes, st.doc_h, st.offset, st.fetch_ms, st.parse_ms, st.layout_ms,
                     st.images_ms, st.draw_ms, (unsigned)st.images, (unsigned)st.img_in_view, (unsigned)st.img_fail,
                     (unsigned)st.psram_low);
        s_index = (s_index + 1) % static_cast<int>(urls.size());
        if(!ok)
        {
            // the previous page stays on the panel
            ESP_LOGW(TAG, "PAGE FAILED %s: %s", url.c_str(), err.c_str());
            s_state = "wait";
            countdown(FAIL_WAIT_MS);
            continue;
        }
        refresh();
        ESP_LOGI(TAG, "PAGE DONE %s after %lld ms", url.c_str(), (esp_timer_get_time() - t_start) / 1000);

        s_state            = "wait";
        int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;
        countdown(static_cast<int64_t>(secs) * 1000 - elapsed_ms);
    }
}
} // namespace

bool browser_start()
{
    // Core 1: the task watchdog does not watch that core's idle task (sdkconfig.defaults), so a
    // 20 s layout does not trip it.
    BaseType_t rc = xTaskCreatePinnedToCore(browser_task, "browser", RENDER_STACK, nullptr, 5, &s_task, 1);
    if(rc != pdPASS) ESP_LOGE(TAG, "browser task not created");
    return rc == pdPASS;
}

void browser_next()
{
    if(s_task) xTaskNotifyGive(s_task);
}

int browser_page_index()
{
    return s_index;
}

const char* browser_state()
{
    return s_state;
}
