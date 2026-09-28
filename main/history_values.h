/*
 * history_values.h — accumulated and time-series quantities, as opposed to the
 * instantaneous ones in values.h.
 *
 * Two of them: the rolling sample window -- the running statistics the zero-current
 * calibration is built on (DESIGN.md §5.5) and the noise figure that says whether a
 * reading can be trusted at all -- and, at the end of this file, the 48-hour SoC
 * history the clients graph. Both answer questions about a SPAN
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

/* --- SoC history (288 points, a point every 5 minutes by default) ------------------ */

/*
 * The one genuine time series the firmware keeps: state of charge at a fixed interval,
 * for the phone app's and the remote display's graphs. Clients used to record their
 * own, which meant a graph that started empty every time a phone connected and was lost
 * whenever the remote rebooted; the monitor is the thing that is always on, so it is
 * the one that remembers.
 *
 * 288 points of 2 bytes, persisted to NVS at every point, so an OTA update or a power
 * cycle does not wipe it. The monitor has no clock and cannot know how long it was
 * off, so a restored history gets a gap mark (SOC_HIST_NONE) at boot: a graph shows a
 * break there rather than drawing a line across time that was never measured.
 *
 * THE INTERVAL IS A SETTING (`hist every <s>`, cfg_t.hist_period_s), because the ring
 * is a fixed 288 points and the interval is therefore the only thing that trades span
 * against resolution: 5 minutes covers 24 h, 10 minutes covers 48 h, and a pack being
 * watched through one charge cycle is better served by the first. The span is not a
 * constant any more, so a client MUST take it from the `interval_s` and `capacity`
 * the `hist` reply carries rather than assuming either.
 *
 * Every point costs one ~900-byte NVS write, so the floor is a minute: at 5 minutes
 * that is 288 writes a day and years of endurance (DESIGN.md 6.3), and the floor is
 * ten times that.
 */
#define SOC_HIST_POINTS           288
#define SOC_HIST_PERIOD_DEFAULT_S 300   /* 5 min -> 24 h across the ring */
#define SOC_HIST_PERIOD_MIN_S     60
#define SOC_HIST_PERIOD_MAX_S     3600
#define SOC_HIST_NONE             0xFFFF
/* State letter for a point with none recorded: a gap, or a point stored by firmware
 * before the state was kept alongside SoC. */
#define SOC_HIST_STATE_NONE '-'

/** Restores the stored history and marks the boot gap. Call once, after NVS is up. */
void soc_history_init(void);

/** Seconds between points, as it is actually running. */
uint32_t soc_history_period_s(void);

/**
 * Sets the interval between points, clamped to SOC_HIST_PERIOD_MIN_S..MAX_S, and
 * restarts the schedule from now so the change takes effect at once.
 *
 * Points already recorded at a DIFFERENT spacing are dropped, because a client places
 * a point by counting intervals back from now and nothing else: kept, they would be
 * drawn at times they were never taken. The spacing the ring was filled at is stored
 * with it, so this is exact rather than a guess -- a board whose stored interval is the
 * one being set (the usual case, config_apply() at boot) keeps everything, and a board
 * upgrading from the old fixed 600 s to the 300 s default drops a history it cannot
 * honestly draw.
 */
void soc_history_set_period_s(uint32_t period_s);

/** True when the next point is due. Cheap enough to call on every sample. */
bool soc_history_due(int64_t now_us);

/** Records one point -- SoC in permille or SOC_HIST_NONE, and the gauge's state letter
 *  (fg_state_code()) at that moment -- and persists the ring. */
void soc_history_push(int64_t now_us, uint16_t soc_permille, char state);

/** Copies the history, oldest first, into out[SOC_HIST_POINTS] and the state letters
 *  into st[SOC_HIST_POINTS]. Returns how many points there are; *age_s is how long
 *  ago the newest was taken. */
int soc_history_get(uint16_t *out, char *st, uint32_t *age_s);

/** Forgets the history, in RAM and in flash. */
void soc_history_clear(void);
