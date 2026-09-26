/*
 * history.c — see history.h. Only the UI task touches it, so it needs no lock.
 */

#include "history.h"

static uint16_t s_ring[HISTORY_POINTS];
static int      s_head;  /* next slot to write */
static int      s_count;

void history_push(uint16_t soc_permille)
{
    s_ring[s_head] = soc_permille;
    s_head         = (s_head + 1) % HISTORY_POINTS;
    if (s_count < HISTORY_POINTS) {
        s_count++;
    }
}

void history_get(uint16_t *out, int minutes)
{
    if (minutes > HISTORY_POINTS) {
        minutes = HISTORY_POINTS;
    }
    for (int i = 0; i < minutes; i++) {
        const int age = minutes - 1 - i; /* 0 = newest */
        if (age >= s_count) {
            out[i] = HISTORY_NONE;
        } else {
            out[i] = s_ring[(s_head - 1 - age + HISTORY_POINTS) % HISTORY_POINTS];
        }
    }
}

int history_count(void)
{
    return s_count;
}
