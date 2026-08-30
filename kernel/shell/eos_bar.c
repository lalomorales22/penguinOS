#include "eos_bar.h"
#include <string.h>
#include <stdio.h>

// Priority is the whole contract. Higher survives narrower bars. The ordering
// is not "what is interesting", it is "what is not already on the screen":
// the pips and the clock cannot be read off the windows themselves, the title
// mostly can, and the buddy is charm. Change these and the OLED bar changes.
static const uint8_t PRIO[EOS_BAR_SEGS] = {
    100,  // EOS_SEG_PIPS
     60,  // EOS_SEG_TITLE
     20,  // EOS_SEG_MOOD
     30,  // EOS_SEG_HEAP
     40,  // EOS_SEG_BRAIN
     80,  // EOS_SEG_WIFI
     90   // EOS_SEG_CLOCK
};

static const char *SEGNAME[EOS_BAR_SEGS] = {
    "pips", "title", "mood", "heap", "brain", "wifi", "clock"
};

uint8_t eos_bar_priority(eos_bar_seg_id_t id)
{
    int i = (int)id;
    return (i >= 0 && i < EOS_BAR_SEGS) ? PRIO[i] : 0;
}

const char *eos_bar_seg_name(eos_bar_seg_id_t id)
{
    int i = (int)id;
    return (i >= 0 && i < EOS_BAR_SEGS) ? SEGNAME[i] : "?";
}

// Two cells wide in every case, so the mood never changes the bar's width.
const char *eos_bar_mood_glyph(eos_bar_mood_t m)
{
    switch (m) {
    case EOS_MOOD_IDLE:      return ":|";
    case EOS_MOOD_THINKING:  return ":?";
    case EOS_MOOD_TALKING:   return ":o";
    case EOS_MOOD_LISTENING: return ":^";
    case EOS_MOOD_SLEEPING:  return "zz";
    case EOS_MOOD_HAPPY:     return ":]";
    case EOS_MOOD_CONFUSED:  return ":/";
    }
    return ":|";
}

void eos_bar_metrics_init(eos_bar_metrics_t *m, int16_t char_w, int16_t pad)
{
    memset(m, 0, sizeof(*m));
    m->char_w   = char_w;
    m->pad      = pad;
    m->ellipsis = '~';
}

void eos_bar_status_init(eos_bar_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->wifi      = EOS_WIFI_OFF;
    st->wifi_rssi = -127;
    st->heap_warn = 12u * 1024u;   // the CYD idles near 20k; 12k is the cliff
    st->mood      = EOS_MOOD_IDLE;
}

static int16_t meas(const eos_bar_metrics_t *m, const char *s)
{
    if (m->measure) return m->measure(s, m->ud);
    return (int16_t)(m->char_w * (int16_t)strlen(s));
}

// Longest prefix of `src` that fits `avail` once the ellipsis is added. Done
// by measuring, not by dividing, because an LVGL font is proportional and the
// arithmetic answer is wrong by a few pixels exactly when it matters.
static void fit_text(const eos_bar_metrics_t *m, const char *src, int16_t avail,
                     char *out, int outn)
{
    char buf[EOS_BAR_TEXT_MAX + 2];
    int  len = (int)strlen(src);
    int  best = 0;

    if (len > outn - 2) len = outn - 2;   // room for the ellipsis and the NUL
    if (meas(m, src) <= avail && (int)strlen(src) <= outn - 1) {
        snprintf(out, (size_t)outn, "%s", src);
        return;
    }
    for (int k = 1; k <= len && k + 2 <= (int)sizeof(buf); k++) {
        memcpy(buf, src, (size_t)k);
        buf[k]     = m->ellipsis;
        buf[k + 1] = 0;
        if (meas(m, buf) > avail) break;
        best = k;
    }
    while (best > 1 && src[best - 1] == ' ') best--;   // no "cpm ~"
    if (best == 0) { out[0] = 0; return; }
    memcpy(out, src, (size_t)best);
    out[best]     = m->ellipsis;
    out[best + 1] = 0;
}

// ---------------------------------------------------------------- candidates

typedef struct {
    eos_bar_seg_id_t id;
    eos_bar_role_t   role;
    eos_bar_align_t  align;
    uint8_t          prio;
    bool             flex;                   // absorbs leftover, truncates
    char             v[3][EOS_BAR_TEXT_MAX];  // v[0] shortest .. v[nv-1] longest
    int8_t           nv;
    int16_t          w[3];
    int16_t          min_w;
    int8_t           chosen;                 // -1 dropped, else variant index
    int16_t          cw;                     // width actually granted
    int16_t          x;                      // laid-out position in the bar
} cand_t;

static void cput(cand_t *c, int slot, const char *s)
{
    snprintf(c->v[slot], sizeof(c->v[slot]), "%s", s);
}

static void build_pips(cand_t *c, const eos_bar_status_t *st)
{
    char lng[EOS_BAR_TEXT_MAX], srt[EOS_BAR_TEXT_MAX], mini[8];
    int  a = 0, b = 0;
    // ws_active is a caller-set field, so it is not trusted to be a workspace
    // that exists. Out of range means "no workspace is marked live" rather
    // than the shortest form naming a workspace the long form never lists.
    int  active = (st->ws_active < EOS_WORKSPACE_PIPS) ? (int)st->ws_active : -1;

    for (int i = 0; i < EOS_WORKSPACE_PIPS; i++) {
        bool occ = (st->ws_occupied >> i) & 1u;
        bool act = (active == i);
        if (!occ && !act) continue;
        char cell[8];
        if (act) snprintf(cell, sizeof(cell), "[%d]", i + 1);
        else     snprintf(cell, sizeof(cell), "%d", i + 1);
        for (const char *p = cell; *p; p++) {
            if (a < (int)sizeof(lng) - 2) lng[a++] = *p;
            if (b < (int)sizeof(srt) - 1) srt[b++] = *p;
        }
        if (a < (int)sizeof(lng) - 1) lng[a++] = ' ';
    }
    if (a > 0 && lng[a - 1] == ' ') a--;
    lng[a] = 0;
    srt[b] = 0;
    if (active >= 0) snprintf(mini, sizeof(mini), "%d", active + 1);
    else             snprintf(mini, sizeof(mini), "%.*s", (int)sizeof(mini) - 1, srt);

    cput(c, 0, mini);
    cput(c, 1, srt);
    cput(c, 2, lng);
    c->nv = 3;
}

static void build_wifi(cand_t *c, const eos_bar_status_t *st)
{
    switch (st->wifi) {
    case EOS_WIFI_UP: {
        int   r = st->wifi_rssi;
        char  ramp = (r >= -55) ? '#' : (r >= -67) ? '=' : (r >= -78) ? '-' : '_';
        char  s1[8], s2[EOS_BAR_TEXT_MAX];
        snprintf(s1, sizeof(s1), "%c", ramp);
        cput(c, 0, s1);
        snprintf(s2, sizeof(s2), "%d", r);
        cput(c, 1, s2);
        snprintf(s2, sizeof(s2), "wifi %d", r);
        cput(c, 2, s2);
        c->nv   = 3;
        c->role = (r >= -78) ? EOS_BAR_ROLE_OK : EOS_BAR_ROLE_WARN;
        break;
    }
    case EOS_WIFI_JOINING:
        cput(c, 0, "?"); cput(c, 1, "..."); cput(c, 2, "wifi ...");
        c->nv = 3; c->role = EOS_BAR_ROLE_MUTED;
        break;
    case EOS_WIFI_DOWN:
        cput(c, 0, "x"); cput(c, 1, "down"); cput(c, 2, "wifi down");
        c->nv = 3; c->role = EOS_BAR_ROLE_WARN;
        break;
    default:
        cput(c, 0, "-"); cput(c, 1, "off"); cput(c, 2, "wifi off");
        c->nv = 3; c->role = EOS_BAR_ROLE_MUTED;
        break;
    }
}

static void build_brain(cand_t *c, const eos_bar_status_t *st)
{
    const char *model = (st->brain_model && st->brain_model[0]) ? st->brain_model : "?";
    char s[EOS_BAR_TEXT_MAX];

    if (st->brain_up) {
        cput(c, 0, "b");
        cput(c, 1, model);
        snprintf(s, sizeof(s), "brain %s", model);
        cput(c, 2, s);
        c->role = EOS_BAR_ROLE_OK;
    } else {
        cput(c, 0, "b!");
        cput(c, 1, "no brain");
        cput(c, 2, "brain down");
        c->role = EOS_BAR_ROLE_WARN;
    }
    c->nv = 3;
}

static void build_heap(cand_t *c, const eos_bar_status_t *st)
{
    char s[12], l[EOS_BAR_TEXT_MAX];      // s holds "999k" or "4.0M", never more
    unsigned kb = (unsigned)(st->free_heap / 1024u);

    if (kb < 1000u) snprintf(s, sizeof(s), "%uk", kb);
    else            snprintf(s, sizeof(s), "%u.%uM", kb / 1024u, (kb % 1024u) * 10u / 1024u);
    snprintf(l, sizeof(l), "heap %s", s);

    cput(c, 0, s);
    cput(c, 1, l);
    c->nv   = 2;
    c->role = (st->free_heap <= st->heap_warn) ? EOS_BAR_ROLE_WARN : EOS_BAR_ROLE_MUTED;
}

static void build_clock(cand_t *c, const eos_bar_status_t *st)
{
    char s[EOS_BAR_TEXT_MAX];
    if (st->clock_valid) snprintf(s, sizeof(s), "%02u:%02u",
                                  (unsigned)(st->hour % 24u), (unsigned)(st->minute % 60u));
    else                 snprintf(s, sizeof(s), "--:--");
    cput(c, 0, s);
    c->nv   = 1;
    c->role = st->clock_valid ? EOS_BAR_ROLE_FG : EOS_BAR_ROLE_MUTED;
}

// ---------------------------------------------------------------------- fit

int eos_bar_build(const eos_bar_status_t *st, const eos_bar_metrics_t *m,
                  int16_t width, eos_bar_seg_t *out, int max)
{
    cand_t c[EOS_BAR_SEGS];
    int    n = 0;

    if (!st || !m || !out || max <= 0 || width <= 0) return 0;

    memset(c, 0, sizeof(c));

    // Display order, left to right. Alignment decides which end they pack to.
    cand_t *pips  = &c[n]; c[n].id = EOS_SEG_PIPS;  c[n].align = EOS_ALIGN_LEFT;
                           c[n].role = EOS_BAR_ROLE_ACCENT; n++;
    cand_t *title = &c[n]; c[n].id = EOS_SEG_TITLE; c[n].align = EOS_ALIGN_LEFT;
                           c[n].role = EOS_BAR_ROLE_FG; c[n].flex = true; n++;
    cand_t *mood  = &c[n]; c[n].id = EOS_SEG_MOOD;  c[n].align = EOS_ALIGN_RIGHT;
                           c[n].role = EOS_BAR_ROLE_ACCENT; n++;
    cand_t *heap  = &c[n]; c[n].id = EOS_SEG_HEAP;  c[n].align = EOS_ALIGN_RIGHT; n++;
    cand_t *brain = &c[n]; c[n].id = EOS_SEG_BRAIN; c[n].align = EOS_ALIGN_RIGHT; n++;
    cand_t *wifi  = &c[n]; c[n].id = EOS_SEG_WIFI;  c[n].align = EOS_ALIGN_RIGHT; n++;
    cand_t *clock = &c[n]; c[n].id = EOS_SEG_CLOCK; c[n].align = EOS_ALIGN_RIGHT; n++;

    build_pips(pips, st);
    build_wifi(wifi, st);
    build_brain(brain, st);
    build_heap(heap, st);
    build_clock(clock, st);
    cput(mood, 0, eos_bar_mood_glyph(st->mood));
    { char s[EOS_BAR_TEXT_MAX];
      snprintf(s, sizeof(s), "buddy %s", eos_bar_mood_glyph(st->mood));
      cput(mood, 1, s); }
    mood->nv = 2;

    if (st->title && st->title[0]) {
        cput(title, 0, st->title);
        title->nv = 1;
    } else {
        title->nv = 0;                       // no focused window: no segment
    }

    // Measure every written form, and work out the floor for the flexible one.
    for (int i = 0; i < n; i++) {
        c[i].prio   = PRIO[c[i].id];
        c[i].chosen = -1;
        for (int k = 0; k < c[i].nv; k++) c[i].w[k] = meas(m, c[i].v[k]);
        if (c[i].nv == 0) { c[i].min_w = 0; continue; }
        if (c[i].flex) {
            // three characters plus the ellipsis is the least that says anything
            char floorbuf[8];
            int  keep = (int)strlen(c[i].v[0]);
            if (keep > 3) keep = 3;
            memcpy(floorbuf, c[i].v[0], (size_t)keep);
            floorbuf[keep] = (keep < (int)strlen(c[i].v[0])) ? m->ellipsis : 0;
            floorbuf[keep + 1] = 0;
            c[i].min_w = meas(m, floorbuf);
            if (c[i].min_w > c[i].w[0]) c[i].min_w = c[i].w[0];
        } else {
            c[i].min_w = c[i].w[0];
        }
    }

    // Pass 1: take segments in priority order at their floor width. The first
    // one that does not fit ends the run, so what survives is always a prefix
    // of the priority order - a lower-priority segment can never outlive a
    // higher-priority one.
    //
    // `max` bounds the run for the same reason the width does. Emitting in
    // display order and truncating at the tail instead would drop the clock
    // to keep the buddy, which is exactly the promise this function makes.
    int16_t used = 0;
    int     kept = 0;
    for (;;) {
        if (kept >= max) break;
        int pick = -1;
        for (int i = 0; i < n; i++) {
            if (c[i].chosen >= 0 || c[i].nv == 0) continue;
            if (pick < 0 || c[i].prio > c[pick].prio) pick = i;
        }
        if (pick < 0) break;
        int16_t want = (int16_t)(used + c[pick].min_w + (kept ? m->pad : 0));
        if (want > width) break;
        c[pick].chosen = 0;
        c[pick].cw     = c[pick].min_w;
        used = want;
        kept++;
    }
    if (kept == 0) return 0;

    // Pass 2: spend what is left, strictly in priority order. Each segment in
    // turn takes the longest written form it can still afford; a flexible one
    // takes as much of its natural width as remains. Nothing lower-priority
    // ever gets a pixel a higher-priority segment could have used.
    {
        bool done[EOS_BAR_SEGS];
        for (int i = 0; i < n; i++) done[i] = (c[i].chosen < 0);
        for (;;) {
            int pick = -1;
            for (int i = 0; i < n; i++) {
                if (done[i]) continue;
                if (pick < 0 || c[i].prio > c[pick].prio) pick = i;
            }
            if (pick < 0) break;
            done[pick] = true;

            cand_t *g    = &c[pick];
            int16_t room = (int16_t)(width - used);
            if (g->flex) {
                int16_t grow = (int16_t)(g->w[0] - g->cw);
                if (grow > room) grow = room;
                if (grow > 0) {
                    g->cw = (int16_t)(g->cw + grow);
                    used  = (int16_t)(used + grow);
                }
            } else {
                for (int k = g->nv - 1; k > g->chosen; k--) {
                    int16_t cost = (int16_t)(g->w[k] - g->cw);
                    if (cost > room) continue;
                    g->chosen = (int8_t)k;
                    g->cw     = g->w[k];
                    used      = (int16_t)(used + cost);
                    break;
                }
            }
        }
    }

    // ------------------------------------------------------------- placement
    int16_t right_total = 0;
    int     right_n = 0, center_n = 0;
    int16_t center_total = 0;
    for (int i = 0; i < n; i++) {
        if (c[i].chosen < 0) continue;
        if (c[i].align == EOS_ALIGN_RIGHT)  { right_total  = (int16_t)(right_total + c[i].cw);  right_n++; }
        if (c[i].align == EOS_ALIGN_CENTER) { center_total = (int16_t)(center_total + c[i].cw); center_n++; }
    }
    if (right_n  > 1) right_total  = (int16_t)(right_total  + m->pad * (right_n  - 1));
    if (center_n > 1) center_total = (int16_t)(center_total + m->pad * (center_n - 1));

    int16_t lx = 0;
    int16_t rx = (int16_t)(width - right_total);
    if (rx < 0) rx = 0;

    for (int i = 0; i < n; i++) {
        if (c[i].chosen < 0 || c[i].align != EOS_ALIGN_LEFT) continue;
        c[i].x = lx;
        lx = (int16_t)(lx + c[i].cw + m->pad);
    }
    if (lx > 0) lx = (int16_t)(lx - m->pad);

    if (center_n > 0) {
        int16_t gap  = (int16_t)(rx - lx);
        int16_t cx   = (int16_t)(lx + (gap - center_total) / 2);
        if (cx < lx) cx = lx;
        for (int i = 0; i < n; i++) {
            if (c[i].chosen < 0 || c[i].align != EOS_ALIGN_CENTER) continue;
            c[i].x = cx;
            cx = (int16_t)(cx + c[i].cw + m->pad);
        }
    }
    for (int i = 0; i < n; i++) {
        if (c[i].chosen < 0 || c[i].align != EOS_ALIGN_RIGHT) continue;
        c[i].x = rx;
        rx = (int16_t)(rx + c[i].cw + m->pad);
    }

    // -------------------------------------------------------------- emission
    int outn = 0;
    for (int i = 0; i < n && outn < max; i++) {
        if (c[i].chosen < 0) continue;
        eos_bar_seg_t *s = &out[outn++];
        if (c[i].flex) fit_text(m, c[i].v[0], c[i].cw, s->text, (int)sizeof(s->text));
        else           snprintf(s->text, sizeof(s->text), "%s", c[i].v[c[i].chosen]);
        s->id       = c[i].id;
        s->role     = c[i].role;
        s->align    = c[i].align;
        s->priority = c[i].prio;
        s->x        = c[i].x;
        s->w        = c[i].cw;
    }
    return outn;
}
