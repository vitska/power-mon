/*
 * fixed_fmt.h — print scaled integers without floating point.
 *
 * DESIGN.md §4.3 keeps floats out of the firmware. That decision is about the
 * integration path, but it is easier to hold if nothing anywhere reaches for a
 * float, including the console. It also lets us keep CONFIG_NEWLIB_NANO_FORMAT=y.
 */

#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Format @p value, expressed in units of 1/@p divisor, with @p decimals places.
 *
 * fixed_fmt(buf, n, -2150000, 1000000, 3) -> "-2.150"
 *
 * Rounds toward zero, matching the truncation used elsewhere in the gauge, so a
 * displayed value never reads higher in magnitude than the measurement.
 *
 * The arithmetic is 64-bit but the two printed components are narrowed to 32 bits
 * on the way out, because CONFIG_NEWLIB_NANO_FORMAT drops %ll support entirely --
 * a "%llu" here would silently print garbage rather than fail to build. That is
 * safe for every call site in this firmware: the integer part of a microvolt,
 * microamp or microwatt reading cannot approach 2^32.
 */
static inline const char *fixed_fmt(char *buf, size_t n, int64_t value,
                                    int64_t divisor, int decimals)
{
    const bool neg = value < 0;
    const uint64_t v = neg ? (uint64_t)(-value) : (uint64_t)value;

    const uint64_t whole = v / (uint64_t)divisor;
    const uint64_t rem   = v % (uint64_t)divisor;

    uint64_t p = 1;
    for (int i = 0; i < decimals; i++) {
        p *= 10;
    }
    const uint64_t frac = (rem * p) / (uint64_t)divisor;

    snprintf(buf, n, "%s%" PRIu32 ".%0*" PRIu32, neg ? "-" : "",
             (uint32_t)whole, decimals, (uint32_t)frac);
    return buf;
}

/* Common cases, each with its own buffer so several can appear in one printf. */
#define FMT_V(buf, uv)  fixed_fmt((buf), sizeof(buf), (int64_t)(uv), 1000000, 3)
#define FMT_A(buf, ua)  fixed_fmt((buf), sizeof(buf), (int64_t)(ua), 1000000, 4)
#define FMT_W(buf, uw)  fixed_fmt((buf), sizeof(buf), (int64_t)(uw), 1000000, 3)
#define FMT_MV(buf, uv) fixed_fmt((buf), sizeof(buf), (int64_t)(uv), 1000, 3)
#define FMT_MA(buf, ua) fixed_fmt((buf), sizeof(buf), (int64_t)(ua), 1000, 3)
