// Page cycle task. Waits are task-notification waits, so the console's "next" ends them early.
// The panel is drawn only for a rendered page and the low battery notice: a draw costs about 12 s, so
// status (no WiFi, no URLs, a failed page) goes to the log and the console's status command, and the
// last page stays on the panel.
//
// Power. A reading (read_power) decides USB or battery and low battery. BAT_SENS is on ADC2, which WiFi
// shares, so a battery result counts only when it was read with the radio off. Readings are taken in
// browser_init() before wifi_init(), after a panel draw, and at the end of a cycle without a page. On
// USB that last one waits until the reading before is BROWSER_USB_RECHECK_MS old. On USB the badge
// stays awake with the radio on, and the LEDs count down to the next page. On battery the radio is off
// during the panel draw, and the badge deep sleeps after the draw for the rest of the page time, and
// for BROWSER_RETRY_S after a cycle without a page. A wake restarts the firmware, and the first cycle
// takes its page from the wake cause. A low battery shows a notice and sleeps until a button wakes the
// badge. The decisions are in browser_logic.h.

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
#include "freertos/task.h"
#include "sdkconfig.h"

#include "browser_logic.h"
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

// Layout plus the TLS handshakes inside createFromString. High-water mark measured at 15.2 KB for
// layout alone on Wikipedia; TLS adds about 10 KB. The stack is internal RAM: with it in PSRAM,
// HTTP fetches from this task stalled (connections opened, then no data for the whole timeout)
// while the same fetch from an internal-stack task completed in 0.7 s.
static const uint32_t RENDER_STACK = 40 * 1024;
// On USB, the pause after a failed page before the next one.
static const int64_t FAIL_WAIT_MS = 10 * 1000;
// A button selection loads once no button has been pressed for this long.
static const int64_t SELECT_HOLD_MS = 1500;
// The amber flash on the first LED after a failed WiFi connect. The same length as the red fault flash
// of main/main.cpp.
static const int WIFI_FAIL_FLASH_MS = 50;

// The low battery notice, drawn by render_html() with no network. The renderer has no SVG, so the
// battery icon is boxes: a 360 x 180 px body with a 12 px black border and a red charge bar, and a
// black terminal centred on its right side.
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

// The welcome notice, drawn when no WiFi network is stored. welcome_html() puts the logo between
// the two parts.
static const char* const WELCOME_HEAD =
    "<html><body style=\"margin:0;padding:120px 80px;font-family:sans-serif;color:#000;background:#fff\">"
    "<div style=\"font-size:96px;font-weight:bold\">"
    "<img style=\"width:100px;height:100px;vertical-align:middle;margin-right:32px\" src=\"";
static const char* const WELCOME_TAIL =
    "\">Welcome</div>"
    "<div style=\"font-size:44px;margin-top:56px\">WiFi is not set up.</div>"
    "<div style=\"font-size:44px;margin-top:28px\">Connect USB and press User B.</div>"
    "<div style=\"font-size:44px;margin-top:28px\">Then on the serial console:</div>"
    "<div style=\"font-size:44px;margin-top:28px;font-family:monospace\">wifi SSID PASSWORD</div>"
    "</body></html>";

// logo/panel_100.png (EMBED_FILES in CMakeLists.txt).
extern const uint8_t logo_100_start[] asm("_binary_panel_100_png_start");
extern const uint8_t logo_100_end[] asm("_binary_panel_100_png_end");

static std::string welcome_html()
{
    return std::string(WELCOME_HEAD) + tabs_png_url(logo_100_start, logo_100_end) + WELCOME_TAIL;
}

namespace
{
TaskHandle_t        s_task;
volatile bool       s_back  = false; // browser_prev() was called: step back two from the advanced index
volatile int        s_count = 1;     // pages in the list, for the button selection to wrap
volatile int        s_select = -1;   // tab chosen with the buttons, -1 none
volatile int64_t    s_select_until = 0; // when the selection loads, esp_timer time
std::atomic<int>    s_pending{0};       // tab steps from presses outside a selection, not yet applied
std::atomic<int>    s_show{0};          // browser_screen asked for by browser_show(), 0 none
const char* volatile s_state = "wait";

// RTC slow memory, not initialised at boot. Deep sleep, a software reset and a panic restart keep it.
// The magic word of main/rtcstate.cpp tells whether it is valid. browser_init() clears it when it is not.
// A change of these needs a new layout version in main/rtcstate.cpp. Each is one 32 bit word.
RTC_NOINIT_ATTR volatile int s_index; // the next page, an index into the URL list
RTC_NOINIT_ATTR volatile int s_panel; // what the panel shows: a page index, 0 and up, or a PANEL_ value
RTC_NOINIT_ATTR volatile int s_low;   // 1: the last reading was low battery

// Sets the cycle state and prints an event line on a change. A Web Serial page follows these
// lines, so it needs no status poll during a load or a draw. index is the status command's.
void set_state(const char* state)
{
    if(!strcmp(state, s_state)) return;
    s_state = state;
    printf("EVT state=%s index=%d\n", state, static_cast<int>(s_index));
    fflush(stdout);
}

// s_panel values other than a page. All are negative, so browser_wake_index() reads them as no page.
const int PANEL_NONE    = -1; // unknown: power on, or a draw that failed
const int PANEL_LOWBATT = -2; // the low battery notice
const int PANEL_WELCOME = -3; // the welcome notice

int panel_of(browser_screen screen)
{
    return screen == BROWSER_SCREEN_LOWBATT ? PANEL_LOWBATT : PANEL_WELCOME;
}

// The last reading, taken with the radio off, and its esp_timer time.
power_reading s_power = {-1, true, false, false};
int64_t       s_power_us;

// Takes a reading. ADC2 serves WiFi first, so a read with the radio on can fail, and a failed read counts
// as USB. A battery result taken with the radio on is therefore read again with the radio off. So is a
// failed read with no USB host connected, where nothing but the failure says USB: without this an
// unplugged badge whose reads fail with the radio on would stay awake. On return the radio is off when
// the result is battery and on when it is USB.
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
    set_state("sleep");
    wifi_stop();
    power_sleep(seconds);
}

// True when the panel draw succeeded.
bool refresh()
{
    set_state("refresh");
    tone_draw_stats ds;
    // A reset between these two marks halts the next boot (rtcstate.h).
    rtcstate_draw_begin();
    bool ok = tones_draw(tones_frame(), &ds);
    rtcstate_draw_end();
    ESP_LOGI(TAG, "draw %s, %u + %u pulls, %u ms", ok ? "ok" : "failed", (unsigned)ds.pulls1, (unsigned)ds.pulls2,
             (unsigned)ds.total_ms);
    return ok;
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
    // Presses made during the render and the draw open the selection now, from the page shown.
    int pend = s_pending.exchange(0);
    int n    = s_count > 0 ? s_count : 1;
    if(pend)
    {
        s_select       = ((s_index - 1 + pend) % n + n) % n;
        s_select_until = esp_timer_get_time() + SELECT_HOLD_MS * 1000;
    } else s_select = -1;
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

// Draws a notice and records it in s_panel. s_panel is written before the draw starts, as for every
// notice: a brownout or reset during the draw then finds the notice recorded, and the next boot does
// not start another draw.
// After a failed render or draw s_panel is PANEL_NONE, so the next check draws the notice again.
bool draw_notice(int panel, const char* html)
{
    s_panel = panel;
    if(render_html(html, tones_frame()) && refresh()) return true;
    s_panel = PANEL_NONE;
    ESP_LOGE(TAG, "notice %d did not draw", panel);
    return false;
}

// Low battery, from the last reading. Draws the notice when it is not on the panel, then sleeps until
// a button wakes the badge. Returns when the battery is not low.
void low_battery_step()
{
    browser_low_step step = browser_low_next(s_power.low, s_panel == PANEL_LOWBATT);
    s_low                 = step.low ? 1 : 0;
    if(step.action == BROWSER_RUN) return;
    wifi_stop();
    if(step.action == BROWSER_LOW_DRAW)
    {
        set_state("lowbatt");
        ESP_LOGW(TAG, "battery low, drawing the notice");
        draw_notice(PANEL_LOWBATT, LOW_BATTERY_HTML);
    }
    sleep_now(0);
}

// No WiFi network is stored. Draws the welcome notice when it is not on the panel. On battery nothing
// can change until the user connects USB and presses a button, so the badge then sleeps until a button
// wakes it. On USB it returns and the cycle waits for the console.
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
    if(!s_power.usb) sleep_now(0);
}

// A notice asked for by browser_show(). Draws it when it is not on the panel, then holds it for
// CONFIG_B26_CYCLE_SECONDS.
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

// After a wake by a button: the tab selection of the countdown, before the page loads. start is the
// tab the wake button chose. Every button steps the selection while it is open, the LEDs show the tab,
// and it closes once the buttons have been quiet for SELECT_HOLD_MS. WiFi connects in this time.
void select_window(int start, int n)
{
    s_count        = n;
    s_select       = start;
    s_select_until = esp_timer_get_time() + SELECT_HOLD_MS * 1000;
    set_state("wait"); // browser_step() acts in this state only
    for(;;)
    {
        int64_t now = esp_timer_get_time();
        if(now >= s_select_until) break;
        leds_select(s_select, s_count);
        wait_ms(std::max<int64_t>(1, (s_select_until - now) / 1000));
    }
}

// Turns the radio on when it is off and waits up to BROWSER_WIFI_WAIT_MS for a connection.
bool wifi_connect()
{
    wifi_resume();
    if(wifi_is_connected()) return true;
    set_state("nowifi");
    leds_off();
    return wifi_wait_connected(BROWSER_WIFI_WAIT_MS);
}

// A cycle without a page. A new reading decides, on battery always, on USB when the last reading is
// older than BROWSER_USB_RECHECK_MS: battery sleeps BROWSER_RETRY_S, USB returns. Otherwise it returns
// at once.
void retry_on_battery()
{
    if(s_power.usb && esp_timer_get_time() - s_power_us < BROWSER_USB_RECHECK_MS * 1000LL) return;
    read_power();
    if(!s_power.usb) sleep_now(BROWSER_RETRY_S);
}

void browser_task(void*)
{
    const char* logged = nullptr; // blocker already logged, so it is logged once per change
    bool        woke   = true;    // the next cycle with a list is the first since the start
    int         fails  = 0;       // pages that failed since the last draw
    // The first cycle starts at boot, so a battery sleep after it counts the boot time.
    for(int64_t t_cycle = 0;; t_cycle = esp_timer_get_time())
    {
        low_battery_step();
        show_step();

        std::vector<page_entry> urls = cfg_urls();
        int                     n    = static_cast<int>(urls.size());
        char ssid[33];
        bool have_ssid = wifi_get_ssid(ssid, sizeof(ssid));
        if(!have_ssid) welcome_step();
        if(n > 0 && woke)
        {
            power_wake wake = power_wake_cause();
            s_index         = browser_wake_index(wake, s_index, s_panel, n);
            woke            = false;
            if(have_ssid && (wake == POWER_WAKE_USER_B || wake == POWER_WAKE_MENU_MINUS)) select_window(s_index, n);
        }
        const char* blocker = nullptr;
        if(n == 0) blocker = "nourls";
        // With no SSID a connect wait could only time out.
        else if(!have_ssid || !wifi_connect()) blocker = "nowifi";
        if(blocker)
        {
            set_state(blocker);
            // The fixed delay keeps the flash its full length when the console's "next" ends a wait.
            if(blocker == std::string("nowifi") && have_ssid)
            {
                leds_amber();
                vTaskDelay(pdMS_TO_TICKS(WIFI_FAIL_FLASH_MS));
            }
            leds_off();
            if(logged != blocker)
            {
                if(blocker == std::string("nowifi"))
                    ESP_LOGW(TAG, "waiting for WiFi: %s%s",
                             have_ssid ? "connecting to " : "none stored, console: wifi SSID PASSWORD", have_ssid ? ssid : "");
                else
                    ESP_LOGW(TAG, "waiting for URLs, console: url add https://...");
                logged = blocker;
            }
            retry_on_battery();
            wait_ms(1000);
            continue;
        }
        logged = nullptr;

        int64_t t_start = esp_timer_get_time();
        s_count = n;
        if(s_index < 0 || s_index >= n) s_index = 0;
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

        // The index moves on before the render. A page that crashes the firmware is then passed over
        // after the reset, which continues from the kept index.
        int drawn = s_index;
        s_index   = (s_index + 1) % n;

        set_state("render");
        leds_load(1, 8); // fetch starts; render_page advances the bar from here
        render_stats st;
        std::string  err;
        bool         ok = render_page(url, page.offset, tones_frame(), st, err);
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
            // the previous page stays on the panel
            ESP_LOGW(TAG, "PAGE FAILED %s: %s", url.c_str(), err.c_str());
            if(!s_power.usb)
            {
                // On battery the next page loads at once. When every page of the list has failed since
                // the last draw, a new reading decides: battery sleeps BROWSER_RETRY_S.
                if(++fails < n) continue;
                fails = 0;
                read_power();
                if(!s_power.usb) sleep_now(BROWSER_RETRY_S);
            }
            retry_on_battery();
            set_state("wait");
            countdown(FAIL_WAIT_MS);
            continue;
        }
        // On battery the draw needs no radio. On USB the radio stays on.
        if(!s_power.usb) wifi_stop();
        s_panel = refresh() ? drawn : PANEL_NONE;
        fails = 0;
        read_power();
        ESP_LOGI(TAG, "PAGE DONE %s after %lld ms", url.c_str(), (esp_timer_get_time() - t_start) / 1000);
        if(!s_power.usb)
        {
            // Presses made during the render and the draw open the selection, and the chosen page loads
            // before any sleep. With no press the badge sleeps.
            int pend = s_pending.exchange(0);
            if(pend == 0) sleep_now(browser_sleep_s(secs, (esp_timer_get_time() - t_cycle) / 1000));
            select_window(((drawn + pend) % n + n) % n, n);
            continue;
        }

        set_state("wait");
        int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;
        countdown(static_cast<int64_t>(secs) * 1000 - elapsed_ms);
    }
}
} // namespace

power_reading browser_init(bool kept)
{
    if(!kept)
    {
        s_index = 0;
        s_panel = PANEL_NONE;
        s_low   = 0;
    }
    read_power();
    return s_power;
}

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
    s_back = true;
    if(s_task) xTaskNotifyGive(s_task);
}

// A selection that is already due: the countdown ends at once and the cycle takes the page.
void browser_load(int index)
{
    if(!s_task) return;
    s_select       = index < 0 ? 0 : index;
    s_select_until = esp_timer_get_time();
    xTaskNotifyGive(s_task);
}

void browser_step(int delta)
{
    if(!s_task) return;
    if(strcmp(s_state, "wait") != 0)
    {
        // No selection is open during a render or a draw. The press is kept for when the draw ends.
        s_pending += delta;
        return;
    }
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
