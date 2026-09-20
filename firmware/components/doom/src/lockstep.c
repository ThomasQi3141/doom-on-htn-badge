// The tic exchange. See lockstep.h for the model.

#include "lockstep.h"

#include <string.h>

// Little-endian by hand rather than by struct: the frame is a wire format
// shared between two builds, and packed structs have bitten this port before.
//
// Frame layout, 31 bytes at LOCKSTEP_SPAN = 3:
//
//   0      type (LOCKSTEP_TIC)
//   1      how many cmds follow, 1..LOCKSTEP_SPAN
//   2..5   tic index of the first cmd
//   6..9   the lowest tic of yours I have not received
//   10..   the cmds, 7 bytes each

static void put_cmd(uint8_t *p, const lockstep_cmd_t *c)
{
    p[0] = (uint8_t)c->forwardmove;
    p[1] = (uint8_t)c->sidemove;
    p[2] = (uint8_t)((uint16_t)c->angleturn & 0xff);
    p[3] = (uint8_t)(((uint16_t)c->angleturn >> 8) & 0xff);
    p[4] = c->chatchar;
    p[5] = c->buttons;
    p[6] = c->consistancy;
}

static void get_cmd(const uint8_t *p, lockstep_cmd_t *c)
{
    c->forwardmove = (int8_t)p[0];
    c->sidemove    = (int8_t)p[1];
    c->angleturn   = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
    c->chatchar    = p[4];
    c->buttons     = p[5];
    c->consistancy = p[6];
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void lockstep_reset(lockstep_t *ls)
{
    memset(ls, 0, sizeof *ls);
}

// Which run of our tics to put in the next frame.
//
// The oldest tic the peer is missing comes first: without it the peer cannot
// run anything, however new the rest is. Normally that is the tic we have
// just built and the frame is simply the newest few. Once the peer has fallen
// behind -- a lost frame, a slow display, a level load -- it is an older one,
// and aiming the frame there is what gets the pair moving again.
static int frame_range(const lockstep_t *ls, uint32_t *first)
{
    uint32_t start = ls->peer_need;
    uint32_t held = ls->hist_next - ls->hist_first;
    uint32_t avail;

    if (held == 0)
        return 0;

    if ((int32_t)(start - ls->hist_first) < 0)
        start = ls->hist_first;         // older than we keep: send what we have
    if ((int32_t)(start - ls->hist_next) >= 0)
        start = ls->hist_next - 1;      // they are up to date: send the newest

    avail = ls->hist_next - start;
    *first = start;
    return avail < LOCKSTEP_SPAN ? (int)avail : LOCKSTEP_SPAN;
}

static int build(const lockstep_t *ls, uint8_t *out)
{
    uint32_t first = 0;
    int count = frame_range(ls, &first);

    if (count == 0)
        return 0;

    out[0] = LOCKSTEP_TIC;
    out[1] = (uint8_t)count;
    put_u32(out + 2, first);
    put_u32(out + 6, ls->next);

    for (int i = 0; i < count; i++)
    {
        uint32_t tic = first + (uint32_t)i;
        put_cmd(out + LOCKSTEP_HDR_BYTES + i * LOCKSTEP_CMD_BYTES,
                &ls->history[tic % LOCKSTEP_HISTORY]);
    }

    return LOCKSTEP_HDR_BYTES + count * LOCKSTEP_CMD_BYTES;
}

int lockstep_pack(lockstep_t *ls, uint32_t tic, const lockstep_cmd_t *cmd,
                  void *out)
{
    if (ls->hist_next == ls->hist_first || tic != ls->hist_next)
    {
        // First tic of the session, or the caller skipped one. Either way the
        // history no longer describes a run of consecutive tics.
        ls->hist_first = tic;
        ls->hist_next = tic;
        if ((int32_t)(ls->peer_need - tic) < 0)
            ls->peer_need = tic;
    }

    ls->history[tic % LOCKSTEP_HISTORY] = *cmd;
    ls->hist_next = tic + 1;
    if (ls->hist_next - ls->hist_first > LOCKSTEP_HISTORY)
        ls->hist_first = ls->hist_next - LOCKSTEP_HISTORY;

    return build(ls, (uint8_t *)out);
}

int lockstep_repack(const lockstep_t *ls, void *out)
{
    return build(ls, (uint8_t *)out);
}

bool lockstep_unpack(lockstep_t *ls, const void *in, int len)
{
    const uint8_t *p = (const uint8_t *)in;

    if (len < LOCKSTEP_HDR_BYTES || p[0] != LOCKSTEP_TIC)
    {
        ls->rx_bad++;
        return false;
    }

    int count = p[1];
    if (count < 1 || count > LOCKSTEP_SPAN
        || len < LOCKSTEP_HDR_BYTES + count * LOCKSTEP_CMD_BYTES)
    {
        ls->rx_bad++;
        return false;
    }

    uint32_t first = get_u32(p + 2);
    uint32_t need = get_u32(p + 6);
    ls->rx_frames++;

    // Frames reorder, so only ever move the peer's request forward.
    if ((int32_t)(need - ls->peer_need) > 0)
        ls->peer_need = need;

    for (int i = 0; i < count; i++)
    {
        uint32_t tic = first + (uint32_t)i;

        // Signed difference: a tic already run, and one so far ahead that
        // holding it would mean overwriting a tic still owed to the game,
        // are both refused rather than stored in the wrong slot.
        int32_t delta = (int32_t)(tic - ls->next);
        if (delta < 0)
        {
            ls->rx_stale++;
            continue;
        }
        if (delta >= LOCKSTEP_WINDOW)
        {
            ls->rx_beyond++;
            continue;
        }

        unsigned slot = tic % LOCKSTEP_WINDOW;
        if (ls->have[slot])
        {
            ls->rx_dup++;
            continue;
        }

        get_cmd(p + LOCKSTEP_HDR_BYTES + i * LOCKSTEP_CMD_BYTES,
                &ls->window[slot]);
        ls->have[slot] = true;
        if (tic + 1 > ls->highest)
            ls->highest = tic + 1;
    }

    return true;
}

bool lockstep_next(lockstep_t *ls, lockstep_cmd_t *out)
{
    unsigned slot = ls->next % LOCKSTEP_WINDOW;
    if (!ls->have[slot])
        return false;

    *out = ls->window[slot];
    ls->have[slot] = false;
    ls->next++;
    ls->delivered++;
    return true;
}
