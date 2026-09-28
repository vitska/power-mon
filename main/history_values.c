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

/* --- SoC history ------------------------------------------------------------------- */

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#define HIST_NS "hist"

static struct {
    uint16_t ring[SOC_HIST_POINTS];
    char     st[SOC_HIST_POINTS]; /* the gauge's state letter at each point */
    uint16_t head;          /* next slot to write */
    uint16_t count;
    uint32_t period_s;      /* seconds between points; a setting, not a constant */
    uint32_t ring_period_s; /* what the points in ring[] were actually taken at */
    int64_t  next_due_us;   /* 0 until the first sample sets the schedule */
    int64_t  last_point_us;
} s_hist = {.period_s      = SOC_HIST_PERIOD_DEFAULT_S,
            .ring_period_s = SOC_HIST_PERIOD_DEFAULT_S};

/* The sampler writes, the console reads: a spinlock around the copy, never around
 * the flash write. */
static portMUX_TYPE s_hist_mux = portMUX_INITIALIZER_UNLOCKED;

static void hist_save(void)
{
    static uint16_t copy[SOC_HIST_POINTS];
    static char     st[SOC_HIST_POINTS];
    uint16_t head, count;
    taskENTER_CRITICAL(&s_hist_mux);
    memcpy(copy, s_hist.ring, sizeof(copy));
    memcpy(st, s_hist.st, sizeof(st));
    head  = s_hist.head;
    count = s_hist.count;
    taskEXIT_CRITICAL(&s_hist_mux);

    nvs_handle_t h;
    if (nvs_open(HIST_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "ring", copy, sizeof(copy));
    /* Its own key, not a wider "ring": history stored before states were kept still
     * loads, just with no state against its points. */
    nvs_set_blob(h, "st", st, sizeof(st));
    nvs_set_u16(h, "head", head);
    nvs_set_u16(h, "count", count);
    /* The spacing these points were taken at, so a restore can tell whether the
     * interval in force still describes them. Absent = the old fixed 600 s. */
    nvs_set_u32(h, "per", s_hist.period_s);
    nvs_commit(h);
    nvs_close(h);
}

static void hist_append(uint16_t v, char state)
{
    taskENTER_CRITICAL(&s_hist_mux);
    s_hist.ring[s_hist.head] = v;
    s_hist.st[s_hist.head]   = state;
    s_hist.head              = (s_hist.head + 1) % SOC_HIST_POINTS;
    if (s_hist.count < SOC_HIST_POINTS) {
        s_hist.count++;
    }
    taskEXIT_CRITICAL(&s_hist_mux);
}

void soc_history_init(void)
{
    nvs_handle_t h;
    size_t       len = sizeof(s_hist.ring);
    uint16_t     head = 0, count = 0;
    memset(s_hist.st, SOC_HIST_STATE_NONE, sizeof(s_hist.st));
    if (nvs_open(HIST_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "ring", s_hist.ring, &len) == ESP_OK && len == sizeof(s_hist.ring) &&
            nvs_get_u16(h, "head", &head) == ESP_OK && nvs_get_u16(h, "count", &count) == ESP_OK &&
            head < SOC_HIST_POINTS && count <= SOC_HIST_POINTS) {
            s_hist.head  = head;
            s_hist.count = count;
            size_t slen = sizeof(s_hist.st);
            if (nvs_get_blob(h, "st", s_hist.st, &slen) != ESP_OK || slen != sizeof(s_hist.st)) {
                memset(s_hist.st, SOC_HIST_STATE_NONE, sizeof(s_hist.st));
            }
            uint32_t per = 600; /* what firmware without this key always used */
            nvs_get_u32(h, "per", &per);
            s_hist.ring_period_s = per;
        }
        nvs_close(h);
    }
    if (s_hist.count > 0) {
        /* However long the power was off, it was not measured. */
        hist_append(SOC_HIST_NONE, SOC_HIST_STATE_NONE);
        s_hist.last_point_us = esp_timer_get_time();
        ESP_LOGI("hist", "restored %u SoC points taken %lu s apart, gap marked",
                 s_hist.count, (unsigned long)s_hist.ring_period_s);
    }
}

uint32_t soc_history_period_s(void)
{
    return s_hist.period_s;
}

void soc_history_set_period_s(uint32_t period_s)
{
    if (period_s < SOC_HIST_PERIOD_MIN_S) {
        period_s = SOC_HIST_PERIOD_MIN_S;
    }
    if (period_s > SOC_HIST_PERIOD_MAX_S) {
        period_s = SOC_HIST_PERIOD_MAX_S;
    }
    if (period_s == s_hist.period_s) {
        return;
    }
    const bool stale = (s_hist.count > 0 && s_hist.ring_period_s != period_s);
    if (stale) {
        ESP_LOGW("hist", "interval %lu -> %lu s: dropping %u points taken at the old one",
                 (unsigned long)s_hist.ring_period_s, (unsigned long)period_s,
                 (unsigned)s_hist.count);
    }
    s_hist.period_s = period_s;
    /* Points taken at another spacing cannot be placed on a graph drawn at this one.
     * clear() resets ring_period_s to the new value along with the ring. */
    if (stale) {
        soc_history_clear();
    } else {
        s_hist.ring_period_s = period_s;
    }
    /* The pending due time was computed from the old spacing. Rebuild it from now, so
     * shortening the interval does not leave the next point an old interval away --
     * the first thing anyone does after asking for points more often is look for one. */
    if (s_hist.next_due_us != 0) {
        s_hist.next_due_us = esp_timer_get_time() + (int64_t)period_s * 1000000;
    }
    ESP_LOGI("hist", "a point every %lu s (%lu h across %d points)",
             (unsigned long)period_s,
             (unsigned long)((uint32_t)period_s * SOC_HIST_POINTS / 3600), SOC_HIST_POINTS);
}

bool soc_history_due(int64_t now_us)
{
    if (s_hist.next_due_us == 0) {
        /* The first point a minute after the first sample: long enough for the gauge
         * to have seeded from voltage or restored its count, short enough that a
         * fresh board shows something soon. */
        s_hist.next_due_us = now_us + 60LL * 1000000;
        return false;
    }
    return now_us >= s_hist.next_due_us;
}

void soc_history_push(int64_t now_us, uint16_t soc_permille, char state)
{
    hist_append(soc_permille, state);
    s_hist.last_point_us = now_us;
    /* On schedule from the previous due time, not from now: a late sample must not
     * make every later point late too. */
    do {
        s_hist.next_due_us += (int64_t)s_hist.period_s * 1000000;
    } while (s_hist.next_due_us <= now_us);
    hist_save();
}

int soc_history_get(uint16_t *out, char *st, uint32_t *age_s)
{
    taskENTER_CRITICAL(&s_hist_mux);
    const int n = s_hist.count;
    for (int i = 0; i < n; i++) {
        const int k = (s_hist.head - n + i + SOC_HIST_POINTS) % SOC_HIST_POINTS;
        out[i] = s_hist.ring[k];
        st[i]  = s_hist.st[k];
    }
    const int64_t last = s_hist.last_point_us;
    taskEXIT_CRITICAL(&s_hist_mux);
    *age_s = n ? (uint32_t)((esp_timer_get_time() - last) / 1000000) : 0;
    return n;
}

void soc_history_clear(void)
{
    taskENTER_CRITICAL(&s_hist_mux);
    s_hist.head  = 0;
    s_hist.count = 0;
    taskEXIT_CRITICAL(&s_hist_mux);
    s_hist.ring_period_s = s_hist.period_s; /* nothing left that was taken at another */
    nvs_handle_t h;
    if (nvs_open(HIST_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}
