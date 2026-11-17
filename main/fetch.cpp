// HTTP GET into memory. The request runs as esp_http_client_open, esp_http_client_fetch_headers and
// a read loop, not esp_http_client_perform: perform's data callback cannot stop a transfer (its
// return value is ignored in IDF 6.0.2), and a byte cap has to stop the read. The read loop stops at
// the cap and only then drops the connection; a complete body leaves the connection open for the
// next request to the same host.

#include "fetch.h"

#include <cstdlib>
#include <cstring>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "url.h"

static const char* TAG = "fetch";

namespace
{
struct ctx
{
    std::string location;      // Location header of the current response
    int64_t     range_total = -1; // the total after the slash in Content-Range
    int64_t     t_connected = 0;  // esp_timer time, first hop
};

// Redirects are followed here, not by esp_http_client: its automatic redirect reuses the handle's
// transport, so a hop from https to http (c.xkcd.com/random/comic/ to http://xkcd.com/NNN/) would
// speak TLS to port 80.
const int    MAX_REDIRECTS = 8;
const size_t READ_CHUNK    = 4096;

// esp_http_client picks its transport (plain or TLS) on the first connect of a handle and keeps it
// through later esp_http_client_set_url calls, so a handle must only ever see one scheme.
// Index 0 is http, 1 is https. Each handle reuses its connection while the host stays the same.
esp_http_client_handle_t s_client[2];
bool                     s_live[2]; // the handle may hold an open connection from the last request
esp_http_client_config_t s_cfg;
char*                    s_buf;
// The handles are shared by the browser task and the console's get command. One fetch at a time.
SemaphoreHandle_t s_lock;

int scheme_index(const std::string& url) { return url.compare(0, 8, "https://") == 0 ? 1 : 0; }

esp_http_client_handle_t client_for(const std::string& url)
{
    int i = scheme_index(url);
    if(!s_client[i])
    {
        esp_http_client_config_t cfg = s_cfg;
        cfg.url                      = url.c_str();
        s_client[i]                  = esp_http_client_init(&cfg);
        if(!s_client[i]) ESP_LOGE(TAG, "esp_http_client_init failed for %s", url.c_str());
    }
    return s_client[i];
}

esp_err_t on_event(esp_http_client_event_t* evt)
{
    auto* c = static_cast<ctx*>(evt->user_data);
    if(!c) return ESP_OK;
    if(evt->event_id == HTTP_EVENT_ON_CONNECTED)
    {
        if(!c->t_connected) c->t_connected = esp_timer_get_time();
    } else if(evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value)
    {
        if(strcasecmp(evt->header_key, "Location") == 0) c->location = evt->header_value;
        else if(strcasecmp(evt->header_key, "Content-Range") == 0)
        {
            const char* slash = strchr(evt->header_value, '/');
            if(slash && slash[1] != '*') c->range_total = strtoll(slash + 1, nullptr, 10);
        }
    }
    return ESP_OK;
}

void drop(int i)
{
    if(s_client[i]) esp_http_client_close(s_client[i]);
    s_live[i] = false;
}

// One request on one URL. Returns ESP_OK with the status in res and the body (up to max_bytes) in out,
// or an error after closing the connection. range_n > 0 asks for the first range_n bytes.
esp_err_t one_request(const std::string& cur, std::string& out, size_t max_bytes, size_t range_n, ctx& c,
                      fetch_result* res, int64_t* t_first)
{
    int                      i      = scheme_index(cur);
    esp_http_client_handle_t client = client_for(cur);
    if(!client) return ESP_FAIL;
    out.clear();
    c.location.clear();
    c.range_total = -1;
    esp_http_client_set_user_data(client, &c);
    esp_err_t err = esp_http_client_set_url(client, cur.c_str());
    if(err == ESP_OK) err = esp_http_client_set_method(client, HTTP_METHOD_GET);
    if(err == ESP_OK)
    {
        if(range_n > 0)
        {
            char range[48];
            snprintf(range, sizeof(range), "bytes=0-%u", (unsigned)(range_n - 1));
            err = esp_http_client_set_header(client, "Range", range);
        } else
            esp_http_client_delete_header(client, "Range");
    }
    if(err != ESP_OK) return err;

    // A kept connection may have been closed by the server since: one retry on a fresh one.
    bool    retried = false;
    int64_t clen    = -1;
    for(;;)
    {
        err = esp_http_client_open(client, 0);
        if(err == ESP_OK)
        {
            clen = esp_http_client_fetch_headers(client);
            if(clen < 0) err = ESP_FAIL;
        }
        if(err == ESP_OK) break;
        bool was_live = s_live[i];
        drop(i);
        if(retried || !was_live) return err;
        retried = true;
    }
    s_live[i]   = true;
    res->status = esp_http_client_get_status_code(client);
    res->total  = c.range_total > 0 ? c.range_total : (res->status == 206 ? -1 : (clen > 0 ? clen : -1));

    for(;;)
    {
        if(out.size() >= max_bytes)
        {
            res->truncated = !esp_http_client_is_complete_data_received(client);
            break;
        }
        int want = static_cast<int>(std::min(READ_CHUNK, max_bytes - out.size()));
        int n    = esp_http_client_read(client, s_buf, want);
        if(n < 0)
        {
            drop(i);
            return ESP_FAIL;
        }
        if(n == 0) break; // body complete, or the server closed
        if(!*t_first) *t_first = esp_timer_get_time();
        out.append(s_buf, static_cast<size_t>(n));
    }
    if(res->truncated || !esp_http_client_is_complete_data_received(client))
    {
        // the rest of the body would arrive on the next request
        drop(i);
    }
    return ESP_OK;
}

bool do_fetch(const std::string& url, std::string& out, size_t max_bytes, size_t range_n, fetch_result* res)
{
    out.clear();
    fetch_result local;
    if(!res) res = &local;
    *res = fetch_result();
    if(url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) return false;
    struct guard
    {
        guard() { xSemaphoreTake(s_lock, portMAX_DELAY); }
        ~guard() { xSemaphoreGive(s_lock); }
    } lock;

    ctx         c;
    int64_t     t0 = esp_timer_get_time(), t_first = 0;
    std::string cur = url;
    esp_err_t   err = ESP_OK;
    for(int hop = 0;; hop++)
    {
        err = one_request(cur, out, max_bytes, range_n, c, res, &t_first);
        if(err != ESP_OK) break;
        if(res->status == 416 && range_n > 0)
        {
            // the server refuses ranges: once more without
            range_n = 0;
            drop(scheme_index(cur));
            err = one_request(cur, out, max_bytes, 0, c, res, &t_first);
            if(err != ESP_OK) break;
        }
        bool redirect = res->status == 301 || res->status == 302 || res->status == 303 || res->status == 307 ||
                        res->status == 308;
        if(!redirect || c.location.empty() || hop >= MAX_REDIRECTS) break;
        std::string next = resolve_url(cur, c.location);
        if(next.empty())
        {
            ESP_LOGW(TAG, "GET %s: %d to unsupported location %s", cur.c_str(), res->status, c.location.c_str());
            break;
        }
        ESP_LOGI(TAG, "GET %s: %d to %s", cur.c_str(), res->status, next.c_str());
        cur = next;
    }
    res->final_url = cur;
    res->partial   = res->status == 206 || res->truncated;
    int ms      = static_cast<int>((esp_timer_get_time() - t0) / 1000);
    int conn_ms = c.t_connected ? static_cast<int>((c.t_connected - t0) / 1000) : -1;
    int ttfb_ms = t_first ? static_cast<int>((t_first - t0) / 1000) : -1;
    if(err != ESP_OK)
    {
        ESP_LOGW(TAG, "GET %s: %s after %d ms (connect %d ms, first data %d ms, %u bytes so far)", url.c_str(),
                 esp_err_to_name(err), ms, conn_ms, ttfb_ms, (unsigned)out.size());
        res->status = 0;
        return false;
    }
    ESP_LOGI(TAG, "GET %s%s: %d, %u bytes%s, %d ms (connect %d ms, first data %d ms)", url.c_str(),
             range_n ? " [prefix]" : "", res->status, (unsigned)out.size(), res->truncated ? " (truncated)" : "", ms,
             conn_ms, ttfb_ms);
    return res->status >= 200 && res->status < 300;
}
} // namespace

void fetch_init()
{
    s_lock                      = xSemaphoreCreateMutex();
    s_buf                       = static_cast<char*>(malloc(READ_CHUNK));
    s_cfg                       = {};
    s_cfg.user_agent            = "Mozilla/5.0 (compatible; browser26/0.1; ESP32-S3; litehtml)";
    s_cfg.timeout_ms            = 5000;
    s_cfg.buffer_size           = 4096;
    s_cfg.buffer_size_tx        = 1024;
    s_cfg.disable_auto_redirect = true;
    s_cfg.event_handler         = on_event;
    s_cfg.keep_alive_enable     = true;
}

bool fetch_url(const std::string& url, std::string& out, size_t max_bytes, fetch_result* res)
{
    fetch_result local;
    if(!res) res = &local;
    bool ok = do_fetch(url, out, max_bytes, 0, res);
    return ok && !res->truncated;
}

bool fetch_prefix(const std::string& url, std::string& out, size_t n, fetch_result* res)
{
    fetch_result local;
    if(!res) res = &local;
    return do_fetch(url, out, n, n, res);
}
