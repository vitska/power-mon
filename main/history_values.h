/*
 * history_values.h — accumulated and time-series quantities, as opposed to the
 * instantaneous ones in values.h.
 *
 * Today that means the rolling sample window: the running statistics the
 * zero-current calibration is built on (DESIGN.md §5.5) and the noise figure that
 * says whether a reading can be trusted at all. Both answer questions about a SPAN
 * of samples rather than about the latest one, which is the line this module draws.
 *
 * The window is deliberately not a ring buffer of samples. Sums and extrema answer
 * every question the firmware actually asks -- mean, sigma, min, max -- in constant
 * memory, and a device that has to keep 4096 samples to report their standard
 * deviation has chosen the wrong representation. The cost is that the window cannot
 * be re-analysed after the fact, which is what the CSV stream is for: a client that
 * wants the raw series already has it.
 *
 * Voltage carries its own count. It is read on its own divisor (§4.5), so a shared
 * n would silently divide the voltage sum by the current's sample count.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sensors.h"

/** Running statistics over a window of samples — the basis of the zero-current
 *  calibration (DESIGN.md §5.5) and of judging noise during bring-up. */
typedef struct {
    uint32_t n;
    int64_t  sum_ua;
    int64_t  sum_sq_ua;   /* in (uA)^2; int64 holds ~9e18, so |i| up to ~3 A over
                           * 1e6 samples stays in range with room to spare */
    int32_t  min_ua;
    int32_t  max_ua;
    uint32_t n_v;         /* fresh voltage samples; voltage is read on its own
                           * divisor (DESIGN.md 4.5) so it has its own count */
    int64_t  sum_uv;
    uint32_t min_uv;
    uint32_t max_uv;
} sample_stats_t;

void    stats_reset(sample_stats_t *s);
void    stats_add(sample_stats_t *s, const power_sample_t *smp);
int32_t stats_mean_ua(const sample_stats_t *s);
int32_t stats_stddev_ua(const sample_stats_t *s);
int32_t stats_mean_uv(const sample_stats_t *s);

/** The window the sampler feeds and `stats` reports on. An accessor for the same
 *  reason values() is one: it is where a lock goes when one is needed. */
sample_stats_t *history_window(void);
