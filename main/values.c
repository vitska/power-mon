/*
 * values.c — see values.h.
 */

#include "values.h"

/*
 * Zero-initialised, which is the correct starting state and not merely a convenient
 * one: last_valid is false, so no reader can mistake an unpopulated snapshot for a
 * measurement of zero volts at zero amps.
 */
static values_t s_values;

values_t *values(void)
{
    return &s_values;
}
