/*
 * history.h — SoC over the last 24 hours, one point a minute, for the graph.
 *
 * The monitor keeps no history of its own (its window is statistics, not a series),
 * so the remote records what it sees. RAM only: 1440 points is under 3 KB, and a
 * remote that reboots starts a fresh graph rather than drawing a stale one against a
 * clock it does not have.
 */
#pragma once

#include <stdint.h>

#define HISTORY_POINTS 1440 /* minutes in a day */
#define HISTORY_NONE   0xFFFF

/** One point per call; the caller calls once a minute. HISTORY_NONE marks a minute
 *  with no data -- the link was down -- so the graph shows a gap, not a straight line. */
void history_push(uint16_t soc_permille);

/** The last `minutes` points, oldest first, into out[minutes]. Minutes before the
 *  first recorded point read as HISTORY_NONE. */
void history_get(uint16_t *out, int minutes);

/** How many points have been recorded, capped at HISTORY_POINTS. */
int history_count(void);
