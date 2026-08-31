// Host test for the pointer: the decode, the curve, the cursor and the click.
//
// Four things are worth testing here and they are the four things that are
// easy to get wrong on a board nobody can attach a debugger to.
//
// The decode, because a uint8_t read of 0xFF is 255 and an int8_t read of it
// is -1, and the whole difference between those two is a cursor that can only
// ever travel right and down.
//
// The curve, because acceleration is where a pointer stops being arithmetic
// and starts being a feel, and the only part of a feel a test can hold onto is
// that it is monotone, symmetric, and still capable of moving exactly one
// pixel when asked.
//
// The clamp, because a 240x240 panel has four edges and each of them is a
// separate off-by-one.
//
// And the hit test, against a REAL eos_wm layout rather than a table of
// handmade rectangles - including the two answers that are supposed to be
// nothing at all, a click in the gap between tiles and a click on the status
// bar. A hit test checked against invented rects proves only that the test and
// the code agree with each other.
//
// cc -std=c99 -Wall -Wextra -O1 -Ikernel/hal/include -Ikernel/wm/include \
//    -Ikernel/shell/include -Ikernel/svc/include -Iboards/generated \
//    kernel/shell/eos_pointer.c kernel/wm/eos_wm.c kernel/hal/eos_input.c \
//    kernel/svc/eos_ble.c kernel/shell/test/test_pointer.c -o /tmp/test_pointer

#include <stdio.h>
#include <string.h>

#include "eos_pointer.h"
#include "eos_ble.h"
#include "eos_wm.h"

static int checks = 0, fails = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } } while (0)
#define CKI(got, want, msg) do { \
    long g_ = (long)(got), w_ = (long)(want); \
    checks++; \
    if (g_ != w_) { fails++; printf("    FAIL: %s (got %ld, want %ld)\n", (msg), g_, w_); } \
} while (0)

// ------------------------------------------------------------------ capture
//
// The events eos_pointer_feed() pushed, drained out of the real HAL ring. It
// is the real ring on purpose: the coalescing rule that keeps a swipe from
// evicting a click lives inside eos_input_inject_pointer(), and a test that
// captured events before that point would never see it.

#define CAPMAX 64
typedef struct { eos_event_t ev[CAPMAX]; int n; } cap_t;

static void drain(cap_t *c)
{
    eos_event_t e;
    c->n = 0;
    while (eos_input_poll(&e)) {
        if (c->n < CAPMAX) c->ev[c->n++] = e;
    }
}

static int count_of(const cap_t *c, uint8_t type)
{
    int i, n = 0;
    for (i = 0; i < c->n; i++) if (c->ev[i].type == type) n++;
    return n;
}

static const eos_event_t *find_ev(const cap_t *c, uint8_t type, uint8_t btn)
{
    int i;
    for (i = 0; i < c->n; i++)
        if (c->ev[i].type == type && (btn == 0 || c->ev[i].key == btn))
            return &c->ev[i];
    return NULL;
}

// eos_rect_empty() lives in eos_display.h and eos_pointer.c deliberately does
// not include it, so the test carries its own two comparisons rather than
// pulling a display header into a suite that draws nothing.
static bool rect_empty(eos_rect_t r) { return r.w <= 0 || r.h <= 0; }

static int index_of(const cap_t *c, uint8_t type)
{
    int i;
    for (i = 0; i < c->n; i++) if (c->ev[i].type == type) return i;
    return -1;
}

// ------------------------------------------------------------------- decode

static void decode(void)
{
    eos_ble_mouse_t m;
    uint8_t rep[8];

    printf("\n=== boot mouse decode ===\n");

    memset(&m, 0, sizeof m);
    rep[0] = 0x00; rep[1] = 5; rep[2] = 7;
    CK(eos_ble_decode_mouse(rep, 3, &m), "three bytes is a boot mouse report");
    CKI(m.dx, 5, "dx comes off byte 1");
    CKI(m.dy, 7, "dy comes off byte 2");
    CKI(m.buttons, 0, "no buttons held");

    // The bug this whole file exists to prevent.
    memset(&m, 0, sizeof m);
    rep[0] = 0x00; rep[1] = 0xFF; rep[2] = 0xFF;
    CK(eos_ble_decode_mouse(rep, 3, &m), "0xFF is still a report");
    CKI(m.dx, -1, "0xFF sign extends to -1, not 255");
    CKI(m.dy, -1, "on both axes");

    memset(&m, 0, sizeof m);
    rep[0] = 0x00; rep[1] = 0x80; rep[2] = 0x7F;
    (void)eos_ble_decode_mouse(rep, 3, &m);
    CKI(m.dx, -128, "0x80 is the most negative count");
    CKI(m.dy, 127,  "0x7F is the most positive");

    memset(&m, 0, sizeof m);
    rep[0] = 0x00; rep[1] = 0xF6; rep[2] = 0x0A;
    (void)eos_ble_decode_mouse(rep, 3, &m);
    CKI(m.dx, -10, "left and down in one report");
    CKI(m.dy, 10,  "and the axes do not swap");

    memset(&m, 0, sizeof m);
    rep[0] = EOS_BTN_LEFT | EOS_BTN_RIGHT; rep[1] = 0; rep[2] = 0;
    (void)eos_ble_decode_mouse(rep, 3, &m);
    CKI(m.buttons, EOS_BTN_LEFT | EOS_BTN_RIGHT, "byte 0 is the button bitmap");

    // Everything that is not three bytes. The eight-byte case is the keyboard
    // on the same bond and the four-byte case is its media keys; decoding
    // either as a pointer is how a volume key becomes a middle click.
    {
        int len;
        for (len = 0; len <= 8; len++) {
            if (len == 3) continue;
            memset(rep, 0x41, sizeof rep);
            checks++;
            if (eos_ble_decode_mouse(rep, len, &m)) {
                fails++;
                printf("    FAIL: a %d-byte report was decoded as a mouse\n", len);
            }
        }
    }
    CK(!eos_ble_decode_mouse(NULL, 3, &m), "a null report is refused");
    CK(!eos_ble_decode_mouse(rep, 3, NULL), "so is a null destination");

    // Nothing is written into out on a refusal, so a caller that ignores the
    // return value at least does not get last report's motion again.
    memset(&m, 0xAB, sizeof m);
    (void)eos_ble_decode_mouse(rep, 8, &m);
    CK(m.dx == (int16_t)0xABAB || m.dx != 0, "a refused decode writes nothing");
}

// ------------------------------------------------------------------- curve

static void curve(void)
{
    eos_pointer_accel_t a = eos_pointer_accel_defaults();
    int32_t acc = 0;
    int i;

    printf("\n=== acceleration ===\n");

    CKI(eos_pointer_scale(&a, 0, &acc), 0, "no counts is no pixels");
    CKI(acc, 0, "and no leftover");

    // The unity zone. A slow drag must be able to move exactly one pixel or
    // the cursor cannot be parked on a one-pixel tile border.
    acc = 0;
    CKI(eos_pointer_scale(&a, 1, &acc), 1, "one count is one pixel");
    CKI(eos_pointer_scale(&a, 2, &acc), 2, "two counts is two pixels");
    CKI(eos_pointer_scale(&a, -1, &acc), -1, "and one the other way is minus one");

    // Past the threshold it accelerates.
    acc = 0;
    CK(eos_pointer_scale(&a, 4, &acc) > 4, "four counts moves more than four pixels");
    acc = 0;
    CK(eos_pointer_scale(&a, 20, &acc) > 40, "a fast swipe moves a lot more");

    // Monotone: pushing harder never travels less far.
    {
        int16_t prev = 0;
        for (i = 1; i <= 127; i++) {
            int16_t px;
            acc = 0;
            px = eos_pointer_scale(&a, (int16_t)i, &acc);
            checks++;
            if (px < prev) {
                fails++;
                printf("    FAIL: %d counts moved %d px, less than %d counts did (%d)\n",
                       i, (int)px, i - 1, (int)prev);
            }
            prev = px;
        }
    }

    // Odd: left and right feel identical. This is the property an arithmetic
    // right shift would break, because -1 >> 4 is -1 and 1 >> 4 is 0.
    for (i = 1; i <= 127; i++) {
        int32_t ap = 0, an = 0;
        int16_t p = eos_pointer_scale(&a, (int16_t)i, &ap);
        int16_t n = eos_pointer_scale(&a, (int16_t)-i, &an);
        checks++;
        if (p != -n) {
            fails++;
            printf("    FAIL: +%d moved %d but -%d moved %d\n", i, (int)p, i, (int)n);
        }
    }

    // The ceiling. Nothing may exceed max_q4 times the count.
    for (i = 1; i <= 127; i++) {
        int32_t ac = 0;
        long px = eos_pointer_scale(&a, (int16_t)i, &ac);
        checks++;
        if (px * 16 > (long)i * a.max_q4) {
            fails++;
            printf("    FAIL: %d counts exceeded the %d/16 gain cap\n", i, (int)a.max_q4);
        }
    }

    // Sub-pixel accumulation: a gain that is not a whole number must not
    // quantise slow motion away one report at a time. Sixteen reports of three
    // counts at 1.625x is 78 pixels, and the remainder is what makes the
    // sixteenth one land.
    {
        long total = 0;
        acc = 0;
        for (i = 0; i < 16; i++) total += eos_pointer_scale(&a, 3, &acc);
        CKI(total, 78, "sixteen three-count reports keep their fractions");
        CK(acc == 0, "and come out even");
    }

    // A configured curve with no acceleration at all is still legal and is
    // exactly 1:1.
    {
        eos_pointer_accel_t flat;
        flat.unity = 127; flat.gain_q4 = 0; flat.max_q4 = 16;
        acc = 0;
        CKI(eos_pointer_scale(&flat, 40, &acc), 40, "a flat curve is 1:1");
        CKI(eos_pointer_scale(&flat, -40, &acc), -40, "in both directions");
    }

    CKI(eos_pointer_scale(&a, 5, NULL), 0, "no accumulator, no motion, no crash");
}

// ------------------------------------------------------------------ motion

static void motion(void)
{
    eos_pointer_t p;
    cap_t c;

    printf("\n=== motion and clamping ===\n");

    eos_pointer_init(&p, 240, 240);
    CKI(p.x, 120, "the cursor starts in the middle");
    CKI(p.y, 120, "on both axes");
    CK(!eos_pointer_visible(&p, 0), "and invisible until a device says something");

    drain(&c);
    eos_pointer_feed(&p, 1, 1, 0, 100);
    CKI(p.x, 121, "one count right is one pixel right");
    CKI(p.y, 121, "one count down is one pixel down");
    CK(eos_pointer_visible(&p, 100), "the first report makes it visible");
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_MOVE), 1, "and pushes one motion event");
    CKI(c.ev[0].x, 121, "carrying the absolute position");
    CKI(c.ev[0].src, EOS_SRC_MOUSE, "tagged as coming from the pointer");

    eos_pointer_feed(&p, -1, -1, 0, 110);
    CKI(p.x, 120, "and back left again");
    CKI(p.y, 120, "and up");

    // A report that moves nothing pushes nothing. A trackpad that is being
    // rested on sends these continuously.
    drain(&c);
    eos_pointer_feed(&p, 0, 0, 0, 120);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_MOVE), 0, "a still report is not a motion event");
    CKI(c.n, 0, "and produces no event at all");

    // All four edges. Each one is its own off-by-one and each is checked from
    // the inside as well as from the outside.
    eos_pointer_init(&p, 240, 240);
    { int i; for (i = 0; i < 40; i++) eos_pointer_feed(&p, -100, 0, 0, 200); }
    CKI(p.x, 0, "the left edge is zero, not minus one");
    { int i; for (i = 0; i < 40; i++) eos_pointer_feed(&p, 0, -100, 0, 200); }
    CKI(p.y, 0, "and the top edge is zero");
    { int i; for (i = 0; i < 40; i++) eos_pointer_feed(&p, 100, 0, 0, 200); }
    CKI(p.x, 239, "the right edge is w-1, so the whole arrow's tip stays on");
    { int i; for (i = 0; i < 40; i++) eos_pointer_feed(&p, 0, 100, 0, 200); }
    CKI(p.y, 239, "and the bottom edge is h-1");

    // Pinned against an edge, one count the other way must move immediately.
    // It is the test for an accumulator that kept piling up while clamped.
    drain(&c);
    eos_pointer_feed(&p, -1, -1, 0, 210);
    CKI(p.x, 238, "one count back off the right edge moves one pixel");
    CKI(p.y, 238, "and one off the bottom edge too");

    // A non-square panel: the clamp reads the two axes separately.
    eos_pointer_init(&p, 320, 172);
    { int i; for (i = 0; i < 40; i++) eos_pointer_feed(&p, 100, 100, 0, 300); }
    CKI(p.x, 319, "a 320-wide panel clamps x at 319");
    CKI(p.y, 171, "and y at 171");

    // The visibility timeout.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 1, 0, 0, 1000);
    CK(eos_pointer_visible(&p, 1000 + EOS_POINTER_IDLE_MS - 1), "visible just before the timeout");
    CK(!eos_pointer_visible(&p, 1000 + EOS_POINTER_IDLE_MS), "and gone at it");
    drain(&c);
}

// ------------------------------------------------------------------ buttons

static void buttons(void)
{
    eos_pointer_t p;
    cap_t c;

    printf("\n=== buttons and clicks ===\n");

    eos_pointer_init(&p, 240, 240);
    drain(&c);

    // Press.
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT, 100);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_DOWN), 1, "a button going down is one event");
    CKI(count_of(&c, EOS_EV_POINTER_UP), 0, "and not an up");
    CKI(count_of(&c, EOS_EV_CLICK), 0, "a press alone is not a click");
    CK(find_ev(&c, EOS_EV_POINTER_DOWN, EOS_BTN_LEFT) != NULL, "naming the button");
    CKI(p.buttons, EOS_BTN_LEFT, "and it is held");

    // Held. The same bitmap again is not a second press.
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT, 110);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_DOWN), 0, "holding is not pressing again");

    // Release on the spot.
    eos_pointer_feed(&p, 0, 0, 0, 120);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_UP), 1, "the release is one event");
    CKI(count_of(&c, EOS_EV_CLICK), 1, "and it produced a click");
    CKI(p.buttons, 0, "nothing is held afterwards");
    CK(index_of(&c, EOS_EV_POINTER_UP) < index_of(&c, EOS_EV_CLICK),
       "the click follows the release, never precedes it");

    // A release that wandered too far is a smudge, not a click.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT, 200);
    { int i; for (i = 0; i < 10; i++) eos_pointer_feed(&p, 2, 0, EOS_BTN_LEFT, 210); }
    eos_pointer_feed(&p, 0, 0, 0, 220);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_UP), 1, "a drag still releases");
    CKI(count_of(&c, EOS_EV_CLICK), 0, "but it is not a click");

    // Right at the slop boundary it still counts. One count is one pixel in
    // the unity zone, so this is exactly EOS_POINTER_SLOP pixels of travel.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT, 300);
    { int i; for (i = 0; i < EOS_POINTER_SLOP; i++) eos_pointer_feed(&p, 1, 0, EOS_BTN_LEFT, 310); }
    eos_pointer_feed(&p, 0, 0, 0, 320);
    drain(&c);
    CKI(count_of(&c, EOS_EV_CLICK), 1, "slop pixels of travel is still a click");

    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT, 400);
    { int i; for (i = 0; i < EOS_POINTER_SLOP + 1; i++) eos_pointer_feed(&p, 1, 0, EOS_BTN_LEFT, 410); }
    eos_pointer_feed(&p, 0, 0, 0, 420);
    drain(&c);
    CKI(count_of(&c, EOS_EV_CLICK), 0, "one pixel more is not");

    // Two buttons at once, released one at a time, each with its own down
    // position. They share one report and must not share one slot.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT | EOS_BTN_RIGHT, 500);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_DOWN), 2, "two buttons in one report is two presses");
    eos_pointer_feed(&p, 0, 0, EOS_BTN_RIGHT, 510);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_UP), 1, "releasing one releases exactly one");
    CK(find_ev(&c, EOS_EV_POINTER_UP, EOS_BTN_LEFT) != NULL, "the one that was let go");
    CKI(p.buttons, EOS_BTN_RIGHT, "the other is still held");

    // The middle button exists and is its own slot.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 0, 0, EOS_BTN_MIDDLE, 600);
    eos_pointer_feed(&p, 0, 0, 0, 610);
    drain(&c);
    CK(find_ev(&c, EOS_EV_CLICK, EOS_BTN_MIDDLE) != NULL, "the middle button clicks too");

    // A fourth button on a device with more than three is ignored rather than
    // written past the end of a three-entry array.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 0, 0, 0xFF, 700);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_DOWN), 3, "only the three boot-mouse buttons");
    CKI(p.buttons, EOS_BTN_LEFT | EOS_BTN_RIGHT | EOS_BTN_MIDDLE, "and only those are held");

    // Motion comes before the button events in the same report, so a release
    // carries the position it was released at.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT, 800);
    drain(&c);
    eos_pointer_feed(&p, 2, 0, 0, 810);
    drain(&c);
    CK(index_of(&c, EOS_EV_POINTER_MOVE) < index_of(&c, EOS_EV_POINTER_UP),
       "motion is pushed before the button event in the same report");
    {
        const eos_event_t *up = find_ev(&c, EOS_EV_POINTER_UP, EOS_BTN_LEFT);
        CKI(up ? up->x : -1, 122, "and the release carries the new position");
    }

    // A trackpad that walked out of range releases what it held and does NOT
    // invent a click out of it.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT, 900);
    drain(&c);
    eos_pointer_disconnect(&p, 910);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_UP), 1, "a disconnect releases the held button");
    CKI(count_of(&c, EOS_EV_CLICK), 0, "and never turns it into a click");
    CK(!eos_pointer_visible(&p, 910), "the arrow goes away with the device");

    eos_pointer_feed(NULL, 1, 1, 0, 1000);
    eos_pointer_disconnect(NULL, 1000);
    CK(true, "a null pointer is survivable");
    drain(&c);
}

// -------------------------------------------------------------- ring safety

static void ring(void)
{
    eos_pointer_t p;
    cap_t c;
    int i;

    printf("\n=== the ring under a swipe ===\n");

    // A real swipe is hundreds of reports and the ring is thirty-two events.
    // Without coalescing the first thirty-two would fill it and the click at
    // the end would be dropped, which is the one event that actually matters.
    eos_pointer_init(&p, 240, 240);
    drain(&c);
    for (i = 0; i < 200; i++) eos_pointer_feed(&p, (i & 1) ? 3 : -3, 1, 0, 1000);
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT, 1100);
    eos_pointer_feed(&p, 0, 0, 0, 1110);
    drain(&c);
    CK(count_of(&c, EOS_EV_POINTER_MOVE) <= 2, "two hundred reports coalesce to one motion");
    CKI(count_of(&c, EOS_EV_POINTER_DOWN), 1, "and the press still gets through");
    CKI(count_of(&c, EOS_EV_POINTER_UP), 1, "and the release");
    CKI(count_of(&c, EOS_EV_CLICK), 1, "and the click, which is the point");

    // The coalesced event is the NEWEST position, not the oldest.
    eos_pointer_init(&p, 240, 240);
    drain(&c);
    for (i = 0; i < 10; i++) eos_pointer_feed(&p, 1, 0, 0, 1200);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_MOVE), 1, "ten reports, one event");
    CKI(c.ev[0].x, p.x, "carrying where the cursor actually ended up");

    // A button event between two motions breaks the run, so the ordering of
    // press against motion is never lost to coalescing.
    eos_pointer_init(&p, 240, 240);
    drain(&c);
    eos_pointer_feed(&p, 1, 0, 0, 1300);
    eos_pointer_feed(&p, 0, 0, EOS_BTN_LEFT, 1310);
    eos_pointer_feed(&p, 1, 0, EOS_BTN_LEFT, 1320);
    drain(&c);
    CKI(count_of(&c, EOS_EV_POINTER_MOVE), 2, "a button event separates two motions");
    CK(index_of(&c, EOS_EV_POINTER_DOWN) == 1, "and the press keeps its place between them");
}

// ------------------------------------------------------------------- damage

static void damage(void)
{
    eos_pointer_t p;
    eos_rect_t r;
    cap_t c;

    printf("\n=== damage rects ===\n");

    eos_pointer_init(&p, 240, 240);
    CK(!eos_pointer_dirty(&p, 0), "an unseen cursor damages nothing");

    eos_pointer_feed(&p, 4, 0, 0, 100);
    CK(eos_pointer_dirty(&p, 100), "the first report is dirty: the arrow appeared");
    r = eos_pointer_rect(&p);
    CKI(r.w, EOS_POINTER_ARROW_W, "the box is the arrow's width");
    CKI(r.h, EOS_POINTER_ARROW_H, "and its height");
    CKI(r.x, p.x, "with the hot spot at its top-left corner");

    eos_pointer_commit(&p, 100);
    CK(!eos_pointer_dirty(&p, 100), "committing settles it");
    CKI(eos_pointer_drawn_rect(&p).x, p.x, "and records where it was drawn");

    eos_pointer_feed(&p, 1, 0, 0, 110);
    CK(eos_pointer_dirty(&p, 110), "moving one pixel is dirty again");
    CKI(eos_pointer_drawn_rect(&p).x, (int)p.x - 1, "the old box is still the old place");
    CKI(eos_pointer_rect(&p).x, p.x, "and the new box is the new one");

    // Clipped at the bottom-right corner: the box must not claim rows the
    // panel does not have, or the damage rect would be rejected and the arrow
    // would smear.
    eos_pointer_init(&p, 240, 240);
    { int i; for (i = 0; i < 40; i++) eos_pointer_feed(&p, 100, 100, 0, 200); }
    r = eos_pointer_rect(&p);
    CK(r.x + r.w <= 240, "the box is clipped to the right edge");
    CK(r.y + r.h <= 240, "and to the bottom");
    CK(r.w > 0 && r.h > 0, "and is still a real rect");

    // Going idle is dirty exactly once: the frame that stops drawing the arrow
    // has to repaint the hole it leaves, and the frames after it do not.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 1, 0, 0, 1000);
    eos_pointer_commit(&p, 1000);
    CK(eos_pointer_dirty(&p, 1000 + EOS_POINTER_IDLE_MS),
       "the arrow timing out is dirty");
    eos_pointer_commit(&p, 1000 + EOS_POINTER_IDLE_MS);
    CK(!eos_pointer_dirty(&p, 1000 + EOS_POINTER_IDLE_MS + 5000),
       "and stays settled once the hole is repainted");
    CK(rect_empty(eos_pointer_drawn_rect(&p)),
       "a hidden cursor's drawn box is empty");

    CK(rect_empty(eos_pointer_rect(NULL)), "a null cursor has no rect");
    CK(!eos_pointer_dirty(NULL, 0), "and is never dirty");
    eos_pointer_commit(NULL, 0);
    drain(&c);
}

// ------------------------------------------------------------------- latch
//
// The whole point of eos_pointer_latch(): the position is written from the BLE
// host task, which preempts the task that draws, and one frame reads it for
// the damage rects, once per band while the scene is replayed, and again at
// the commit. Every one of those has to see the SAME number or the arrow gets
// painted somewhere no damage rect covered and the pixels stay on the glass.
// Each report below stands in for one arriving mid-frame.

static void latching(void)
{
    eos_pointer_t p;
    eos_rect_t r;
    cap_t c;

    printf("\n=== the frame latch ===\n");

    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 10, 0, 0, 100);
    eos_pointer_commit(&p, 100);
    drain(&c);

    // Nothing armed: every answer is the live one, exactly as it was before
    // the latch existed. This is what keeps a caller that never latches, and
    // every check above, working unchanged.
    eos_pointer_feed(&p, 5, 0, 0, 110);
    CKI(eos_pointer_rect(&p).x, p.x, "with no latch the box is the live position");
    CK(eos_pointer_shown(&p, 110), "and visibility is the live answer too");

    // Latch, then let three more reports land. Everything the frame asks must
    // still describe the latched position.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 10, 4, 0, 100);
    eos_pointer_commit(&p, 100);
    eos_pointer_feed(&p, 6, 6, 0, 150);   // moved since the last frame drew
    eos_pointer_latch(&p, 200);
    {
        int16_t lx = p.x, ly = p.y;

        eos_pointer_feed(&p, 20, 20, 0, 205);
        eos_pointer_feed(&p, 20, 20, 0, 210);
        CK(p.x != lx, "the live position has moved on");

        r = eos_pointer_rect(&p);
        CKI(r.x, lx, "the frame's box is still the latched x");
        CKI(r.y, ly, "and the latched y");
        CK(eos_pointer_dirty(&p, 210), "the latched position is dirty against the drawn one");

        // The commit records what the bands actually painted, not where the
        // trackpad got to while they were being pushed. Getting this wrong is
        // what leaves the NEXT frame repainting the wrong hole.
        eos_pointer_commit(&p, 210);
        CKI(eos_pointer_drawn_rect(&p).x, lx, "the commit records the latched x");
        CKI(eos_pointer_drawn_rect(&p).y, ly, "and the latched y");
        CK(eos_pointer_dirty(&p, 210), "and the motion it missed is dirty for the next frame");

        // Disarmed by the commit, so the next frame reads live again until it
        // latches for itself.
        CKI(eos_pointer_rect(&p).x, p.x, "the commit disarmed the latch");
    }

    // A latch taken while the arrow is timed out keeps the frame from drawing
    // it even if a report lands mid-frame. The report is not lost: it makes
    // the NEXT frame's latch visible.
    eos_pointer_init(&p, 240, 240);
    eos_pointer_feed(&p, 3, 0, 0, 1000);
    eos_pointer_commit(&p, 1000);
    eos_pointer_latch(&p, 1000 + EOS_POINTER_IDLE_MS);
    CK(!eos_pointer_shown(&p, 1000 + EOS_POINTER_IDLE_MS), "a timed-out latch draws nothing");
    eos_pointer_feed(&p, 3, 0, 0, 1000 + EOS_POINTER_IDLE_MS + 1);
    CK(!eos_pointer_shown(&p, 1000 + EOS_POINTER_IDLE_MS + 1),
       "and a report arriving mid-frame does not bring it back inside that frame");
    eos_pointer_commit(&p, 1000 + EOS_POINTER_IDLE_MS + 1);
    eos_pointer_latch(&p, 1000 + EOS_POINTER_IDLE_MS + 2);
    CK(eos_pointer_shown(&p, 1000 + EOS_POINTER_IDLE_MS + 2),
       "the next frame's latch has it back");

    CK(!eos_pointer_shown(NULL, 0), "a null cursor is never shown");
    eos_pointer_latch(NULL, 0);
    drain(&c);
}

// ---------------------------------------------------------------- hit tests

// The 240x240 desktop the board actually boots: the C6's minimum tile size and
// the shipped theme's gap, bar and tab metrics. Five windows on this panel is
// what collapses a split into a tab strip, which is exactly the case worth
// clicking on.
static const eos_wm_cfg_t CFG_240 = { 70, 40, 2, 12, 10 };

static void dump_layout(const eos_tile_t *t, int n)
{
    int i;
    for (i = 0; i < n; i++)
        printf("    win %-2d %-8s rect %3d,%3d %3dx%-3d  tab %d/%d strip %3d,%3d %3dx%d\n",
               (int)t[i].win, t[i].visible ? "visible" : "tabbed",
               (int)t[i].rect.x, (int)t[i].rect.y, (int)t[i].rect.w, (int)t[i].rect.h,
               (int)t[i].tab_index, (int)t[i].tab_count,
               (int)t[i].tab_rect.x, (int)t[i].tab_rect.y,
               (int)t[i].tab_rect.w, (int)t[i].tab_rect.h);
}

static void hits(void)
{
    eos_wm_t    wm;
    eos_rect_t  screen = { 0, 0, 240, 240 };
    eos_tile_t  t[EOS_MAX_WINDOWS * 2];
    eos_hit_t   h;
    int n, i, w;

    printf("\n=== hit testing a real layout ===\n");

    eos_wm_init(&wm, &CFG_240);
    for (i = 0; i < 5; i++) (void)eos_wm_open(&wm, (uint16_t)i, screen);
    n = eos_wm_layout(&wm, screen, t, EOS_MAX_WINDOWS * 2);
    dump_layout(t, n);
    CK(n >= 5, "five windows lay out as at least five tiles");

    // Every visible tile: its centre, its top-left pixel and the pixel just
    // past its bottom-right corner.
    for (i = 0; i < n; i++) {
        if (!t[i].visible) continue;

        h = eos_pointer_hit(&wm, screen, NULL,
                            (int16_t)(t[i].rect.x + t[i].rect.w / 2),
                            (int16_t)(t[i].rect.y + t[i].rect.h / 2));
        CKI(h.kind, EOS_HIT_TILE, "the middle of a tile hits that tile");
        CKI(h.win, t[i].win, "and names the right window");

        h = eos_pointer_hit(&wm, screen, NULL, t[i].rect.x, t[i].rect.y);
        CKI(h.win, t[i].win, "the tile's own top-left pixel is inside it");

        h = eos_pointer_hit(&wm, screen, NULL,
                            (int16_t)(t[i].rect.x + t[i].rect.w - 1),
                            (int16_t)(t[i].rect.y + t[i].rect.h - 1));
        CKI(h.win, t[i].win, "and so is its bottom-right one");

        h = eos_pointer_hit(&wm, screen, NULL,
                            (int16_t)(t[i].rect.x + t[i].rect.w),
                            (int16_t)(t[i].rect.y + t[i].rect.h / 2));
        checks++;
        if (h.kind == EOS_HIT_TILE && h.win == t[i].win) {
            fails++;
            printf("    FAIL: the pixel past a tile's right edge is still in it\n");
        }
    }

    // The bar. It is above every tile and answers before the layout runs.
    h = eos_pointer_hit(&wm, screen, NULL, 120, 0);
    CKI(h.kind, EOS_HIT_BAR, "the top row is the status bar");
    CKI(h.win, EOS_NONE, "which belongs to no window");
    h = eos_pointer_hit(&wm, screen, NULL, 120, (int16_t)(CFG_240.bar_h - 1));
    CKI(h.kind, EOS_HIT_BAR, "and so is its last row");
    h = eos_pointer_hit(&wm, screen, NULL, 0, 0);
    CKI(h.kind, EOS_HIT_BAR, "including its left corner");

    // The gap. eos_wm insets every tile by cfg.gap, so the row immediately
    // under the bar is desktop and belongs to nobody. Clicking it must change
    // no focus at all.
    h = eos_pointer_hit(&wm, screen, NULL, 120, (int16_t)CFG_240.bar_h);
    CKI(h.kind, EOS_HIT_NONE, "the gap under the bar hits nothing");
    CKI(h.win, EOS_NONE, "and names no window");
    h = eos_pointer_hit(&wm, screen, NULL, 0, 239);
    CKI(h.kind, EOS_HIT_NONE, "and so does the screen's bottom-left corner");

    // The tab strip, cell by cell. Every window in a collapsed group reports
    // the same strip with its own index, so this walks the whole strip.
    w = 0;
    for (i = 0; i < n; i++) {
        eos_rect_t strip = t[i].tab_rect;
        int16_t cw, cx;

        if (strip.w <= 0 || strip.h <= 0 || t[i].tab_count < 2) continue;
        w++;

        cw = (int16_t)(strip.w / t[i].tab_count);
        cx = (int16_t)(strip.x + t[i].tab_index * cw);

        h = eos_pointer_hit(&wm, screen, NULL, (int16_t)(cx + cw / 2),
                            (int16_t)(strip.y + strip.h / 2));
        CKI(h.kind, EOS_HIT_TAB, "the middle of a tab cell is a tab");
        CKI(h.win, t[i].win, "and names the window that cell labels");
        CKI(h.tab_index, t[i].tab_index, "and its index in the strip");

        h = eos_pointer_hit(&wm, screen, NULL, cx, strip.y);
        CKI(h.win, t[i].win, "the cell's first pixel belongs to it");

        // The last cell absorbs the remainder of the division, so the strip
        // has no unclickable column. That is the same rule the renderer uses
        // and this is the check that the two agree.
        if (t[i].tab_index == t[i].tab_count - 1) {
            h = eos_pointer_hit(&wm, screen, NULL,
                                (int16_t)(strip.x + strip.w - 1),
                                (int16_t)(strip.y + strip.h / 2));
            CKI(h.win, t[i].win, "the last cell reaches the strip's last column");
        }
    }
    CK(w >= 2, "this layout really did collapse a split into a tab group");

    // Off the panel entirely.
    h = eos_pointer_hit(&wm, screen, NULL, -1, -1);
    CKI(h.kind, EOS_HIT_NONE, "a point off the top-left hits nothing");
    h = eos_pointer_hit(&wm, screen, NULL, 1000, 1000);
    CKI(h.kind, EOS_HIT_NONE, "and neither does one off the bottom-right");
    h = eos_pointer_hit(NULL, screen, NULL, 10, 10);
    CKI(h.kind, EOS_HIT_NONE, "and a null window manager is survivable");
}

// ------------------------------------------------------------------ clicks

static void clicks(void)
{
    eos_wm_t   wm;
    eos_rect_t screen = { 0, 0, 240, 240 };
    eos_tile_t t[EOS_MAX_WINDOWS * 2];
    eos_event_t e;
    int n, i, other = EOS_NONE;

    printf("\n=== clicking ===\n");

    eos_wm_init(&wm, &CFG_240);
    for (i = 0; i < 5; i++) (void)eos_wm_open(&wm, (uint16_t)i, screen);
    n = eos_wm_layout(&wm, screen, t, EOS_MAX_WINDOWS * 2);

    // A visible tile that is not the focused one.
    for (i = 0; i < n; i++)
        if (t[i].visible && t[i].win != wm.focus) { other = i; break; }
    CK(other >= 0, "there is a visible tile that is not focused");

    if (other >= 0) {
        int16_t want = t[other].win;
        CK(eos_pointer_click(&wm, screen, NULL,
                             (int16_t)(t[other].rect.x + t[other].rect.w / 2),
                             (int16_t)(t[other].rect.y + t[other].rect.h / 2)),
           "clicking another tile moves the focus");
        CKI(wm.focus, want, "to that window");

        CK(!eos_pointer_click(&wm, screen, NULL,
                              (int16_t)(t[other].rect.x + t[other].rect.w / 2),
                              (int16_t)(t[other].rect.y + t[other].rect.h / 2)),
           "clicking the focused tile again changes nothing");
    }

    // The bar and the gap are not clickable. They must not merely fail to
    // change focus - they must report that nothing needs redrawing.
    CK(!eos_pointer_click(&wm, screen, NULL, 120, 0), "the status bar is not clickable");
    CK(!eos_pointer_click(&wm, screen, NULL, 120, (int16_t)CFG_240.bar_h),
       "and neither is the gap");

    // A tab. Clicking the cell of a window that is currently BEHIND its group
    // must raise it - that is the second of the two things a click means.
    n = eos_wm_layout(&wm, screen, t, EOS_MAX_WINDOWS * 2);
    for (i = 0; i < n; i++) {
        eos_rect_t strip = t[i].tab_rect;
        int16_t cw, cx, want;
        int j, m;

        if (t[i].visible || strip.w <= 0 || t[i].tab_count < 2) continue;

        cw   = (int16_t)(strip.w / t[i].tab_count);
        cx   = (int16_t)(strip.x + t[i].tab_index * cw);
        want = t[i].win;

        CK(eos_pointer_click(&wm, screen, NULL, (int16_t)(cx + cw / 2),
                             (int16_t)(strip.y + strip.h / 2)),
           "clicking a hidden window's tab moves the focus");
        CKI(wm.focus, want, "to that window");

        // And it is now the one the layout draws, which is what "raise" means
        // inside a collapsed group.
        m = eos_wm_layout(&wm, screen, t, EOS_MAX_WINDOWS * 2);
        for (j = 0; j < m; j++)
            if (t[j].win == want) CK(t[j].visible, "and the tab it labels is now on top");
        break;
    }

    // Through the event door, which is what the shell's pump actually calls.
    memset(&e, 0, sizeof e);
    e.type = EOS_EV_CLICK;
    e.key  = EOS_BTN_LEFT;
    e.x    = 120;
    e.y    = 0;                       // the bar
    CK(!eos_pointer_event(&wm, screen, NULL, &e), "a click on the bar does nothing");

    n = eos_wm_layout(&wm, screen, t, EOS_MAX_WINDOWS * 2);
    other = EOS_NONE;
    for (i = 0; i < n; i++)
        if (t[i].visible && t[i].win != wm.focus) { other = i; break; }
    if (other >= 0) {
        e.x = (int16_t)(t[other].rect.x + t[other].rect.w / 2);
        e.y = (int16_t)(t[other].rect.y + t[other].rect.h / 2);
        CK(eos_pointer_event(&wm, screen, NULL, &e), "a left click on a tile focuses it");
        CKI(wm.focus, t[other].win, "the one under the arrow");

        // Right and middle travel through the ring for apps to bind, but they
        // mean nothing to a tiling window manager and must not move focus.
        eos_wm_focus_win(&wm, t[0].win);
        e.key = EOS_BTN_RIGHT;
        CK(!eos_pointer_event(&wm, screen, NULL, &e), "a right click moves no focus");
        e.key = EOS_BTN_MIDDLE;
        CK(!eos_pointer_event(&wm, screen, NULL, &e), "and neither does a middle one");
        CKI(wm.focus, t[0].win, "the focus stayed where it was");

        // Nor does anything that is not a click.
        e.key  = EOS_BTN_LEFT;
        e.type = EOS_EV_POINTER_MOVE;
        CK(!eos_pointer_event(&wm, screen, NULL, &e), "motion alone focuses nothing");
        e.type = EOS_EV_POINTER_DOWN;
        CK(!eos_pointer_event(&wm, screen, NULL, &e), "and neither does a press on its own");
        e.type = EOS_EV_KEY_DOWN;
        CK(!eos_pointer_event(&wm, screen, NULL, &e), "and a keystroke is not a click");
        CKI(wm.focus, t[0].win, "through all of which the focus did not move");
    }

    CK(!eos_pointer_event(NULL, screen, NULL, &e), "a null window manager is survivable");
    CK(!eos_pointer_event(&wm, screen, NULL, NULL), "and so is a null event");
}

// --------------------------------------------------------------- end to end

// One trackpad swipe, from the bytes on the wire to a focus change, with
// nothing faked in between: the same decode the notify path calls, the same
// cursor the frame loop draws, and the same event door the shell's pump reads.
static void wire(void)
{
    eos_wm_t    wm;
    eos_rect_t  screen = { 0, 0, 240, 240 };
    eos_tile_t  t[EOS_MAX_WINDOWS * 2];
    eos_pointer_t p;
    cap_t c;
    int n, i, target = EOS_NONE;

    printf("\n=== wire to focus ===\n");

    eos_wm_init(&wm, &CFG_240);
    for (i = 0; i < 5; i++) (void)eos_wm_open(&wm, (uint16_t)i, screen);
    n = eos_wm_layout(&wm, screen, t, EOS_MAX_WINDOWS * 2);
    for (i = 0; i < n; i++)
        if (t[i].visible && t[i].win != wm.focus) { target = i; break; }
    CK(target >= 0, "there is somewhere to click");
    if (target < 0) return;

    eos_pointer_init(&p, 240, 240);
    drain(&c);

    // Walk the cursor onto the target tile one real report at a time, then
    // press and release. Reports are built as bytes and decoded, so a sign
    // error anywhere in the chain shows up here as a cursor in the wrong tile.
    {
        int16_t wantx = (int16_t)(t[target].rect.x + t[target].rect.w / 2);
        int16_t wanty = (int16_t)(t[target].rect.y + t[target].rect.h / 2);
        int guard = 0;

        while ((p.x != wantx || p.y != wanty) && guard++ < 2000) {
            uint8_t rep[3];
            eos_ble_mouse_t m;
            int dx = wantx - p.x, dy = wanty - p.y;

            if (dx >  1) dx =  1;
            if (dx < -1) dx = -1;
            if (dy >  1) dy =  1;
            if (dy < -1) dy = -1;

            rep[0] = 0;
            rep[1] = (uint8_t)(int8_t)dx;
            rep[2] = (uint8_t)(int8_t)dy;
            if (!eos_ble_decode_mouse(rep, 3, &m)) break;
            eos_pointer_feed(&p, m.dx, m.dy, m.buttons, 1000);
        }
        CKI(p.x, wantx, "the cursor walked to the target's x");
        CKI(p.y, wanty, "and to its y, which needs left and up to work");
    }

    {
        uint8_t down[3] = { EOS_BTN_LEFT, 0, 0 };
        uint8_t up[3]   = { 0, 0, 0 };
        eos_ble_mouse_t m;
        const eos_event_t *click;

        drain(&c);
        (void)eos_ble_decode_mouse(down, 3, &m);
        eos_pointer_feed(&p, m.dx, m.dy, m.buttons, 1100);
        (void)eos_ble_decode_mouse(up, 3, &m);
        eos_pointer_feed(&p, m.dx, m.dy, m.buttons, 1110);
        drain(&c);

        click = find_ev(&c, EOS_EV_CLICK, EOS_BTN_LEFT);
        CK(click != NULL, "the tap produced a click event");
        if (click) {
            CK(eos_pointer_event(&wm, screen, NULL, click), "which the shell's pump acts on");
            CKI(wm.focus, t[target].win, "and the window under the arrow is focused");
        }
    }
}


// The close box: the pure geometry, and the one ordering rule that makes it
// reachable at all. It lives inside its tile's rect, so a hit test that
// answered tiles first would report EOS_HIT_TILE for every pixel of it and the
// x would be decoration.
static void close_box(void)
{
    eos_wm_t    wm;
    eos_rect_t  screen = { 0, 0, 240, 240 };
    eos_tile_t  t[EOS_MAX_WINDOWS * 2];
    eos_pointer_chrome_t ch;
    eos_hit_t   h;
    int n, i, boxes = 0;

    printf("\n=== the close box ===\n");

    // The board's real numbers: a one-pixel border and the 6x8 UI face.
    ch.border = 1; ch.hdr_h = 8; ch.close_w = 11;

    eos_wm_init(&wm, &CFG_240);
    for (i = 0; i < 5; i++) (void)eos_wm_open(&wm, (uint16_t)i, screen);
    n = eos_wm_layout(&wm, screen, t, EOS_MAX_WINDOWS * 2);

    for (i = 0; i < n; i++) {
        eos_rect_t b = eos_pointer_close_box(&ch, &t[i]);

        if (!t[i].visible) {
            CK(b.w == 0 && b.h == 0, "a tile behind a tab has no close box");
            continue;
        }
        CK(b.w == ch.close_w, "a visible tile's box is exactly close_w wide");
        boxes++;

        // Inside its tile, hard against the right edge, starting at the very
        // top of the tile so the border rows are part of the target.
        CK(b.x >= t[i].rect.x && b.x + b.w <= t[i].rect.x + t[i].rect.w,
           "and inside that tile horizontally");
        CKI(b.x + b.w, t[i].rect.x + t[i].rect.w - (ch.border + 1),
            "hard against the inner right edge");
        CKI(b.y, t[i].rect.y, "starting at the tile's own top edge");
        CKI(b.h, ch.border + 1 + ch.hdr_h, "and as tall as the border plus the header");

        // Every corner of it answers CLOSE, not TILE.
        h = eos_pointer_hit(&wm, screen, &ch, b.x, b.y);
        CKI(h.kind, EOS_HIT_CLOSE, "its top-left pixel is the close box");
        CKI(h.win, t[i].win, "and names the window it belongs to");
        h = eos_pointer_hit(&wm, screen, &ch,
                            (int16_t)(b.x + b.w - 1), (int16_t)(b.y + b.h - 1));
        CKI(h.kind, EOS_HIT_CLOSE, "and so does its bottom-right one");

        // One pixel to the left of it, and one row below it, are the tile.
        h = eos_pointer_hit(&wm, screen, &ch, (int16_t)(b.x - 1), b.y);
        CKI(h.kind, EOS_HIT_TILE, "the pixel left of the box is the tile again");
        h = eos_pointer_hit(&wm, screen, &ch,
                            (int16_t)(b.x + b.w / 2), (int16_t)(b.y + b.h));
        CKI(h.kind, EOS_HIT_TILE, "and so is the row under it");

        // The SAME point with no chrome is the tile, which is what every
        // caller written before there was a close box still sees.
        h = eos_pointer_hit(&wm, screen, NULL, b.x, b.y);
        CKI(h.kind, EOS_HIT_TILE, "with no chrome that point is simply the tile");
    }
    CK(boxes >= 2, "at least two visible tiles carried a box");

    // And it closes. Through eos_wm_close(), the same call super+q makes.
    for (i = 0; i < n; i++) {
        eos_rect_t b;
        if (!t[i].visible) continue;
        b = eos_pointer_close_box(&ch, &t[i]);
        CK(eos_pointer_click(&wm, screen, &ch,
                             (int16_t)(b.x + b.w / 2), (int16_t)(b.y + b.h / 2)),
           "clicking a close box is a change worth redrawing");
        CK(!wm.win[t[i].win].alive, "and the window is gone");
        break;
    }

    // A board that never set a chrome cannot close anything by accident: the
    // same click focuses instead.
    {
        eos_wm_t w2;
        eos_tile_t t2[EOS_MAX_WINDOWS * 2];
        eos_rect_t b;
        int n2;

        eos_wm_init(&w2, &CFG_240);
        for (i = 0; i < 3; i++) (void)eos_wm_open(&w2, (uint16_t)i, screen);
        n2 = eos_wm_layout(&w2, screen, t2, EOS_MAX_WINDOWS * 2);
        CK(n2 >= 3, "three windows for the no-chrome case");
        b = eos_pointer_close_box(&ch, &t2[0]);
        (void)eos_pointer_click(&w2, screen, NULL,
                                (int16_t)(b.x + b.w / 2), (int16_t)(b.y + b.h / 2));
        CK(w2.win[t2[0].win].alive, "with no chrome that click closes nothing");
    }

    // A zero close_w is the same as no chrome, which is how a panel too narrow
    // to spare the pixels turns the whole feature off.
    {
        eos_pointer_chrome_t off = ch;
        off.close_w = 0;
        for (i = 0; i < n; i++)
            CK(eos_pointer_close_box(&off, &t[i]).w == 0,
               "close_w 0 gives no tile a box");
    }
}

int main(void)
{
    eos_input_init(NULL);

    decode();
    curve();
    motion();
    buttons();
    ring();
    damage();
    latching();
    hits();
    close_box();
    clicks();
    wire();

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
