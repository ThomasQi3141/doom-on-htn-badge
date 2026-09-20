// Ten minutes of two-badge co-op, on a host, over a channel that loses,
// duplicates and reorders frames.
//
// The ticket asks for ten minutes of play with dropped frames on one side and
// no consistency failure and no freeze. That is not a thing two badges on a
// bench can be made to do on demand, and it is exactly what the protocol in
// lockstep.c can be asked directly -- so this runs 21,000 tics (10 minutes at
// 35 Hz) of it, with one badge running at half the other's frame rate, and
// checks the two properties that matter:
//
//   consistency  every tic each side hands to its engine is the cmd the other
//                side actually built for that tic, in order and exactly once;
//   liveness     both sides reach tic 21,000 -- nothing wedges waiting for a
//                frame that will never come again.
//
// The loop below mirrors d_loop.c: a tic is built only while fewer than five
// are outstanding, and a tic runs only once the peer's cmd for it is in hand.

#include "lockstep.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICS          21000     // 35 Hz for ten minutes
#define MAX_AHEAD     5         // d_loop.c's old-sync limit on maketic
#define RESEND_STEPS  2         // a blocked side resends after this long
#define INFLIGHT      256

static unsigned failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            if (++failures > 20) { printf("too many failures\n"); exit(1); }  \
        }                                                                     \
    } while (0)

// ------------------------------------------------------------ deterministic

static uint32_t rng_state = 0x1d872b41;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int rnd_pct(void)
{
    return (int)(rnd() % 100);
}

// The cmd a given side builds for a given tic. Both sides can compute the
// other's, which is what lets the test check every delivered cmd rather than
// only that something arrived.
static lockstep_cmd_t cmd_for(int side, uint32_t tic)
{
    uint32_t h = (uint32_t)side * 2654435761u + tic * 2246822519u;
    lockstep_cmd_t c;
    h ^= h >> 15;
    c.forwardmove = (int8_t)(h & 0xff);
    c.sidemove    = (int8_t)((h >> 8) & 0xff);
    c.angleturn   = (int16_t)((h >> 11) & 0xffff);
    c.chatchar    = (uint8_t)((h >> 5) & 0x7f);
    c.buttons     = (uint8_t)((h >> 3) & 0xff);
    c.consistancy = (uint8_t)((h >> 19) & 0xff);
    return c;
}

static bool is_cmd(const lockstep_cmd_t *got, int side, uint32_t tic);

static bool cmd_eq(const lockstep_cmd_t *a, const lockstep_cmd_t *b)
{
    return a->forwardmove == b->forwardmove && a->sidemove == b->sidemove
        && a->angleturn == b->angleturn && a->chatchar == b->chatchar
        && a->buttons == b->buttons && a->consistancy == b->consistancy;
}

static bool is_cmd(const lockstep_cmd_t *got, int side, uint32_t tic)
{
    lockstep_cmd_t want = cmd_for(side, tic);
    return cmd_eq(got, &want);
}

// ------------------------------------------------------------ the channel

typedef struct
{
    int      to;            // side it is addressed to
    long     due;           // step it arrives on
    int      len;
    uint8_t  data[LOCKSTEP_MAX_FRAME];
    bool     used;
} packet_t;

typedef struct
{
    packet_t p[INFLIGHT];
    int      loss_pct;
    int      dup_pct;
    int      reorder_pct;
    long     sent, lost, duped;
} channel_t;

static void chan_send(channel_t *ch, int to, long now, const void *data, int len)
{
    ch->sent++;

    for (int copies = 1 + (rnd_pct() < ch->dup_pct ? 1 : 0); copies > 0; copies--)
    {
        if (rnd_pct() < ch->loss_pct)
        {
            ch->lost++;
            continue;
        }
        if (copies == 2)
            ch->duped++;

        // A reordered frame lands a step or two late; everything else lands
        // on the next step.
        long due = now + 1;
        if (rnd_pct() < ch->reorder_pct)
            due += 1 + (long)(rnd() % 2);

        int slot = -1;
        for (int i = 0; i < INFLIGHT; i++)
            if (!ch->p[i].used) { slot = i; break; }
        if (slot < 0)
        {
            ch->lost++;             // a full channel is another way to lose
            continue;
        }

        ch->p[slot].used = true;
        ch->p[slot].to = to;
        ch->p[slot].due = due;
        ch->p[slot].len = len;
        memcpy(ch->p[slot].data, data, (size_t)len);
    }
}

// ------------------------------------------------------------ a badge

typedef struct
{
    int        side;
    lockstep_t ls;
    uint32_t   maketic;         // next tic to build
    uint32_t   gametic;         // next tic to run
    long       last_send;
    long       blocked_steps;   // longest run of steps making no progress
    long       blocked_now;
} badge_t;

static void badge_init(badge_t *b, int side)
{
    memset(b, 0, sizeof *b);
    b->side = side;
    lockstep_reset(&b->ls);
    b->last_send = -1000;
}

// One pass of the engine's loop: take what has arrived, run what can be run,
// build and send a new tic if we are allowed to.
static void badge_step(badge_t *b, channel_t *ch, long now)
{
    uint8_t frame[LOCKSTEP_MAX_FRAME];
    lockstep_cmd_t c;
    int len;
    uint32_t before = b->gametic;

    // NetUpdate: drain the radio.
    for (int i = 0; i < INFLIGHT; i++)
    {
        packet_t *p = &ch->p[i];
        if (p->used && p->to == b->side && p->due <= now)
        {
            p->used = false;
            lockstep_unpack(&b->ls, p->data, p->len);
        }
    }

    // D_ReceiveTic: a tic runs only when both cmds for it are in hand, and
    // our own is in hand exactly when we have built it.
    while (b->gametic < b->maketic && lockstep_next(&b->ls, &c))
    {
        lockstep_cmd_t want = cmd_for(b->side ^ 1, b->gametic);
        CHECK(cmd_eq(&c, &want),
              "side %d tic %u: delivered cmd is not the one the peer built",
              b->side, b->gametic);
        b->gametic++;
    }

    // BuildNewTic, with d_loop's limit on running ahead.
    if (b->maketic - b->gametic < MAX_AHEAD)
    {
        lockstep_cmd_t mine = cmd_for(b->side, b->maketic);
        len = lockstep_pack(&b->ls, b->maketic, &mine, frame);
        chan_send(ch, b->side ^ 1, now, frame, len);
        b->last_send = now;
        b->maketic++;
    }
    else if (now - b->last_send >= RESEND_STEPS)
    {
        // Blocked: the peer may be waiting on a tic of ours whose every copy
        // was lost, and while we are blocked nothing new would replace it.
        len = lockstep_repack(&b->ls, frame);
        if (len > 0)
            chan_send(ch, b->side ^ 1, now, frame, len);
        b->last_send = now;
    }

    if (b->gametic == before)
    {
        if (++b->blocked_now > b->blocked_steps)
            b->blocked_steps = b->blocked_now;
    }
    else
    {
        b->blocked_now = 0;
    }
}

// ------------------------------------------------------------ the runs

static int run_session(const char *name, int loss_pct, int dup_pct,
                       int reorder_pct, int slow_side)
{
    channel_t ch;
    badge_t a, b;
    long step;
    const long max_steps = TICS * 40L;

    memset(&ch, 0, sizeof ch);
    ch.loss_pct = loss_pct;
    ch.dup_pct = dup_pct;
    ch.reorder_pct = reorder_pct;

    badge_init(&a, 0);
    badge_init(&b, 1);

    for (step = 0; step < max_steps; step++)
    {
        if (a.gametic >= TICS && b.gametic >= TICS)
            break;

        // "Dropped frames on one side": the slow badge only gets a turn every
        // other step, so it builds and consumes tics at half the rate.
        if (slow_side != 0 || (step & 1) == 0)
            badge_step(&a, &ch, step);
        if (slow_side != 1 || (step & 1) == 0)
            badge_step(&b, &ch, step);
    }

    CHECK(a.gametic >= TICS && b.gametic >= TICS,
          "%s: stalled at %u / %u tics after %ld steps",
          name, a.gametic, b.gametic, step);

    // A tic arriving past the window would be a gap the protocol cannot fill,
    // and the next lockstep_next would wait for it forever.
    CHECK(a.ls.rx_beyond == 0 && b.ls.rx_beyond == 0,
          "%s: %lu/%lu tics arrived outside the window",
          name, (unsigned long)a.ls.rx_beyond, (unsigned long)b.ls.rx_beyond);
    CHECK(a.ls.rx_bad == 0 && b.ls.rx_bad == 0, "%s: malformed frames", name);
    CHECK(a.ls.delivered == a.gametic && b.ls.delivered == b.gametic,
          "%s: delivered %lu/%lu but ran %u/%u", name,
          (unsigned long)a.ls.delivered, (unsigned long)b.ls.delivered,
          a.gametic, b.gametic);

    printf("  %-28s %6ld steps  frames %ld sent, %ld lost (%.1f%%), %ld duped;"
           " longest stall %ld/%ld steps\n",
           name, step, ch.sent, ch.lost,
           ch.sent ? 100.0 * ch.lost / ch.sent : 0.0, ch.duped,
           a.blocked_steps, b.blocked_steps);
    return 0;
}

// ------------------------------------------------------------ unit checks

static void frame_checks(void)
{
    lockstep_t ls;
    uint8_t f1[LOCKSTEP_MAX_FRAME], f2[LOCKSTEP_MAX_FRAME];
    uint8_t f3[LOCKSTEP_MAX_FRAME];
    lockstep_cmd_t c, got;
    int l1, l2, l3;

    // A frame must fit what the radio will carry.
    lockstep_reset(&ls);
    for (uint32_t t = 0; t < 8; t++)
    {
        c = cmd_for(0, t);
        int len = lockstep_pack(&ls, t, &c, f1);
        CHECK(len <= LOCKSTEP_MAX_FRAME, "frame of %d bytes exceeds the radio's %d",
              len, LOCKSTEP_MAX_FRAME);
    }

    // Every field survives the round trip, sign included.
    lockstep_reset(&ls);
    c.forwardmove = -50; c.sidemove = 40; c.angleturn = -3000;
    c.chatchar = 'x'; c.buttons = 0xa5; c.consistancy = 0xff;
    l1 = lockstep_pack(&ls, 0, &c, f1);
    lockstep_reset(&ls);
    CHECK(lockstep_unpack(&ls, f1, l1), "a good frame was rejected");
    CHECK(lockstep_next(&ls, &got), "nothing delivered from a good frame");
    CHECK(cmd_eq(&c, &got), "a cmd did not survive the round trip");

    // Out of order, then the duplicate, then the gap-filler.
    lockstep_reset(&ls);
    c = cmd_for(0, 0); l1 = lockstep_pack(&ls, 0, &c, f1);
    c = cmd_for(0, 1); l2 = lockstep_pack(&ls, 1, &c, f2);
    c = cmd_for(0, 2); l3 = lockstep_pack(&ls, 2, &c, f3);

    lockstep_reset(&ls);
    lockstep_unpack(&ls, f3, l3);           // carries tics 0,1,2
    CHECK(lockstep_next(&ls, &got) && is_cmd(&got, 0, 0),
          "the newest frame did not carry the older tics");
    lockstep_unpack(&ls, f1, l1);           // tic 0 again, already run
    CHECK(ls.rx_stale == 1, "a tic already run was not counted stale (%lu)",
          (unsigned long)ls.rx_stale);
    lockstep_unpack(&ls, f2, l2);           // tics 0,1: one stale, one held
    CHECK(ls.rx_dup >= 1, "a tic already held was not counted duplicate");
    CHECK(lockstep_next(&ls, &got) && is_cmd(&got, 0, 1),
          "tic 1 did not come out next");
    CHECK(lockstep_next(&ls, &got) && is_cmd(&got, 0, 2),
          "tic 2 did not come out next");
    CHECK(!lockstep_next(&ls, &got), "a tic was delivered that never arrived");

    // Rubbish is refused rather than parsed.
    lockstep_reset(&ls);
    uint8_t junk[LOCKSTEP_MAX_FRAME];
    memset(junk, 0, sizeof junk);
    junk[0] = 99; junk[1] = 1;
    CHECK(!lockstep_unpack(&ls, junk, LOCKSTEP_HDR_BYTES + LOCKSTEP_CMD_BYTES),
          "a foreign frame was accepted");
    junk[0] = LOCKSTEP_TIC; junk[1] = 200;
    CHECK(!lockstep_unpack(&ls, junk, LOCKSTEP_HDR_BYTES + LOCKSTEP_CMD_BYTES),
          "an overlong count was accepted");
    junk[1] = LOCKSTEP_SPAN;
    CHECK(!lockstep_unpack(&ls, junk, LOCKSTEP_HDR_BYTES + LOCKSTEP_CMD_BYTES),
          "a frame shorter than its own count was accepted");
    junk[1] = 1;
    CHECK(!lockstep_unpack(&ls, junk, LOCKSTEP_HDR_BYTES - 1),
          "a frame too short for a header was accepted");
    CHECK(ls.rx_bad == 4, "malformed frames not counted (%lu)",
          (unsigned long)ls.rx_bad);
}

int main(void)
{
    printf("lockstep: frame checks\n");
    frame_checks();

    printf("lockstep: %d tics (%d minutes of play) per run\n",
           TICS, TICS / 35 / 60);
    run_session("clean link", 0, 0, 0, -1);
    run_session("5% loss", 5, 0, 0, -1);
    run_session("20% loss, reordering", 20, 5, 10, -1);
    run_session("20% loss, one slow badge", 20, 5, 10, 1);
    run_session("40% loss, one slow badge", 40, 10, 15, 1);

    if (failures > 0)
    {
        printf("\n%u check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
