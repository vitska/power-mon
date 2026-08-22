/*
 * values.h — every live measured or calculated quantity, in one place.
 *
 * This is the firmware's answer to "what is true right now". The sampler task is the
 * only writer; the CLI, the LCD and the BLE telemetry emitters are readers. Keeping
 * them all reading ONE snapshot is what makes the console, the panel and a phone
 * agree with each other -- a reader that recomputes its own numbers from the sensors
 * is a reader that will eventually disagree with the others and be believed anyway
 * (DESIGN.md §9.11 makes the same argument about the debug screens).
 *
 * What belongs here: anything measured or derived that outlives the call that
 * produced it. What does not: settings (config.h), accumulated history and
 * statistics (history_values.h), and the state-of-charge model, which owns its own
 * integrator and is reached through fuelgauge.h.
 *
 * Concurrency: single-writer, and readers tolerate a torn read of an int64 in the
 * sense that they may print a stale value, never a corrupt structure. Promoted to
 * the seqlock of DESIGN.md §3.3 in M3, at which point these accessors are where the
 * sequence counter goes -- which is the other reason they are accessors and not a
 * bare extern.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bme280.h"
#include "sensors.h"

typedef struct {
    /* --- the measurement ------------------------------------------------------ */

    /** The most recent accepted sample. Rejected reads never land here: a discarded
     *  range or a not-ready poll leaves the previous value standing, because a gap in
     *  the stream is honest and a zero is not. */
    power_sample_t last;
    bool           last_valid;
    uint32_t       n_samples;

    /* --- the environment ------------------------------------------------------ */

    /*
     * Read on its own slow cadence (§4.4) and cached. Nothing needs it at the sample
     * rate -- thermal mass makes anything faster pointless -- and a blocking
     * forced-mode read on every pass would cost 12 ms in the sampler for a value that
     * moves in minutes.
     */
    bme280_sample_t env;
    bool            env_valid;
    int64_t         env_next_us;

    /* --- what went wrong ------------------------------------------------------ */

    /*
     * Counters, not flags: the useful question about a flaky bus is "how often",
     * which a flag cannot answer. `not_finished` is expected to be large -- the tick
     * polls faster than the ADC converts -- so it is a rate to compare against
     * n_samples, not an error count to alarm on.
     */
    uint32_t err_not_finished;
    uint32_t err_range_discard;
    uint32_t err_unresolved;
    uint32_t err_bus;
} values_t;

/** The singleton. An accessor rather than a bare extern so the seqlock of §3.3 has
 *  somewhere to live when it lands, without touching every caller again. */
values_t *values(void);
