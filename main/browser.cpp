// Page cycle task. Waits are task-notification waits, so the console's "next" ends them early.
// The panel is drawn only for a rendered page: a draw costs about 12 s, so status (no WiFi, no URLs,
// a failed page) goes to the log and the console's status command, and the last page stays on the
// panel.

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
#include "tones.h"
#include "wifi.h"

static const char* TAG = "browser";

// Layout plus the TLS handshakes inside createFromString. High-water mark measured at 15.2 KB for
// layout alone on Wikipedia; TLS adds about 10 KB. The stack is internal RAM: with it in PSRAM,
// HTTP fetches from this task stalled (connections opened, then no data for the whole timeout)
// while the same fetch from an internal-stack task completed in 0.7 s.
static const uint32_t RENDER_STACK = 40 * 1024;
// Pause after a failed page before the next one.
static const int64_t FAIL_WAIT_MS = 10 * 1000;
// A button selection loads once no button has been pressed for this long.
static const int64_t SELECT_HOLD_MS = 1500;

namespace
{
TaskHandle_t        s_task;
volatile int        s_index = 0;
volatile bool       s_back  = false; // browser_prev() was called: step back two from the advanced index
volatile int        s_count = 1;     // pages in the list, for the button selection to wrap
volatile int        s_select = -1;   // tab chosen with the buttons, -1 none
volatile int64_t    s_select_until = 0; // when the selection loads, esp_timer time
const char* volatile s_state = "wait";

void refresh()
{
    s_state = "refresh";
    tone_draw_stats ds;
    bool            ok = tones_draw(tones_frame(), &ds);
    ESP_LOGI(TAG, "draw %s, %u + %u pulls, %u ms", ok ? "ok" : "failed", (unsigned)ds.pulls1, (unsigned)ds.pulls2,
             (unsigned)ds.total_ms);
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
    s_select      = -1;
    int64_t t_end = esp_timer_get_time() + total_ms * 1000;
    for(;;)
    {
        int64_t now = esp_timer_get_time();
        if(s_select >= 0)
        {
            // a button selection is open: the timer pauses, the LEDs show the chosen tab, and the
            // page loads once the buttons have been quiet for SELECT_HOLD_MS
            if(now >= s_select_until) break;
            leds_select(s_select, s_count);
            wait_ms(std::max<int64_t>(1, (s_select_until - now) / 1000));
            continue;
        }
        int64_t remaining_ms = (t_end - now) / 1000;
        if(remaining_ms <= 0) break;
        leds_timer(static_cast<int>(remaining_ms), static_cast<int>(total_ms));
        if(wait_ms(std::min<int64_t>(remaining_ms, 250)) && s_select < 0) break;
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
        int n   = static_cast<int>(urls.size());
        s_count = n;
        if(s_index >= n) s_index = 0;
        if(s_select >= 0)
        {
            s_index  = s_select % n;
            s_select = -1;
        } else if(s_back)
        {
            // s_index already points past the page shown, so the page before it is two back
            s_back  = false;
            s_index = ((s_index - 2) % n + n) % n;
        }
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
        bool         ok = render_page(url, page.offset, tones_frame(), st, err);
        if(ok && !render_tabs(urls, static_cast<size_t>(s_index), tones_frame()))
            ESP_LOGW(TAG, "tab bar did not render");
        if(ok)
            ESP_LOGI(TAG,
                     "rendered: html %u bytes, doc_h %d, offset %d, fetch %d ms, parse %d ms, layout %d ms, "
                     "images %d ms, draw %d ms of which dither %d ms, images %u refs %u in view %u failed, PSRAM low %u",
                     (unsigned)st.html_bytes, st.doc_h, st.offset, st.fetch_ms, st.parse_ms, st.layout_ms,
                     st.images_ms, st.draw_ms, st.dither_ms, (unsigned)st.images, (unsigned)st.img_in_view, (unsigned)st.img_fail,
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

void browser_prev()
{
    s_back = true;
    if(s_task) xTaskNotifyGive(s_task);
}

void browser_step(int delta)
{
    if(strcmp(s_state, "wait") != 0 || !s_task) return;
    int n = s_count > 0 ? s_count : 1;
    // the selection starts at the page shown, which is one before the advanced index
    int cur = s_select >= 0 ? s_select : ((s_index - 1) % n + n) % n;
    s_select       = ((cur + delta) % n + n) % n;
    s_select_until = esp_timer_get_time() + SELECT_HOLD_MS * 1000;
    xTaskNotifyGive(s_task);
}

int browser_page_index()
{
    return s_index;
}

const char* browser_state()
{
    return s_state;
}
