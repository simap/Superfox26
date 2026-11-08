// LittleFS mount of the data partition and the page list.

#include "config.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "urls.h"

static const char* TAG = "config";

static const char* BASE  = CFG_BASE;
static const char* LABEL = "data";
static const char* URLS  = CFG_BASE "/urls.txt";

namespace
{
SemaphoreHandle_t       s_lock;
std::vector<page_entry> s_pages;
bool                    s_from_file = false;
bool                    s_mounted   = false;

struct lock_guard
{
    lock_guard() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~lock_guard() { xSemaphoreGive(s_lock); }
};

std::vector<page_entry> defaults()
{
    return std::vector<page_entry>(std::begin(PAGE_URLS), std::end(PAGE_URLS));
}

// "URL [OFFSET [SECONDS]]", blank lines and # comments skipped. False for a line without a URL.
bool parse_line(char* line, page_entry& e)
{
    char* save = nullptr;
    char* tok  = strtok_r(line, " \t\r\n", &save);
    if(!tok || *tok == '#') return false;
    e        = page_entry();
    e.url    = tok;
    if((tok = strtok_r(nullptr, " \t\r\n", &save))) e.offset = atoi(tok);
    if((tok = strtok_r(nullptr, " \t\r\n", &save))) e.seconds = atoi(tok);
    if(e.offset < 0) e.offset = 0;
    if(e.seconds < 0) e.seconds = 0;
    return true;
}

// Reads URLS into out. False when the file is missing or unreadable.
bool read_file(std::vector<page_entry>& out)
{
    FILE* f = fopen(URLS, "r");
    if(!f) return false;
    char line[512];
    while(fgets(line, sizeof(line), f))
    {
        page_entry e;
        if(parse_line(line, e)) out.push_back(e);
    }
    fclose(f);
    return true;
}

bool write_file(const std::vector<page_entry>& pages)
{
    FILE* f = fopen(URLS, "w");
    if(!f)
    {
        ESP_LOGE(TAG, "cannot write %s", URLS);
        return false;
    }
    bool ok = true;
    for(auto& e : pages)
        if(fprintf(f, "%s %d %d\n", e.url.c_str(), e.offset, e.seconds) < 0) ok = false;
    if(fclose(f) != 0) ok = false;
    return ok;
}
} // namespace

bool cfg_init()
{
    s_lock  = xSemaphoreCreateMutex();
    s_pages = defaults();

    // Formatting a blank partition gives a volume for urls.txt but no fonts: render_init then fails
    // and the log says so.
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = BASE;
    conf.partition_label        = LABEL;
    conf.format_if_mount_failed = true;
    esp_err_t err               = esp_vfs_littlefs_register(&conf);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "mount of '%s' failed: %s, using the built-in page list", LABEL, esp_err_to_name(err));
        return false;
    }
    s_mounted = true;
    size_t total = 0, used = 0;
    if(esp_littlefs_info(LABEL, &total, &used) == ESP_OK)
        ESP_LOGI(TAG, "mounted '%s' at %s: %u of %u bytes used", LABEL, BASE, (unsigned)used, (unsigned)total);

    std::vector<page_entry> v;
    if(read_file(v))
    {
        s_pages     = v;
        s_from_file = true;
        ESP_LOGI(TAG, "%u pages from %s", (unsigned)v.size(), URLS);
    } else
        ESP_LOGI(TAG, "no %s, %u built-in pages", URLS, (unsigned)s_pages.size());
    return true;
}

std::vector<page_entry> cfg_urls()
{
    lock_guard g;
    return s_pages;
}

bool cfg_urls_from_file()
{
    lock_guard g;
    return s_from_file;
}

bool cfg_urls_set(const std::vector<page_entry>& pages)
{
    lock_guard g;
    s_pages = pages;
    if(!s_mounted) return false;
    s_from_file = write_file(pages);
    return s_from_file;
}

bool cfg_urls_reset()
{
    lock_guard g;
    s_pages     = defaults();
    s_from_file = false;
    if(!s_mounted) return false;
    return remove(URLS) == 0 || errno == ENOENT;
}
