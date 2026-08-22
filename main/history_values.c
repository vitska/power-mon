/*
 * history_values.c — see history_values.h.
 */

#include "history_values.h"

#include <stdlib.h>
#include <string.h>

/*
 * Integer square root, Newton's method. Avoiding sqrt() keeps the float ban whole
 * (DESIGN.md §4.3) and costs nothing at these call rates.
 *
 * The loop condition is `y < g`, not `y != g`: the naive form oscillates forever
 * between two adjacent values for inputs whose root is not exact, which on a
 * console command would simply hang the calling task.
 */
static uint64_t isqrt64(uint64_t x)
{
    if (x == 0) {
        return 0;
    }
    uint64_t g = x;
    uint64_t y = (g + 1) / 2;
    while (y < g) {
        g = y;
        y = (g + x / g) / 2;
    }
    return g;
}

static sample_stats_t s_window;

sample_stats_t *history_window(void)
{
    return &s_window;
}

void stats_reset(sample_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    s->min_ua = INT32_MAX;
    s->max_ua = INT32_MIN;
    s->min_uv = UINT32_MAX;
    s->max_uv = 0;
}

void stats_add(sample_stats_t *s, const power_sample_t *smp)
{
    if (smp->i_valid) {
        s->n++;
        s->sum_ua    += smp->i_ua;
        s->sum_sq_ua += (int64_t)smp->i_ua * (int64_t)smp->i_ua;
        if (smp->i_ua < s->min_ua) s->min_ua = smp->i_ua;
        if (smp->i_ua > s->max_ua) s->max_ua = smp->i_ua;
    }
    /* Voltage is only sampled every Nth pass (DESIGN.md §4.5), so accumulate it only
     * on the passes where it is genuinely new. Otherwise the same reading would be
     * counted eight times and the min/max would look far more stable than it is. */
    if (smp->v_valid && smp->v_is_fresh) {
        s->n_v++;
        s->sum_uv += smp->v_pack_uv;
        if (smp->v_pack_uv < s->min_uv) s->min_uv = smp->v_pack_uv;
        if (smp->v_pack_uv > s->max_uv) s->max_uv = smp->v_pack_uv;
    }
}

int32_t stats_mean_ua(const sample_stats_t *s)
{
    return s->n ? (int32_t)(s->sum_ua / (int64_t)s->n) : 0;
}

int32_t stats_stddev_ua(const sample_stats_t *s)
{
    if (s->n < 2) {
        return 0;
    }
    const int64_t mean = s->sum_ua / (int64_t)s->n;
    /* var = E[x^2] - E[x]^2, population variance. Adequate here; the sample-vs-
     * population distinction is noise next to the measurement it describes. */
    int64_t var = (s->sum_sq_ua / (int64_t)s->n) - (mean * mean);
    if (var < 0) {
        var = 0; /* can only be rounding */
    }
    return (int32_t)isqrt64((uint64_t)var);
}

/* Divides by the voltage count, not the current count: voltage is sampled on its own
 * divisor (DESIGN.md 4.5), so the two differ by that factor. */
int32_t stats_mean_uv(const sample_stats_t *s)
{
    return s->n_v ? (int32_t)(s->sum_uv / (int64_t)s->n_v) : 0;
}
