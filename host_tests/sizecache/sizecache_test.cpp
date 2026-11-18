// Host test of main/sizecache.cpp and main/rtcstate.cpp. The test includes both source files, so it
// reads and writes the RTC table and the magic word the way a reboot or an interrupted put leaves them.
// Usage: sizecache_test. Exit status 0 when every check passes. The hit rates at the end are
// information, not checks.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rtcstate.cpp"
#include "sizecache.cpp"

static uint32_t s_rand = 0x2545F491;

uint32_t esp_random(void)
{
    s_rand ^= s_rand << 13;
    s_rand ^= s_rand >> 17;
    s_rand ^= s_rand << 5;
    return s_rand;
}

namespace
{
unsigned s_fails = 0;

void check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if(!ok) s_fails++;
}

// Entries in use.
int count()
{
    int n = 0;
    for(auto& set : s_table)
        for(auto& e : set)
            if(in_use(e)) n++;
    return n;
}

// URL i of a mix of four URL shapes seen on real pages.
std::string url_of(int i)
{
    char buf[200];
    switch(i % 4)
    {
    case 0:
        snprintf(buf, sizeof buf, "https://upload.wikimedia.org/wikipedia/commons/thumb/%x/%02x/Photo_%d.jpg/320px-Photo_%d.jpg",
                 i & 15, i & 255, i, i);
        break;
    case 1: snprintf(buf, sizeof buf, "https://cdn.example.com/assets/img/%04d.png", i); break;
    case 2: snprintf(buf, sizeof buf, "https://news.example.org/images/2026/11/%d/story-%d-hero.webp?w=640", i % 30 + 1, i); break;
    default: snprintf(buf, sizeof buf, "https://i.example.net/%08x.gif", static_cast<unsigned>(i * 2654435761u)); break;
    }
    return buf;
}

int set_index(const std::string& url)
{
    return static_cast<int>(fnv1a64(url) & (SETS - 1));
}

// The boot sequence of main/main.cpp.
void boot()
{
    bool kept = rtcstate_begin();
    sizecache_init(kept);
    rtcstate_commit();
}

// A power on: random bits in RTC memory.
void power_on()
{
    s_magic = RTCSTATE_MAGIC ^ 0x5A5A5A5A;
    for(size_t i = 0; i < sizeof(s_table); i++) reinterpret_cast<uint8_t*>(s_table)[i] = static_cast<uint8_t>(esp_random());
    boot();
}

bool has(const std::string& url, int kind, int w, int h)
{
    int k = -1, ww = -1, hh = -1;
    return sizecache_get(url, k, ww, hh) && k == kind && ww == w && hh == h;
}

bool absent(const std::string& url)
{
    int k = 0, w = 0, h = 0;
    return !sizecache_get(url, k, w, h);
}

// 4 URLs for each of the 128 sets, by set.
std::vector<std::vector<std::string>> even_urls()
{
    std::vector<std::vector<std::string>> sets(SETS);
    int                                   full = 0;
    for(int i = 0; full < SETS; i++)
    {
        auto  u = url_of(i);
        auto& s = sets[set_index(u)];
        if(s.size() < WAYS)
        {
            s.push_back(u);
            if(s.size() == WAYS) full++;
        }
    }
    return sets;
}

// Kind, w and h stored for URL j of set s in the even fill.
int kind_of(int s, int j) { return 1 + (s + j) % 5; }
int w_of(int s, int j) { return 1 + s * WAYS + j; }
int h_of(int s, int j) { return 65535 - (s * WAYS + j); }

void fill_even(const std::vector<std::vector<std::string>>& sets)
{
    for(int s = 0; s < SETS; s++)
        for(int j = 0; j < WAYS; j++) sizecache_put(sets[s][j], kind_of(s, j), w_of(s, j), h_of(s, j));
}

bool all_even_present(const std::vector<std::vector<std::string>>& sets)
{
    for(int s = 0; s < SETS; s++)
        for(int j = 0; j < WAYS; j++)
            if(!has(sets[s][j], kind_of(s, j), w_of(s, j), h_of(s, j))) return false;
    return true;
}

void test_basic()
{
    power_on();
    check(count() == 0, "power on gives an empty cache");
    std::string a = "https://example.com/a.png";
    sizecache_put(a, 1, 640, 480);
    check(has(a, 1, 640, 480), "put then get returns kind, w, h");
    check(absent("https://example.com/b.png"), "an unknown URL misses");
    sizecache_put(a, 2, 800, 600);
    check(has(a, 2, 800, 600) && count() == 1, "a changed size replaces the entry");

    std::string m = "https://example.com/max.bmp";
    sizecache_put(m, 127, 65535, 65535);
    check(has(m, 127, 65535, 65535), "kind 127, w and h 65535 round trip");
    std::string n = "https://example.com/min.gif";
    sizecache_put(n, 1, 1, 1);
    check(has(n, 1, 1, 1), "kind 1, w and h 1 round trip");
    std::string o = "https://example.com/odd.webp";
    sizecache_put(o, 5, 0x8001, 0x7FFE);
    check(has(o, 5, 0x8001, 0x7FFE), "w 0x8001 and h 0x7FFE round trip");

    int before = count();
    sizecache_put("https://example.com/r1", 0, 10, 10);
    sizecache_put("https://example.com/r2", 128, 10, 10);
    sizecache_put("https://example.com/r3", 1, 0, 10);
    sizecache_put("https://example.com/r4", 1, 10, -1);
    sizecache_put("https://example.com/r5", 1, 65536, 10);
    sizecache_put("https://example.com/r6", 1, 10, 65536);
    check(count() == before && absent("https://example.com/r1") && absent("https://example.com/r2") &&
              absent("https://example.com/r6"),
          "put rejects kind 0 and 128, w or h outside 1 to 65535");

    entry snap[SETS][WAYS];
    memcpy(snap, s_table, sizeof(snap));
    int k, w, h;
    sizecache_get(a, k, w, h);
    sizecache_get("https://example.com/none", k, w, h);
    sizecache_put(a, 2, 800, 600);
    check(memcmp(snap, s_table, sizeof(snap)) == 0, "a hit, a miss and a put of the same values write nothing");
}

void test_even_fill()
{
    auto sets = even_urls();
    power_on();
    fill_even(sets);
    check(count() == SETS * WAYS, "512 entries spread evenly fit, count 512");
    check(all_even_present(sets), "all 512 entries return their kind, w, h");

    // A fifth URL of one set replaces one entry of that set and nothing else.
    const int   ks = 37;
    std::string extra;
    for(int i = 100000; extra.empty(); i++)
        if(set_index(url_of(i)) == ks) extra = url_of(i);
    entry snap[SETS][WAYS];
    memcpy(snap, s_table, sizeof(snap));
    sizecache_put(extra, 3, 123, 456);
    bool others = true;
    for(int s = 0; s < SETS; s++)
        if(s != ks && memcmp(snap[s], s_table[s], sizeof(snap[s])) != 0) others = false;
    int changed = 0, kept = 0;
    for(int j = 0; j < WAYS; j++)
    {
        if(memcmp(&snap[ks][j], &s_table[ks][j], sizeof(entry)) != 0) changed++;
        if(has(sets[ks][j], kind_of(ks, j), w_of(ks, j), h_of(ks, j))) kept++;
    }
    check(others, "a put into a full set leaves the other 127 sets unchanged");
    check(changed == 1 && kept == 3 && has(extra, 3, 123, 456) && count() == SETS * WAYS,
          "a put into a full set replaces one of its 4 entries, count stays 512");

    // Further new URLs of the same set: every way is a victim at some point.
    bool way_hit[WAYS] = {};
    int  added         = 0;
    for(int i = 200000; added < 40; i++)
    {
        auto u = url_of(i);
        if(set_index(u) != ks) continue;
        memcpy(snap, s_table, sizeof(snap));
        sizecache_put(u, 1, 10, 10);
        for(int j = 0; j < WAYS; j++)
            if(memcmp(&snap[ks][j], &s_table[ks][j], sizeof(entry)) != 0) way_hit[j] = true;
        added++;
    }
    check(way_hit[0] && way_hit[1] && way_hit[2] && way_hit[3], "40 puts into a full set pick each of the 4 ways");
}

void test_reboot()
{
    auto sets = even_urls();
    power_on();
    fill_even(sets);
    entry snap[SETS][WAYS];
    memcpy(snap, s_table, sizeof(snap));
    boot();
    check(count() == SETS * WAYS && all_even_present(sets) && memcmp(snap, s_table, sizeof(snap)) == 0,
          "right magic: a second boot keeps all 512 entries and writes nothing to the table");

    s_magic ^= 0x80000000u;
    check(!rtcstate_begin() && s_magic != RTCSTATE_MAGIC, "wrong magic: begin returns false and leaves it wrong");
    sizecache_init(false);
    check(count() == 0 && absent(sets[0][0]), "wrong magic: init gives an empty cache");
    rtcstate_commit();
    check(rtcstate_begin(), "commit makes the magic right");

    fill_even(sets);
    s_magic = 0;
    memset(s_table, 0, sizeof(s_table) / 2); // a reset in the middle of the clear
    boot();
    check(count() == 0, "a reset during the clear gives another clear on the next boot");
}

// Each prefix of the 5 stores of a put to entry e, from old to n: key_lo = 0, w, h, key_hi, key_lo.
void store_prefix(entry& e, const entry& n, int steps)
{
    if(steps >= 1) e.key_lo = 0;
    if(steps >= 2) e.w = n.w;
    if(steps >= 3) e.h = n.h;
    if(steps >= 4) e.key_hi = n.key_hi;
    if(steps >= 5) e.key_lo = n.key_lo;
}

void test_interrupted_put()
{
    // The put of main/sizecache.cpp, step by step, for a changed size on a known URL and for a new URL
    // into a slot that held another URL. After each store the slot is the old entry, empty or the new
    // entry, and get never returns one URL with another size.
    std::string a = "https://example.com/a.png";
    std::string b;
    for(int i = 0; b.empty(); i++)
        if(set_index(url_of(i)) == set_index(a)) b = url_of(i);
    bool ok = true;
    for(int same = 0; same < 2; same++)
        for(int steps = 0; steps <= 5; steps++)
        {
            power_on();
            sizecache_put(a, 2, 300, 200);
            entry* e = nullptr;
            for(auto& x : s_table[set_index(a)])
                if(in_use(x)) e = &x;
            std::string url  = same ? a : b;
            uint64_t    hash = fnv1a64(url);
            entry       n;
            n.key_lo = (static_cast<uint32_t>(hash) & ~KIND_MASK) | 4;
            n.key_hi = static_cast<uint32_t>(hash >> 32);
            n.w      = 1000;
            n.h      = 700;
            store_prefix(*e, n, steps);
            int  k, w, h;
            bool hit_a = sizecache_get(a, k, w, h);
            bool a_old = hit_a && k == 2 && w == 300 && h == 200;
            bool a_new = hit_a && same && k == 4 && w == 1000 && h == 700;
            bool hit_b = sizecache_get(b, k, w, h);
            bool b_new = hit_b && !same && k == 4 && w == 1000 && h == 700;
            bool want_old = steps == 0, want_new = steps == 5;
            if(want_old) ok = ok && a_old && !hit_b && count() == 1;
            else if(want_new) ok = ok && (same ? a_new && !hit_b : b_new && !hit_a) && count() == 1;
            else ok = ok && !hit_a && !hit_b && count() == 0;
            // An empty slot left by the interrupted put is taken by the next put.
            if(!want_old && !want_new)
            {
                sizecache_put(b, 3, 50, 60);
                ok = ok && has(b, 3, 50, 60) && count() == 1;
            }
        }
    check(ok, "a put interrupted after each of its stores leaves the slot old, empty or complete");
}

// Every page of a cycle of n distinct URLs: get, put on a miss. Hit rate over cycles 4 to 13.
void hit_rate(int n)
{
    power_on();
    int load[SETS] = {};
    for(int i = 0; i < n; i++) load[set_index(url_of(i))]++;
    int over = 0, most = 0;
    for(int s = 0; s < SETS; s++)
    {
        if(load[s] > WAYS) over++;
        if(load[s] > most) most = load[s];
    }
    long hits = 0, gets = 0;
    for(int c = 1; c <= 13; c++)
        for(int i = 0; i < n; i++)
        {
            auto u = url_of(i);
            int  k, w, h;
            bool hit = sizecache_get(u, k, w, h);
            if(!hit) sizecache_put(u, 1 + i % 5, 100 + i, 50 + i);
            if(c >= 4)
            {
                gets++;
                if(hit) hits++;
            }
        }
    printf("info  %4d URLs: hit rate %.1f %% over cycles 4 to 13, %d of 128 sets hold more than 4 URLs, most %d\n", n,
           100.0 * hits / gets, over, most);
}
} // namespace

int main()
{
    test_basic();
    test_even_fill();
    test_reboot();
    test_interrupted_put();
    hit_rate(600);
    hit_rate(768);
    hit_rate(1024);
    printf("%u failed\n", s_fails);
    return s_fails ? 1 : 0;
}
