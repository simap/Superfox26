// Pattern expansion, DC balance and push limit of main/tone_plan.h. See tone_plan_expand.h.

#include "tone_plan_expand.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "epd97_config.h"
#include "epd97_lut.h"

namespace
{
// Phase 2 shapes per pull at unit TONE_NUDGE_FRAMES, the seq table that epd_build_lutb_seq() in
// epd97_lut.h builds. G ground, H VDH, L VDL, R VDHR.
const char P2_SHAPE_P[] = "GLLLLGGGGGG";
const char P2_SHAPE_I[] = "GGGGGGHHHHG";
const char P2_SHAPE_W[] = "GRRRRRRRRRG";
const char P2_SHAPE_H[] = "GGGGGGGGGGG";

static_assert(TONE_NUDGE_FRAMES >= 1 && TONE_NUDGE_FRAMES <= EPD_SEQ_UNIT_MAX, "nudge unit in range");
static_assert(sizeof(P2_SHAPE_H) - 1 == EPD_SEQ_PULL_FRAMES, "h shape is the pull length");
static_assert(sizeof(P2_SHAPE_P) - 1 == EPD_SEQ_PULL_FRAMES, "p shape is the pull length");
static_assert(sizeof(P2_SHAPE_I) - 1 == EPD_SEQ_PULL_FRAMES, "i shape is the pull length");
static_assert(sizeof(P2_SHAPE_W) - 1 == EPD_SEQ_PULL_FRAMES, "w shape is the pull length");
static_assert(TONE_NUDGE_FRAMES == 4, "p and i shapes drive TONE_NUDGE_FRAMES");
static_assert(EPD_SEQ_W_FRAMES == 9, "w shape drives EPD_SEQ_W_FRAMES");
static_assert(TONE_SEQ_MAX <= 255, "part lengths fit in a byte");

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

void push_frames(tone_push* u, const char* shape)
{
    for(; *shape != '\0'; shape++)
    {
        switch(*shape)
        {
        case 'H':
            u->vdh++;
            u->pos++;
            u->vdl = 0;
            break;
        case 'L':
            u->vdl++;
            u->vdh  = 0;
            u->vdhr = 0;
            u->pos  = 0;
            break;
        case 'R':
            u->vdhr++;
            u->pos++;
            u->vdl = 0;
            break;
        default: break;
        }
        if(u->vdh > u->max_vdh) u->max_vdh = u->vdh;
        if(u->vdl > u->max_vdl) u->max_vdl = u->vdl;
        if(u->vdhr > u->max_vdhr) u->max_vdhr = u->vdhr;
        if(u->pos > u->max_pos) u->max_pos = u->pos;
    }
}

const char* p1_shape(char c)
{
    switch(c)
    {
    case 'I': return EPD_PHASE1_SHAPE_I;
    case 'P': return EPD_PHASE1_SHAPE_P;
    case 'S': return EPD_PHASE1_SHAPE_S;
    case 'M': return EPD_PHASE1_SHAPE_M;
    default: return "";
    }
}

const char* p2_shape(char c)
{
    switch(c)
    {
    case 'p': return P2_SHAPE_P;
    case 'i': return P2_SHAPE_I;
    case 'w': return P2_SHAPE_W;
    case 'h': return P2_SHAPE_H;
    default: return "";
    }
}

// One play of s: an S pull if lead, the prefix, the I and P, an S pull if mid, the S and M, the
// nudge, the phase 2 letters. One S pull stands for a pad or fill of any length, since every S
// pull starts and ends on the same frames.
void push_play(const tone_seq& s, bool lead, bool mid, tone_push* u)
{
    *u = tone_push();
    if(lead) push_frames(u, EPD_PHASE1_SHAPE_S);
    for(unsigned k = 0; k < mag(s.prefix); k++) push_frames(u, s.prefix > 0 ? EPD_PHASE1_SHAPE_I : EPD_PHASE1_SHAPE_P);
    for(const char* c = s.p1a; *c != '\0'; c++) push_frames(u, p1_shape(*c));
    if(mid) push_frames(u, EPD_PHASE1_SHAPE_S);
    for(const char* c = s.p1b; *c != '\0'; c++) push_frames(u, p1_shape(*c));
    for(unsigned k = 0; k < mag(s.nudge); k++) push_frames(u, s.nudge > 0 ? P2_SHAPE_I : P2_SHAPE_P);
    for(const char* c = s.p2; *c != '\0'; c++) push_frames(u, p2_shape(*c));
}

unsigned max_u(unsigned a, unsigned b)
{
    return a > b ? a : b;
}

bool append(char* dst, unsigned* n, char c, const char* part, char* why, size_t why_len)
{
    if(*n >= TONE_SEQ_MAX) return fail(why, why_len, "%s is over %u pulls after expansion", part, TONE_SEQ_MAX);
    dst[(*n)++] = c;
    return true;
}
} // namespace

// Letters I and P before any S or M go to p1a, S and M to p1b, h p i and w to p2, r is p w and R is
// p w w. h is a phase 2 hold pull: ground, net 0, counted in the pattern's phase 2 pulls. An I or P
// after an S or M plays in phase 2 as i i or p p, the same net in nudges. A digit run repeats the
// letter before it. One '|' with no digit run after it splits phase 2. The nudge and the letters
// before it play from the first pull of phase 2, and the letters after it end on the last pull.
// Hold pulls fill the gap, and this pad is not counted in the pattern's pulls. Letters after the
// '|' are phase 2 letters, so an I or P there plays as i i or p p.
bool tone_pattern_expand(const char* src, tone_seq* s, char* why, size_t why_len)
{
    unsigned n1a = 0, n1b = 0, n2 = 0;
    *s       = tone_seq();
    s->split = -1;
    for(size_t k = 0; src[k] != '\0';)
    {
        const char c = src[k++];
        if(c == '|')
        {
            if(s->split >= 0) return fail(why, why_len, "more than one '|' in '%s'", src);
            if(isdigit(static_cast<unsigned char>(src[k])))
                return fail(why, why_len, "a repeat count after '|' in '%s'", src);
            s->split = static_cast<int8_t>(n2);
            continue;
        }
        const bool in2 = n2 > 0 || s->split >= 0;
        unsigned   rep = 1;
        if(isdigit(static_cast<unsigned char>(src[k])))
        {
            rep = 0;
            while(isdigit(static_cast<unsigned char>(src[k])))
            {
                rep = rep * 10u + static_cast<unsigned>(src[k] - '0');
                if(rep > TONE_SEQ_MAX) return fail(why, why_len, "repeat count over %u in '%s'", TONE_SEQ_MAX, src);
                k++;
            }
            if(rep == 0) return fail(why, why_len, "repeat count 0 in '%s'", src);
        }
        const bool  first = n1b == 0 && !in2;
        const char* unit;
        unsigned    group; // 1 for p1a, 2 for p1b, 3 for p2
        switch(c)
        {
        case 'I':
            unit  = first ? "I" : "ii";
            group = first ? 1u : 3u;
            break;
        case 'P':
            unit  = first ? "P" : "pp";
            group = first ? 1u : 3u;
            break;
        case 'S':
            unit  = "S";
            group = 2u;
            break;
        case 'M':
            unit  = "M";
            group = 2u;
            break;
        case 'h':
            unit  = "h";
            group = 3u;
            break;
        case 'p':
            unit  = "p";
            group = 3u;
            break;
        case 'i':
            unit  = "i";
            group = 3u;
            break;
        case 'r':
            unit  = "pw";
            group = 3u;
            break;
        case 'R':
            unit  = "pww";
            group = 3u;
            break;
        default: return fail(why, why_len, "'%c' in '%s' is not one of I P S M h i p r R |", c, src);
        }
        if(group == 2u && in2) return fail(why, why_len, "'%c' after a phase 2 letter in '%s'", c, src);
        if(c == 'S' && n1b > 0 && s->p1b[n1b - 1] == 'M')
            return fail(why, why_len, "'S' after 'M' in '%s', nothing but M follows M", src);
        for(unsigned r = 0; r < rep; r++)
        {
            for(const char* u = unit; *u != '\0'; u++)
            {
                const bool ok = group == 1u   ? append(s->p1a, &n1a, *u, "the I and P part", why, why_len)
                                : group == 2u ? append(s->p1b, &n1b, *u, "the S and M part", why, why_len)
                                              : append(s->p2, &n2, *u, "phase 2", why, why_len);
                if(!ok) return false;
            }
        }
    }
    s->p1a[n1a] = '\0';
    s->p1b[n1b] = '\0';
    s->p2[n2]   = '\0';
    s->n1a      = static_cast<uint8_t>(n1a);
    s->n1b      = static_cast<uint8_t>(n1b);
    s->n2       = static_cast<uint8_t>(n2);

    int net = 0;
    for(unsigned k = 0; k < n1a; k++) net += tone_p1_net(s->p1a[k]);
    for(unsigned k = 0; k < n1b; k++) net += tone_p1_net(s->p1b[k]);
    for(unsigned k = 0; k < n2; k++) net += tone_p2_net(s->p2[k]);
    const tone_balance b = tone_balance_of(net);
    if(b.prefix < -127 || b.prefix > 127) return fail(why, why_len, "'%s' needs %u prefix pulls", src, mag(b.prefix));
    s->net    = static_cast<int16_t>(net);
    s->prefix = static_cast<int8_t>(b.prefix);
    s->nudge  = static_cast<int8_t>(b.nudge);
    if(b.residual < -127 || b.residual > 127)
        return fail(why, why_len, "'%s' leaves %d frames unpaid", src, b.residual);
    s->residual = static_cast<int8_t>(b.residual);

    tone_push u;
    tone_push_of(*s, &u);
    if(u.max_vdh > EPD_PUSH_MAX_VDH || u.max_vdl > EPD_PUSH_MAX_VDL || u.max_vdhr > EPD_PUSH_MAX_VDHR)
        return fail(why, why_len, "'%s' pushes %u VDH, %u VDL, %u VDHR frames in one run, the limits are %u, %u, %u",
                    src, u.max_vdh, u.max_vdl, u.max_vdhr, static_cast<unsigned>(EPD_PUSH_MAX_VDH),
                    static_cast<unsigned>(EPD_PUSH_MAX_VDL), static_cast<unsigned>(EPD_PUSH_MAX_VDHR));
    if(tone_push_over(u))
        return fail(why, why_len, "'%s' drives a positive run of %u VDH and VDHR frames, the limit is %u", src,
                    u.max_pos, static_cast<unsigned>(EPD_PUSH_MAX_VDH));
    // Every draw opens with an S pull on every pixel. Its head extends an open VDH tail and an open
    // positive tail, then its VDL ends both. An open VDL tail ends at the head's first VDH frame, and
    // VDH does not extend an open VDHR tail, so both stay within the check above.
    const unsigned head = tone_s_head_vdh();
    if(u.vdh + head > EPD_PUSH_MAX_VDH)
        return fail(why, why_len, "'%s' ends on an open run of %u VDH frames, and the next draw's S head adds %u, over %u",
                    src, u.vdh, head, static_cast<unsigned>(EPD_PUSH_MAX_VDH));
    if(u.pos + head > EPD_PUSH_MAX_VDH)
        return fail(why, why_len,
                    "'%s' ends on an open positive run of %u frames, and the next draw's S head adds %u, over %u", src,
                    u.pos, head, static_cast<unsigned>(EPD_PUSH_MAX_VDH));
    return true;
}

// Two plays, both opened by an S pull: S, prefix, I P, S, S M, where S fill sits between the
// parts, and S, prefix, I P, S M, where the S and M part fills to the end. Where an S pull sits
// decides which runs it joins, so the count takes the larger of the two per rail. The h pulls and
// the hold pads of phase 2 are ground and end nothing. So a '|' pad between two letters joins the
// same runs as the play without it. The open runs at the end are the larger of the two plays too.
void tone_push_of(const tone_seq& s, tone_push* u)
{
    *u                         = tone_push();
    static const bool k_mid[2] = {true, false};
    for(bool mid : k_mid)
    {
        tone_push p;
        push_play(s, true, mid, &p);
        u->vdh      = max_u(u->vdh, p.vdh);
        u->vdl      = max_u(u->vdl, p.vdl);
        u->vdhr     = max_u(u->vdhr, p.vdhr);
        u->pos      = max_u(u->pos, p.pos);
        u->max_vdh  = max_u(u->max_vdh, p.max_vdh);
        u->max_vdl  = max_u(u->max_vdl, p.max_vdl);
        u->max_vdhr = max_u(u->max_vdhr, p.max_vdhr);
        u->max_pos  = max_u(u->max_pos, p.max_pos);
    }
}
