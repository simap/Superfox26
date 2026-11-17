// The plan of one draw and the boot check of the expanded patterns. See tone_plan.h.

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

// Phase 1 code of pull j of the prefix, the I and P and the S and M of s, in that order.
uint8_t p1_part_code(const tone_seq& s, unsigned j)
{
    const unsigned pre = mag(s.prefix);
    if(j < pre) return s.prefix > 0 ? TONE_P1_I : TONE_P1_P;
    j -= pre;
    return letter_code("SIPM", j < s.n1a ? s.p1a[j] : s.p1b[j - s.n1a]);
}

// The letters of one part: n of them from the set letters, then the NUL, n at most TONE_SEQ_MAX.
// Returns their net through net_of, or false.
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
} // namespace

// Three checks per entry. The generator refuses an entry that fails any check of
// tone_pattern_expand(), so these fail only for a file that does not come from it or a generator
// that does not follow the rules of this firmware.
//   The parts. tone_plan_code() reads n1a, n1b and n2 letters from p1a, p1b and p2 and turns each
//   into a table code by its place in "SIPM" or "hpiw". A count past its part or a letter outside
//   the set would play a code the pattern does not hold.
//   The balance. The net is the sum of the letters' nets as this firmware counts them, and the
//   prefix, the nudge and the residual are tone_balance_of() of that net. A draw then pays back
//   what its letters drive, and every pixel keeps only the residual the planner computes.
//   The push. The longest run on each rail and the positive run are within the limits of
//   epd97_config.h as compiled here, and the runs left open at the end stay within the VDH limit
//   with the S head of the next draw. The figures are tone_push_of() over the same parts on the
//   host. tone_tables.h holds the build to the shapes and limits they were made with.
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
           !part_ok(s.p2, s.n2, "piw", tone_p2_net, &net))
            return fail(why, why_len, "entry %u: a part's length or letters do not match", e);
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

const tone_seq& tone_plan_seq(unsigned e)
{
    return tone_tables::seq[e];
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
    // A draw that drives in neither phase plays nothing.
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
        const unsigned a   = mag(s.prefix) + s.n1a;
        const unsigned b   = s.n1b;
        const unsigned len = plan.len[TONE_PHASE_1];
        if(k < plan.mid - a) return TONE_P1_S;
        if(k < plan.mid) return p1_part_code(s, k - (plan.mid - a));
        if(k < len - b) return TONE_P1_S;
        return p1_part_code(s, a + k - (len - b));
    }
    const unsigned own   = mag(s.nudge) + s.n2;
    const unsigned start = plan.len[TONE_PHASE_2] - own;
    if(k >= start + own || k < start) return TONE_P2_H;
    const unsigned k2 = k - start;
    const unsigned nd = mag(s.nudge);
    if(k2 < nd) return s.nudge > 0 ? TONE_P2_I : TONE_P2_P;
    return letter_code("hpiw", s.p2[k2 - nd]);
}
