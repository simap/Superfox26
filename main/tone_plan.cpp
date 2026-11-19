#include "tone_plan.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "tone_tables.h"

namespace
{
bool s_ready = false;

bool fail(char* why, size_t why_len, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

bool fail(char* why, size_t why_len, const char* fmt, ...)
{
    if(why && why_len)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, why_len, fmt, ap);
        va_end(ap);
    }
    return false;
}

unsigned mag(int v)
{
    return static_cast<unsigned>(v < 0 ? -v : v);
}

unsigned max_u(unsigned a, unsigned b)
{
    return a > b ? a : b;
}

uint8_t letter_code(const char* letters, char c)
{
    return static_cast<uint8_t>(strchr(letters, c) - letters);
}

uint8_t p1_part_code(const tone_seq& s, unsigned j)
{
    const unsigned pre = mag(s.prefix);
    if(j < pre) return s.prefix > 0 ? TONE_P1_I : TONE_P1_P;
    j -= pre;
    return letter_code("SIPM", j < s.n1a ? s.p1a[j] : s.p1b[j - s.n1a]);
}

bool part_ok(const char* part, unsigned n, const char* letters, int (*net_of)(char), int* net)
{
    if(n > TONE_SEQ_MAX || part[n] != '\0') return false;
    for(unsigned k = 0; k < n; k++)
    {
        if(part[k] == '\0' || !strchr(letters, part[k])) return false;
        *net += net_of(part[k]);
    }
    return true;
}
}

bool tone_plan_init(char* why, size_t why_len)
{
    s_ready = false;
    const unsigned head = tone_s_head_vdh();
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
    {
        const tone_seq&  s   = tone_tables::seq[e];
        const tone_push& u   = tone_tables::push[e];
        int              net = 0;
        if(!part_ok(s.p1a, s.n1a, "IP", tone_p1_net, &net) || !part_ok(s.p1b, s.n1b, "SM", tone_p1_net, &net) ||
           !part_ok(s.p2, s.n2, "hpiw", tone_p2_net, &net))
            return fail(why, why_len, "entry %u: a part's length or letters do not match", e);
        if(s.split < -1 || s.split > s.n2)
            return fail(why, why_len, "entry %u: split %d is outside -1 to %u", e, s.split, s.n2);
        const tone_balance b = tone_balance_of(net);
        if(net != s.net || b.prefix != s.prefix || b.nudge != s.nudge || b.residual != s.residual)
            return fail(why, why_len, "entry %u: net %d prefix %d nudge %d residual %d, the letters give %d %d %d %d",
                        e, s.net, s.prefix, s.nudge, s.residual, net, b.prefix, b.nudge, b.residual);
        if(tone_push_over(u))
            return fail(why, why_len,
                        "entry %u: runs of %u VDH, %u VDL, %u VDHR, %u positive frames, the limits are %u, %u, %u", e,
                        u.max_vdh, u.max_vdl, u.max_vdhr, u.max_pos, static_cast<unsigned>(EPD_PUSH_MAX_VDH),
                        static_cast<unsigned>(EPD_PUSH_MAX_VDL), static_cast<unsigned>(EPD_PUSH_MAX_VDHR));
        if(u.vdh + head > EPD_PUSH_MAX_VDH || u.pos + head > EPD_PUSH_MAX_VDH)
            return fail(why, why_len,
                        "entry %u: open runs of %u VDH and %u positive frames, with the S head of %u, over %u", e,
                        u.vdh, u.pos, head, static_cast<unsigned>(EPD_PUSH_MAX_VDH));
    }
    s_ready = true;
    return true;
}

bool tone_plan_build(const uint32_t count[TONEMAP_COUNT], tone_plan* plan)
{
    *plan = tone_plan();
    if(!s_ready) return false;
    unsigned max_a = 0, max_b = 0, len2 = 0;
    for(unsigned e = 0; e < TONEMAP_COUNT; e++)
    {
        if(count[e] == 0) continue;
        const tone_seq& s = tone_tables::seq[e];
        max_a             = max_u(max_a, mag(s.prefix) + s.n1a);
        max_b             = max_u(max_b, s.n1b);
        len2              = max_u(len2, mag(s.nudge) + s.n2);
    }
    if(max_a + max_b + len2 > 0)
    {
        plan->mid                = max_a + 1u;
        plan->len[TONE_PHASE_1] = plan->mid + max_b;
        if(max_a + max_b > 0 && plan->len[TONE_PHASE_1] < TONE_SHAKE_MIN) plan->len[TONE_PHASE_1] = TONE_SHAKE_MIN;
    }
    plan->len[TONE_PHASE_2] = len2;
    return true;
}

uint8_t tone_plan_code(const tone_plan& plan, unsigned e, tone_phase ph, unsigned k)
{
    const tone_seq& s = tone_tables::seq[e];
    if(ph == TONE_PHASE_1)
    {
        // Every entry's drive pulls end at mid and its S and M pulls end at len, so neighbouring
        // pixels of different entries drive on the same pulls. This alignment matters for
        // artifacts at the edges between tones.
        const unsigned a   = mag(s.prefix) + s.n1a;
        const unsigned b   = s.n1b;
        const unsigned len = plan.len[TONE_PHASE_1];
        if(k < plan.mid - a) return TONE_P1_S;
        if(k < plan.mid) return p1_part_code(s, k - (plan.mid - a));
        if(k < len - b) return TONE_P1_S;
        return p1_part_code(s, a + k - (len - b));
    }
    // j is the played letter at pull k: the nudge, then p2.
    const unsigned nd  = mag(s.nudge);
    const unsigned own = nd + s.n2;
    const unsigned len = plan.len[TONE_PHASE_2];
    unsigned       j;
    if(s.split >= 0)
    {
        const unsigned head    = nd + static_cast<unsigned>(s.split);
        const unsigned tail_at = len - (own - head);
        if(k < head) j = k;
        else if(k < tail_at) return TONE_P2_H;
        else j = head + k - tail_at;
    }
    else
    {
        if(k < len - own) return TONE_P2_H;
        j = k - (len - own);
    }
    if(j >= own) return TONE_P2_H;
    if(j < nd) return s.nudge > 0 ? TONE_P2_I : TONE_P2_P;
    return letter_code("hpiw", s.p2[j - nd]);
}
