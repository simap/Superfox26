// Each command prints its reply to stdout as one or more lines.
// A reply line leads with OK when the command ran or ERR when it was rejected or could not run.

#include "console.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "browser.h"
#include "config.h"
#include "power.h"
#include "tones.h"
#include "wifi.h"

static const char* TAG = "console";

static const int LINE_MAX_BYTES = 1024;
static const int CONSOLE_STACK  = 16384;

namespace
{
std::vector<std::string> split(const char* line)
{
    std::vector<std::string> out;
    std::string              cur;
    bool                     in_word = false, quoted = false;
    for(const char* p = line; *p; p++)
    {
        char c = *p;
        if(c == '\\' && p[1])
        {
            cur += *++p;
            in_word = true;
        } else if(c == '"')
        {
            quoted  = !quoted;
            in_word = true;
        } else if((c == ' ' || c == '\t') && !quoted)
        {
            if(in_word) out.push_back(cur);
            cur.clear();
            in_word = false;
        } else
        {
            cur += c;
            in_word = true;
        }
    }
    if(in_word) out.push_back(cur);
    return out;
}

void cmd_help()
{
    printf("OK commands:\n"
           "OK   status\n"
           "OK   wifi [SSID [PASSWORD]] | wifi clear\n"
           "OK   scan\n"
           "OK   urls\n"
           "OK   url add URL [OFFSET [SECONDS]] | url put N URL [OFFSET [SECONDS]] | url set N OFFSET [SECONDS]\n"
           "OK   url move FROM TO | url del N | url clear\n"
           "OK   next | prev | load N\n"
           "OK   show lowbatt | show welcome\n"
           "OK   dither [diffuse | triangle | nearest] | dither solid T W | dither line T W\n");
}

// The names of the photo modes of tones.h, in their order.
const char* const DITHER_NAMES[TONE_PHOTO_MODES] = {"diffuse", "triangle", "nearest"};

void cmd_dither(const std::vector<std::string>& a)
{
    if(a.size() == 2)
        for(int m = 0; m < TONE_PHOTO_MODES; m++)
            if(a[1] == DITHER_NAMES[m])
            {
                tones_set_photo_mode(static_cast<tone_photo_mode>(m));
                printf("OK dither=%s from the next page drawn. load N draws page N again\n", DITHER_NAMES[m]);
                return;
            }
    int st, sw, lt, lw;
    tones_near_ease(&st, &sw, &lt, &lw);
    if(a.size() == 4 && (a[1] == "solid" || a[1] == "line"))
    {
        char*      e1 = nullptr;
        char*      e2 = nullptr;
        const long t  = strtol(a[2].c_str(), &e1, 10), w = strtol(a[3].c_str(), &e2, 10);
        if(*e1 == '\0' && *e2 == '\0' && t >= 0 && t <= 1024 && w >= 0 && w <= 1024)
        {
            if(a[1] == "solid") st = static_cast<int>(t), sw = static_cast<int>(w);
            else lt = static_cast<int>(t), lw = static_cast<int>(w);
            tones_set_near_ease(st, sw, lt, lw);
            printf("OK dither solid=%d %d line=%d %d from the next page drawn, until power off or deep sleep\n", st, sw,
                   lt, lw);
            return;
        }
    }
    if(a.size() > 1)
    {
        printf("ERR dither diffuse | dither triangle | dither nearest | dither solid T W | dither line T W (0 to 1024 "
               "each)\n");
        return;
    }
    printf("OK dither=%s solid=%d %d line=%d %d. Photo pixels: diffuse takes error diffusion, triangle the mix that "
           "keeps the mean colour, nearest the mix of the nearest tones. In nearest, solid T W: a target within T of "
           "its main tone takes that tone alone, linear again at T + W. line T W: a minor tone within T of the "
           "opposite edge takes no dots, linear again at T + W. In 1/4096 of L, 0 0 is off\n",
           DITHER_NAMES[tones_photo_mode()], st, sw, lt, lw);
}

void cmd_status()
{
    char ssid[33], ip[16];
    wifi_get_ssid(ssid, sizeof(ssid));
    wifi_get_ip(ip, sizeof(ip));
    int rssi = 0, channel = 0;
    bool link = wifi_get_link(&rssi, &channel);
    printf("OK wifi ssid=\"%s\" connected=%d ip=%s rssi=%d channel=%d radio=%s\n", ssid, wifi_is_connected() ? 1 : 0,
           ip, link ? rssi : 0, link ? channel : 0, wifi_is_stopped() ? "off" : "on");
    power_reading r   = power_read(false);
    int           mv  = r.rail_mv;
    bool          usb = r.usb;
    if(mv < 0)
        printf("OK power rail_mv=failed source=%s wake=\"%s\" (rail read failed%s)\n", usb ? "usb" : "battery",
               power_wake_name(power_wake_cause()), wifi_is_stopped() ? "" : ", the radio is on");
    else
        printf("OK power rail_mv=%d source=%s wake=\"%s\"\n", mv, usb ? "usb" : "battery",
               power_wake_name(power_wake_cause()));
    printf("OK urls count=%u source=%s\n", (unsigned)cfg_urls().size(), cfg_urls_from_file() ? "file" : "builtin");
    printf("OK page index=%d state=%s\n", browser_page_index(), browser_state());
    const esp_app_desc_t* app = esp_app_get_description();
    printf("OK app built=\"%s %s\" sha=", app->date, app->time);
    for(int i = 0; i < 8; i++) printf("%02x", app->app_elf_sha256[i]);
    printf("\n");
    printf("OK heap internal=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void cmd_wifi(const std::vector<std::string>& a)
{
    if(a.size() == 1)
    {
        char ssid[33];
        bool have = wifi_get_ssid(ssid, sizeof(ssid));
        printf("OK ssid=\"%s\" connected=%d\n", have ? ssid : "", wifi_is_connected() ? 1 : 0);
        return;
    }
    if(a.size() == 2 && a[1] == "clear")
    {
        if(wifi_clear_credentials()) printf("OK cleared. The welcome notice draws at the next cycle\n");
        else printf("ERR not cleared, the driver refused (see log)\n");
        return;
    }
    const char* pass = a.size() > 2 ? a[2].c_str() : "";
    if(wifi_set_credentials(a[1].c_str(), pass))
    {
        if(wifi_is_stopped())
            printf("OK stored \"%s\". The radio is off, it connects at the next page\n", a[1].c_str());
        else
            printf("OK stored, connecting to \"%s\". The log shows the result, status shows the state\n", a[1].c_str());
    }
    else printf("ERR not connecting: ssid up to 32 bytes, password up to 63, or the driver refused (see log)\n");
}

void cmd_scan()
{
    static wifi_scan_entry_t found[32];
    printf("OK scanning\n");
    fflush(stdout);
    int n = wifi_scan(found, 32);
    if(n < 0)
    {
        printf("ERR scan failed, see log\n");
        return;
    }
    printf("OK %d networks (2.4 GHz only)\n", n);
    for(int i = 0; i < n; i++)
        printf("OK ch %2d %4d dBm %-10s %s\n", found[i].channel, found[i].rssi, found[i].auth,
               found[i].ssid[0] ? found[i].ssid : "(hidden)");
}

void cmd_urls()
{
    auto v = cfg_urls();
    printf("OK %u pages (%s): N URL OFFSET SECONDS, seconds 0 = %d\n", (unsigned)v.size(),
           cfg_urls_from_file() ? "file" : "builtin", CONFIG_B26_CYCLE_SECONDS);
    for(size_t i = 0; i < v.size(); i++)
        printf("OK %u %s %d %d\n", (unsigned)i + 1, v[i].url.c_str(), v[i].offset, v[i].seconds);
}

void report_set(bool ok, size_t n)
{
    printf("%s %u pages\n", ok ? "OK" : "ERR not written, list is in RAM only:", (unsigned)n);
}

int page_index(const std::string& s, size_t count)
{
    int n = atoi(s.c_str());
    if(n < 1 || n > static_cast<int>(count))
    {
        printf("ERR index 1 to %u\n", (unsigned)count);
        return 0;
    }
    return n;
}

bool url_ok(const std::string& u)
{
    if(u.compare(0, 7, "http://") == 0 || u.compare(0, 8, "https://") == 0) return true;
    printf("ERR url must start with http:// or https://\n");
    return false;
}

void cmd_url(const std::vector<std::string>& a)
{
    if(a.size() >= 3 && a[1] == "add")
    {
        if(!url_ok(a[2])) return;
        auto       v = cfg_urls();
        page_entry e;
        e.url = a[2];
        if(a.size() > 3) e.offset = std::max(0, atoi(a[3].c_str()));
        if(a.size() > 4) e.seconds = std::max(0, atoi(a[4].c_str()));
        v.push_back(e);
        report_set(cfg_urls_set(v), v.size());
    } else if(a.size() >= 4 && a[1] == "set")
    {
        auto v = cfg_urls();
        int  n = page_index(a[2], v.size());
        if(!n) return;
        v[n - 1].offset = std::max(0, atoi(a[3].c_str()));
        if(a.size() > 4) v[n - 1].seconds = std::max(0, atoi(a[4].c_str()));
        report_set(cfg_urls_set(v), v.size());
    } else if(a.size() >= 4 && a[1] == "put")
    {
        auto v = cfg_urls();
        int  n = page_index(a[2], v.size());
        if(!n || !url_ok(a[3])) return;
        page_entry e;
        e.url = a[3];
        if(a.size() > 4) e.offset = std::max(0, atoi(a[4].c_str()));
        if(a.size() > 5) e.seconds = std::max(0, atoi(a[5].c_str()));
        v[n - 1] = e;
        report_set(cfg_urls_set(v), v.size());
    } else if(a.size() >= 4 && a[1] == "move")
    {
        auto v    = cfg_urls();
        int  from = page_index(a[2], v.size());
        if(!from) return;
        int to = page_index(a[3], v.size());
        if(!to) return;
        page_entry e = v[from - 1];
        v.erase(v.begin() + (from - 1));
        v.insert(v.begin() + (to - 1), e);
        report_set(cfg_urls_set(v), v.size());
    } else if(a.size() >= 3 && a[1] == "del")
    {
        auto v = cfg_urls();
        int  n = page_index(a[2], v.size());
        if(!n) return;
        v.erase(v.begin() + (n - 1));
        report_set(cfg_urls_set(v), v.size());
    } else if(a.size() >= 2 && a[1] == "clear")
    {
        printf("%s builtin list, %u pages\n", cfg_urls_reset() ? "OK" : "ERR file not removed,",
               (unsigned)cfg_urls().size());
    } else
        printf("ERR url add URL [OFFSET [SECONDS]] | url put N URL [OFFSET [SECONDS]] | url set N OFFSET [SECONDS] | "
               "url move FROM TO | url del N | url clear\n");
}

void cmd_page(const std::vector<std::string>& a)
{
    const std::string& c = a[0];
    int                n = 0;
    if(c == "load")
    {
        if(a.size() != 2)
        {
            printf("ERR load N\n");
            return;
        }
        n = page_index(a[1], cfg_urls().size());
        if(!n) return;
    }
    const char* state = browser_state();
    if(!strcmp(state, "nowifi")) printf("ERR waiting for WiFi, set it: wifi SSID PASSWORD\n");
    else if(!strcmp(state, "nourls")) printf("ERR no URLs: url add https://...\n");
    else if(!strcmp(state, "wait") || !strcmp(state, "hold"))
    {
        if(c == "prev") browser_prev();
        else if(c == "load") browser_load(n - 1);
        else browser_next();
        if(n) printf("OK page %d\n", n);
        else printf("OK %s page\n", c == "prev" ? "previous" : "next");
    } else
        printf("ERR busy: %s, the next page follows\n", state);
}

void run_line(const char* line)
{
    auto a = split(line);
    if(a.empty()) return;
    const std::string& c = a[0];
    if(c == "help") cmd_help();
    else if(c == "status") cmd_status();
    else if(c == "wifi") cmd_wifi(a);
    else if(c == "scan") cmd_scan();
    else if(c == "urls") cmd_urls();
    else if(c == "url") cmd_url(a);
    else if(c == "next" || c == "prev" || c == "load") cmd_page(a);
    else if(c == "dither") cmd_dither(a);
    else if(c == "show")
    {
        const char* state = browser_state();
        int         s     = 0;
        if(a.size() == 2 && a[1] == "lowbatt") s = BROWSER_SCREEN_LOWBATT;
        else if(a.size() == 2 && a[1] == "welcome") s = BROWSER_SCREEN_WELCOME;
        if(s == 0) printf("ERR show lowbatt | show welcome\n");
        else if(browser_shows(static_cast<browser_screen>(s)))
            printf("OK %s is on the panel already\n", a[1].c_str());
        else if(strcmp(state, "wait") && strcmp(state, "hold") && strcmp(state, "nowifi") && strcmp(state, "nourls"))
            printf("ERR busy: %s, try again when it ends\n", state);
        else
        {
            char ssid[33];
            bool connecting = !strcmp(state, "nowifi") && wifi_get_ssid(ssid, sizeof(ssid));
            browser_show(static_cast<browser_screen>(s));
            printf("OK drawing %s%s\n", a[1].c_str(), connecting ? " when the WiFi wait ends, up to 20 s" : "");
        }
    } else
        printf("ERR unknown command \"%s\", try help\n", c.c_str());
    fflush(stdout);
}

void console_task(void*)
{
    static char line[LINE_MAX_BYTES];
    size_t      n  = 0;
    const int   fd = fileno(stdin);
    for(;;)
    {
        char c;
        int  rc = read(fd, &c, 1);
        if(rc != 1)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if(c == '\n' || c == '\r')
        {
            fputs("\n", stdout);
            line[n] = 0;
            n       = 0;
            run_line(line);
        } else if(c == '\b' || c == 127)
        {
            if(n)
            {
                n--;
                fputs("\b \b", stdout);
            }
        } else if(static_cast<unsigned char>(c) >= 0x20 && n < sizeof(line) - 1)
        {
            line[n++] = c;
            fputc(c, stdout);
        }
        fflush(stdout);
    }
}
}

void console_start()
{
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);
    usb_serial_jtag_driver_config_t jtag = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&jtag));
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);

    if(xTaskCreate(console_task, "console", CONSOLE_STACK, nullptr, 3, nullptr) != pdPASS)
        ESP_LOGE(TAG, "console task not created");
}
