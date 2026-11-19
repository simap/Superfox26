// Only the browser task renders pages and draws to the panel: render_page, render_html, render_tabs
// and tones_draw run on it alone.

#include "browser.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "browser_logic.h"
#include "buttons.h"
#include "config.h"
#include "epd97.h"
#include "leds.h"
#include "power.h"
#include "render.h"
#include "rtcstate.h"
#include "tabs.h"
#include "tones.h"
#include "wifi.h"

static const char* TAG = "browser";

static const uint32_t RENDER_STACK = 40 * 1024;
static const int64_t FAIL_WAIT_MS = 10 * 1000;
static const int64_t SELECT_HOLD_US = 1500 * 1000;
static const int WIFI_FAIL_FLASH_MS = 50;

static const char* const LOW_BATTERY_HTML =
    "<html><body style=\"margin:0;padding:80px 80px;font-family:sans-serif;color:#000;background:#fff\">"
    "<div style=\"height:180px\">"
    "<div style=\"float:left;width:312px;height:132px;padding:12px;border:12px solid #000;border-radius:20px\">"
    "<div style=\"width:52px;height:132px;background:#ea0000;border-radius:6px\"></div></div>"
    "<div style=\"float:left;width:28px;height:72px;margin-top:54px;background:#000;"
    "border-radius:0 10px 10px 0\"></div>"
    "</div>"
    "<div style=\"clear:both;font-size:96px;font-weight:bold;margin-top:48px\">Battery low</div>"
    "<div style=\"font-size:44px;margin-top:56px\">Connect USB, then press User B or Menu -</div>"
    "</body></html>";

static const char* const WELCOME_HEAD =
    "<html><body style=\"margin:0;padding:48px 72px;font-family:sans-serif;color:#000;background:#fff\">"
    "<table style=\"border-collapse:collapse\"><tr><td style=\"padding:0 40px 0 0\">"
    "<img style=\"width:200px;height:200px\" src=\"";
static const char* const WELCOME_TAIL =
    "\"></td><td style=\"vertical-align:middle\"><div style=\"font-size:88px;font-weight:bold\">Superfox26</div>"
    "<div style=\"font-size:30px;margin-top:8px\">a browser for the Supercon X badge</div></td></tr></table>"
    "<div style=\"font-size:40px;margin-top:44px\">WiFi is not set up.<br>Connect USB and press User B.</div>"
    "<div style=\"font-size:40px;margin-top:24px\">Then on the serial console:</div>"
    "<div style=\"font-size:40px;margin-top:24px;font-family:monospace\">wifi SSID PASSWORD</div>"
    "</body></html>";

extern const uint8_t logo_200_start[] asm("_binary_panel_200_png_start");
extern const uint8_t logo_200_end[] asm("_binary_panel_200_png_end");

static std::string welcome_html()
{
    return std::string(WELCOME_HEAD) + tabs_png_url(logo_200_start, logo_200_end) + WELCOME_TAIL;
}

namespace
{
TaskHandle_t             s_task;
std::atomic<int>         s_count{0};
std::atomic<int>         s_show{0};
std::atomic<const char*> s_state{"boot"};

// The tab selection of the buttons and the console. The button task, the console task and the
// browser task change it, so it and the fields after it change only under s_sel_lock.
struct selection
{
    bool    open;
    bool    force; // a wake or the console asked for the load, so it loads even when it changes nothing
    int     base;  // the page the offset on the LEDs counts from
    int     target;
    int64_t until; // the hold ends, esp_timer time
};
SemaphoreHandle_t s_sel_lock;
selection         s_sel;
unsigned          s_presses;      // counts every press, so a sleep that has begun can tell one came in
bool              s_sleeping;     // past the last point where a press can stop the sleep
int               s_loading = -1; // the page being rendered
int               s_drawing = -1; // the page being drawn

struct sel_guard
{
    sel_guard() { xSemaphoreTake(s_sel_lock, portMAX_DELAY); }
    ~sel_guard() { xSemaphoreGive(s_sel_lock); }
};

// A change of these needs a new magic in main/rtcstate.cpp.
RTC_NOINIT_ATTR volatile int s_index;
RTC_NOINIT_ATTR volatile int s_panel;
RTC_NOINIT_ATTR volatile int s_low;
RTC_NOINIT_ATTR volatile int s_hold;

void set_state(const char* state)
{
    if(!strcmp(state, s_state.load())) return;
    s_state = state;
    leds_busy(strcmp(state, "wait") && strcmp(state, "hold"));
    printf("EVT state=%s index=%d\n", state, static_cast<int>(s_index));
    fflush(stdout);
}

const int PANEL_NONE    = -1;
const int PANEL_LOWBATT = -2;
const int PANEL_WELCOME = -3;

int panel_of(browser_screen screen)
{
    return screen == BROWSER_SCREEN_LOWBATT ? PANEL_LOWBATT : PANEL_WELCOME;
}

// Under the lock. The page the user sees: the one being drawn, else the one on the panel.
int base_page(int n)
{
    if(s_drawing >= 0 && s_drawing < n) return s_drawing;
    if(s_panel >= 0 && s_panel < n) return s_panel;
    return browser_wrap(s_index - 1, n);
}

// Under the lock.
void sel_open(int n, int target, int64_t until, bool force)
{
    if(!s_sel.open)
    {
        s_sel.open  = true;
        s_sel.force = false;
        s_sel.base  = base_page(n);
    }
    s_sel.target = browser_wrap(target, n);
    s_sel.until  = until;
    s_sel.force  = s_sel.force || force;
    leds_select(browser_offset(s_sel.target, s_sel.base, n));
    if(s_task) xTaskNotifyGive(s_task);
}

// Under the lock.
void sel_close()
{
    s_sel.open = false;
    leds_select_end();
}

// Under the lock. A due selection that changes nothing closes here, and the answer is then NONE.
browser_sel_action sel_check(int loading)
{
    browser_sel_action a = browser_sel_next(s_sel.open, esp_timer_get_time() >= s_sel.until, s_sel.force,
                                            s_sel.target, s_sel.base, loading);
    if(a != BROWSER_SEL_DROP) return a;
    sel_close();
    return BROWSER_SEL_NONE;
}

bool wait_ms(int64_t ms)
{
    if(ms <= 0) return false;
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) != 0;
}

// Waits out the hold of an open selection. loading as for browser_sel_next(). The answer is NONE or
// LOAD, and a LOAD selection stays open for take_page().
browser_sel_action sel_wait(int loading)
{
    for(;;)
    {
        int64_t left_us;
        {
            sel_guard          g;
            browser_sel_action a = sel_check(loading);
            if(a != BROWSER_SEL_WAIT) return a;
            left_us = s_sel.until - esp_timer_get_time();
        }
        wait_ms(std::max<int64_t>(1, (left_us + 999) / 1000));
    }
}

bool render_cancel()
{
    sel_guard g;
    return sel_check(s_loading) == BROWSER_SEL_LOAD;
}

power_reading s_power = {-1, true, false, false, false};
int64_t       s_power_us;

void read_power()
{
    power_reading r      = power_read(s_low != 0);
    bool          unsure = r.rail_mv < 0 && !r.host;
    if((!r.usb || unsure) && !wifi_is_stopped())
    {
        wifi_stop();
        r = power_read(s_low != 0);
    }
    if(r.usb) wifi_resume();
    s_power    = r;
    s_power_us = esp_timer_get_time();
}

[[noreturn]] void sleep_now(int seconds)
{
    {
        sel_guard g;
        s_sleeping = true;
        set_state("sleep");
    }
    wifi_stop();
    power_sleep(seconds, buttons_released());
}

// A deep sleep that a press stops: an open selection, or a press before the sleep is past its last
// check, and then it returns and the cycle goes on awake. seconds 0 sleeps until a button.
void rest(int seconds)
{
    unsigned seen;
    {
        sel_guard g;
        if(s_sel.open) return;
        seen = s_presses;
        set_state("sleep");
    }
    wifi_stop();
    {
        sel_guard g;
        if(s_sel.open || s_presses != seen)
        {
            set_state("wait");
            return;
        }
        s_sleeping = true;
    }
    power_sleep(seconds, buttons_released());
}

bool refresh()
{
    set_state("refresh");
    tone_draw_stats ds;
    rtcstate_draw_begin();
    bool ok = tones_draw(tones_frame(), &ds);
    rtcstate_draw_end();
    ESP_LOGI(TAG, "draw %s, %u + %u pulls, %u ms", ok ? "ok" : "failed", (unsigned)ds.pulls1, (unsigned)ds.pulls2,
             (unsigned)ds.total_ms);
    return ok;
}

// The page time, total_ms from now, on the LEDs. Ends early when a selection loads or a notice is
// asked for. Hold stops the clock until the hold ends.
void countdown(int64_t total_ms)
{
    int64_t t_end   = esp_timer_get_time() + total_ms * 1000;
    int64_t held_at = -1;
    for(;;)
    {
        if(sel_wait(-1) == BROWSER_SEL_LOAD || s_show.load()) return;
        int64_t now = esp_timer_get_time();
        leds_timer(static_cast<int>(std::max<int64_t>(0, t_end - now) / 1000), static_cast<int>(total_ms));
        if(s_hold)
        {
            if(held_at < 0) held_at = now;
            set_state("hold");
            // On USB the badge stays awake while it holds, so it reads the rail as the cycle does: a badge
            // unplugged while it holds goes to sleep until a button.
            int64_t next_read_ms = BROWSER_USB_RECHECK_MS - (now - s_power_us) / 1000;
            if(next_read_ms <= 0)
            {
                read_power();
                if(!s_power.usb) rest(0);
                continue;
            }
            wait_ms(next_read_ms);
            continue;
        }
        if(held_at >= 0)
        {
            t_end += now - held_at;
            held_at = -1;
        }
        set_state("wait");
        int64_t remaining_ms = (t_end - now) / 1000;
        if(remaining_ms <= 0) return;
        wait_ms(std::min<int64_t>(remaining_ms, 250));
    }
}

void rest_or_select(int seconds)
{
    set_state("wait");
    while(sel_wait(-1) != BROWSER_SEL_LOAD) rest(s_hold ? 0 : seconds);
}

bool draw_notice(int panel, const char* html)
{
    s_panel = panel;
    if(render_html(html, tones_frame()) && refresh()) return true;
    s_panel = PANEL_NONE;
    ESP_LOGE(TAG, "notice %d did not draw", panel);
    return false;
}

void low_battery_step()
{
    browser_low_step step = browser_low_next(s_power.low, s_panel == PANEL_LOWBATT, s_power.critical);
    s_low                 = step.low ? 1 : 0;
    power_keep_critical(s_power.critical);
    if(step.action == BROWSER_RUN) return;
    wifi_stop();
    if(step.action == BROWSER_CRITICAL_SLEEP)
    {
        ESP_LOGE(TAG, "battery critical, sleeping without a draw");
        sleep_now(POWER_CRITICAL_CHECK_S);
    }
    if(step.action == BROWSER_LOW_DRAW)
    {
        set_state("lowbatt");
        ESP_LOGW(TAG, "battery low, drawing the notice");
        draw_notice(PANEL_LOWBATT, LOW_BATTERY_HTML);
    }
    sleep_now(0);
}

void welcome_step()
{
    if(s_panel != PANEL_WELCOME)
    {
        set_state("welcome");
        ESP_LOGW(TAG, "no WiFi network stored, drawing the welcome notice");
        if(!s_power.usb) wifi_stop();
        draw_notice(PANEL_WELCOME, welcome_html().c_str());
        read_power();
    }
    if(!s_power.usb) rest(0);
}

void show_step()
{
    int screen = s_show.exchange(0);
    if(screen == 0) return;
    bool low   = screen == BROWSER_SCREEN_LOWBATT;
    int  panel = panel_of(static_cast<browser_screen>(screen));
    if(s_panel == panel) ESP_LOGI(TAG, "the %s notice is on the panel already", low ? "low battery" : "welcome");
    else
    {
        set_state("refresh");
        ESP_LOGI(TAG, "drawing the %s notice for the console", low ? "low battery" : "welcome");
        if(!s_power.usb) wifi_stop();
        draw_notice(panel, low ? LOW_BATTERY_HTML : welcome_html().c_str());
        read_power();
    }
    set_state("wait");
    countdown(static_cast<int64_t>(CONFIG_B26_CYCLE_SECONDS) * 1000);
}

bool wifi_connect()
{
    wifi_resume();
    if(wifi_is_connected()) return true;
    set_state("nowifi");
    leds_off();
    return wifi_wait_connected(BROWSER_WIFI_WAIT_MS);
}

void retry_on_battery()
{
    if(s_power.usb && esp_timer_get_time() - s_power_us < BROWSER_USB_RECHECK_MS * 1000LL) return;
    read_power();
    if(!s_power.usb) rest(s_hold ? 0 : BROWSER_RETRY_S);
}

// The page to load: the selection once its hold ends, else the next page of the cycle. It becomes
// s_loading under the same lock, so a press after this counts from the page on the panel.
int take_page(int n)
{
    for(;;)
    {
        int64_t left_us;
        {
            sel_guard g;
            if(s_index < 0 || s_index >= n) s_index = 0;
            browser_sel_action a = sel_check(-1);
            if(a != BROWSER_SEL_WAIT)
            {
                if(a == BROWSER_SEL_LOAD)
                {
                    s_index = s_sel.target % n;
                    sel_close();
                }
                s_loading = s_index;
                return s_index;
            }
            left_us = s_sel.until - esp_timer_get_time();
        }
        set_state("wait");
        wait_ms(std::max<int64_t>(1, (left_us + 999) / 1000));
    }
}

enum blocker_kind
{
    BLOCK_NONE,
    BLOCK_NO_URLS,
    BLOCK_NO_WIFI,
};

void browser_task(void*)
{
    blocker_kind logged = BLOCK_NONE;
    int         fails  = 0;
    for(int64_t t_cycle = 0;; t_cycle = esp_timer_get_time())
    {
        low_battery_step();
        show_step();

        std::vector<page_entry> urls = cfg_urls();
        int                     n    = static_cast<int>(urls.size());
        s_count = n;
        char ssid[33];
        bool have_ssid = wifi_get_ssid(ssid, sizeof(ssid));
        if(!have_ssid) welcome_step();
        blocker_kind blocker = BLOCK_NONE;
        if(n == 0) blocker = BLOCK_NO_URLS;
        else if(!have_ssid || !wifi_connect()) blocker = BLOCK_NO_WIFI;
        if(blocker != BLOCK_NONE)
        {
            set_state(blocker == BLOCK_NO_WIFI ? "nowifi" : "nourls");
            if(blocker == BLOCK_NO_WIFI && have_ssid)
            {
                leds_amber();
                vTaskDelay(pdMS_TO_TICKS(WIFI_FAIL_FLASH_MS));
            }
            leds_off();
            {
                // No page can load. A selection whose hold has ended is dropped, so it does not load
                // long after the press.
                sel_guard g;
                if(s_sel.open && esp_timer_get_time() >= s_sel.until) sel_close();
            }
            if(logged != blocker)
            {
                if(blocker == BLOCK_NO_WIFI)
                    ESP_LOGW(TAG, "waiting for WiFi: %s%s",
                             have_ssid ? "connecting to " : "none stored, console: wifi SSID PASSWORD", have_ssid ? ssid : "");
                else
                    ESP_LOGW(TAG, "waiting for URLs, console: url add URL");
                logged = blocker;
            }
            retry_on_battery();
            wait_ms(1000);
            continue;
        }
        logged = BLOCK_NONE;

        int         drawn   = take_page(n);
        int64_t     t_start = esp_timer_get_time();
        page_entry  page    = urls[drawn];
        std::string url     = page.url;
        int         secs    = page.seconds > 0 ? page.seconds : CONFIG_B26_CYCLE_SECONDS;
        ESP_LOGI(TAG, "PAGE %d of %u: %s offset %d for %d s", drawn + 1, (unsigned)urls.size(), url.c_str(),
                 page.offset, secs);
        ESP_LOGI(TAG, "free internal %u, PSRAM %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        s_index = (drawn + 1) % n;

        set_state("render");
        leds_load(1, 8);
        render_stats st;
        std::string  err;
        bool         ok = render_page(url, page.offset, tones_frame(), st, err, render_cancel);
        if(ok && !render_tabs(urls, static_cast<size_t>(drawn), tones_frame()))
            ESP_LOGW(TAG, "tab bar did not render");
        if(ok)
            ESP_LOGI(TAG,
                     "rendered: html %u bytes, doc_h %d, offset %d, fetch %d ms, parse %d ms, layout %d ms, "
                     "images %d ms, draw %d ms of which dither %d ms, images %u refs %u in view %u failed, PSRAM low %u",
                     (unsigned)st.html_bytes, st.doc_h, st.offset, st.fetch_ms, st.parse_ms, st.layout_ms,
                     st.images_ms, st.draw_ms, st.dither_ms, (unsigned)st.images, (unsigned)st.img_in_view, (unsigned)st.img_fail,
                     (unsigned)st.psram_low);
        if(!ok)
        {
            {
                sel_guard g;
                s_loading = -1;
            }
            if(st.cancelled)
            {
                ESP_LOGI(TAG, "PAGE CANCELLED %s: another tab was chosen", url.c_str());
                continue;
            }
            ESP_LOGW(TAG, "PAGE FAILED %s: %s", url.c_str(), err.c_str());
            if(!s_power.usb)
            {
                if(!s_hold && ++fails < n) continue;
                fails = 0;
                read_power();
                if(!s_power.usb)
                {
                    rest_or_select(BROWSER_RETRY_S);
                    continue;
                }
            }
            retry_on_battery();
            set_state("wait");
            countdown(FAIL_WAIT_MS);
            continue;
        }

        // A selection still in its hold decides whether this page is drawn. Under the lock with the
        // answer, the page becomes s_drawing, so a press from here counts from it.
        browser_sel_action a;
        for(;;)
        {
            int64_t left_us;
            {
                sel_guard g;
                a = sel_check(drawn);
                if(a != BROWSER_SEL_WAIT)
                {
                    s_loading = -1;
                    if(a == BROWSER_SEL_NONE) s_drawing = drawn;
                    break;
                }
                left_us = s_sel.until - esp_timer_get_time();
            }
            wait_ms(std::max<int64_t>(1, (left_us + 999) / 1000));
        }
        if(a == BROWSER_SEL_LOAD)
        {
            ESP_LOGI(TAG, "PAGE SKIPPED %s: another tab was chosen", url.c_str());
            continue;
        }

        if(!s_power.usb) wifi_stop();
        bool drew = refresh();
        {
            sel_guard g;
            s_panel   = drew ? drawn : PANEL_NONE;
            s_drawing = -1;
        }
        fails = 0;
        read_power();
        ESP_LOGI(TAG, "PAGE DONE %s after %lld ms", url.c_str(), (esp_timer_get_time() - t_start) / 1000);
        if(!s_power.usb)
        {
            rest_or_select(browser_sleep_s(secs, (esp_timer_get_time() - t_cycle) / 1000));
            continue;
        }

        set_state("wait");
        int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;
        countdown(static_cast<int64_t>(secs) * 1000 - elapsed_ms);
    }
}
} // namespace

bool browser_was_low(bool kept)
{
    return kept && s_low != 0;
}

power_reading browser_init(bool kept, bool low)
{
    s_sel_lock = xSemaphoreCreateMutex();
    if(!kept)
    {
        s_index = 0;
        s_panel = PANEL_NONE;
        s_low   = 0;
        s_hold  = 0;
    }
    if(low) s_low = 1;
    read_power();
    return s_power;
}

bool browser_start()
{
    if(!s_sel_lock) return false;
    int n   = static_cast<int>(cfg_urls().size());
    s_count = n;
    leds_hold(s_hold != 0);
    power_wake wake = power_wake_cause();
    if(n > 0)
    {
        s_index = browser_wake_index(wake, s_index, s_panel, n);
        char ssid[33];
        if(wifi_get_ssid(ssid, sizeof(ssid)) && (wake == POWER_WAKE_USER_B || wake == POWER_WAKE_MENU_MINUS))
        {
            sel_guard g;
            sel_open(n, s_index, esp_timer_get_time() + SELECT_HOLD_US, true);
        } else leds_select_end();
    } else leds_select_end();
    BaseType_t rc = xTaskCreatePinnedToCore(browser_task, "browser", RENDER_STACK, nullptr, 5, &s_task, 1);
    if(rc != pdPASS) ESP_LOGE(TAG, "browser task not created");
    return rc == pdPASS;
}

namespace
{
// A selection from the console: it loads at once, also when it is the page shown.
void console_select(int target, bool from_base)
{
    if(!s_task) return;
    sel_guard g;
    int       n = s_count;
    if(n <= 0) return;
    sel_open(n, from_base ? base_page(n) + target : target, esp_timer_get_time(), true);
}
} // namespace

void browser_next()
{
    console_select(s_index, false);
}

void browser_show(browser_screen screen)
{
    s_show = screen;
    if(s_task) xTaskNotifyGive(s_task);
}

bool browser_shows(browser_screen screen)
{
    return s_panel == panel_of(screen);
}

void browser_prev()
{
    console_select(-1, true);
}

void browser_load(int index)
{
    console_select(index < 0 ? 0 : index, false);
}

int browser_step(int delta)
{
    if(!s_task) return -1;
    sel_guard g;
    int       n = s_count;
    if(n <= 0 || s_sleeping)
    {
        leds_flash();
        return -1;
    }
    s_presses++;
    sel_open(n, (s_sel.open ? s_sel.target : base_page(n)) + delta, esp_timer_get_time() + SELECT_HOLD_US, false);
    return s_sel.target;
}

int browser_hold_toggle()
{
    if(!s_task) return -1;
    sel_guard g;
    if(s_sleeping) return -1;
    s_presses++;
    s_hold = s_hold ? 0 : 1;
    leds_hold(s_hold != 0);
    xTaskNotifyGive(s_task);
    return s_hold;
}

int browser_page_index()
{
    return s_index;
}

const char* browser_state()
{
    return s_state.load();
}
