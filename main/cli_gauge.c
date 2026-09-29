/*
 * cli_gauge.c -- the fuel gauge's own commands.
 *
 * `soc` and its dozen settings, the battery chemistry that supplies their defaults, and
 * the SoC history the clients graph. The gauge is one model (DESIGN.md 5, CAPACITY.md)
 * and these are the three faces of it: where it is now, what pack it thinks it has, and
 * where it has been.
 *
 * Split out of cli.c, which had grown to a quarter of the firmware in one file.
 * The framing, the transports and the command table stay there; see cli_internal.h
 * for what these files share.
 */

#include "cli.h"

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_ctx.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fixed_fmt.h"
#include "freertos/FreeRTOS.h"
#include "fuelgauge.h"
#include "history_values.h"
#include "sdkconfig.h"
#include "cli_internal.h"

/* --- state of charge (DESIGN.md 5) -------------------------------------------- */

static void soc_show(void)
{
    fg_status_t   st;
    fg_config_t   c = fg_get_config();
    char          b1[24], b2[24], b3[24];

    fg_get(&st);

    printf("SoC          %s %%%s\n",
           fixed_fmt(b1, sizeof(b1), st.soc_permille, 10, 1),
           st.voltage_only ? "  (from VOLTAGE only -- no count behind it yet)" : "");
    printf("state        %s", fg_state_str(st.state));
    if      (st.t_full_s  >= 0) printf("   full in %ld min", (long)(st.t_full_s / 60));
    else if (st.t_empty_s >= 0) printf("   empty in %ld min", (long)(st.t_empty_s / 60));
    else if (st.settle_s  >= 0) printf("   rested in %ld s", (long)st.settle_s);
    printf("\n");
    printf("charge       %s Ah of %s Ah\n",
           fixed_fmt(b1, sizeof(b1), st.charge_uas / 3600, 1000000, 3),
           fixed_fmt(b2, sizeof(b2), st.full_capacity_uah, 1000000, 1));
    printf("SoH          %lu %%   (%s Ah learned / %s Ah nameplate, %lu learn%s)\n",
           (unsigned long)((uint64_t)st.full_capacity_uah * 100 /
                           (st.design_capacity_uah ? st.design_capacity_uah : 1)),
           fixed_fmt(b1, sizeof(b1), st.full_capacity_uah, 1000000, 1),
           fixed_fmt(b2, sizeof(b2), st.design_capacity_uah, 1000000, 1),
           (unsigned long)st.learn_count, st.learn_count == 1 ? "" : "s");
    if (st.last_learn_uah) {
        printf("             last raw measurement %s Ah\n",
               fixed_fmt(b1, sizeof(b1), st.last_learn_uah, 1000000, 2));
    }
    if (st.have_ref) {
        printf("learn span   from %s %% reference, %s Ah since; learns at the next\n",
               fixed_fmt(b1, sizeof(b1), st.ref_soc_permille, 10, 1),
               fixed_fmt(b2, sizeof(b2), -st.q_since_ref_uas / 3600, 1000000, 3));
        printf("             FULL/EMPTY/rest at least %s %% lower\n",
               fixed_fmt(b3, sizeof(b3), c.learn_min_depth_permille, 10, 1));
    } else {
        printf("learn span   none -- opens at the next FULL, EMPTY or settled rest\n");
    }
    printf("Peukert      k %s, x%s at the present rate\n",
           fixed_fmt(b1, sizeof(b1), c.peukert_q8, 256, 3),
           fixed_fmt(b2, sizeof(b2), st.peukert_factor_q16, 65536, 4));
    printf("OCV estimate %s V   (terminal minus I*R, %s mV of compensation)\n",
           FMT_V(b1, st.ocv_uv), FMT_MV(b2, st.ir_drop_uv));
    printf("lifetime     in %s Ah, out %s Ah\n",
           fixed_fmt(b1, sizeof(b1), st.cum_in_uas / 3600, 1000000, 3),
           fixed_fmt(b2, sizeof(b2), st.cum_out_uas / 3600, 1000000, 3));
    if (st.s_since_anchor == UINT32_MAX) {
        printf("last anchor  never since boot -- the count is carried over and\n");
        printf("             unverified; rest the pack to re-sync it\n");
    } else {
        printf("last anchor  %lu s ago\n", (unsigned long)st.s_since_anchor);
    }
    printf("\n");
    {
        const fg_chem_profile_t *chem = fg_chem_profile((fg_chem_t)c.chemistry);
        printf("battery      %s, %u cells -- 'battery' to change\n",
               chem ? chem->name : "unknown", (unsigned)c.cells);
    }
    printf("0%% at        %s V (resting OCV)\n", FMT_V(b1, c.v_0pct_uv));
    printf("100%% at      %s V (resting OCV)\n", FMT_V(b2, c.v_100pct_uv));
    printf("full at      %s V with charge current below %s A\n",
           FMT_V(b3, c.v_full_uv), FMT_A(b1, st.taper_current_ua));
    printf("             (taper %s A set for the nameplate, scaled to learned capacity)\n",
           FMT_A(b2, c.i_taper_ua));
    printf("R internal   %lu uOhm   deadband %s A\n",
           (unsigned long)c.r_int_uohm, FMT_A(b2, c.i_deadband_ua));
    printf("rest         %lu s below %s A (%u permille of capacity), then SoC\n",
           (unsigned long)c.t_rest_s, FMT_A(b3, st.rest_current_ua),
           (unsigned)c.i_rest_permille);
    printf("             re-syncs to resting voltage\n");
    printf("rated rate   %s A -- the current the nameplate capacity assumes\n",
           FMT_A(b1, c.i_rated_ua));
    printf("learning     needs %lu.%lu %% SoC change between references, blend %lu %%\n",
           (unsigned long)(c.learn_min_depth_permille / 10),
           (unsigned long)(c.learn_min_depth_permille % 10),
           (unsigned long)((uint32_t)c.learn_blend_q8 * 100 / 256));
}

int cmd_soc(int argc, char **argv)
{
    if (argc < 2) {
        soc_show();
        printf("\n");
        printf("  soc set <permille>     force SoC, 0..1000 (500 = 50.0%%)\n");
        printf("  soc full               declare the pack full now\n");
        printf("  soc reset              forget the count; re-seed from voltage\n");
        printf("  soc cap <uAh>          capacity, e.g. 44000000 for 44 Ah\n");
        printf("  soc v0 <uV>            resting OCV at 0%%\n");
        printf("  soc v100 <uV>          resting OCV at 100%%\n");
        printf("  soc vfull <uV>         absorption voltage for full detection\n");
        printf("  soc reset [all]        forget the count; 'all' wipes the history too\n");
        printf("  soc rint <uOhm>        internal resistance for I*R compensation\n");
        printf("  soc taper <uA>         charge current below which full can latch\n");
        printf("  soc rest <s>           idle time before OCV is trusted\n");
        printf("  soc irest <permille>   rest current as per mille of capacity, 1..200\n");
        printf("  soc peukert <q8>       k in Q8: 256 = 1.00, 294 = 1.15 lead-acid\n");
        printf("  soc irated <uA>        rate the nameplate capacity assumes, C/20\n");
        printf("  soc depth <permille>   SoC change between references to learn capacity\n");
        printf("Everything here is stored in flash and survives a reboot.\n");
        return 0;
    }

    if (strcmp(argv[1], "full") == 0) {
        ESP_ERROR_CHECK(fg_set_full());
        printf("declared full.\n");
        soc_show();
        return 0;
    }

    if (strcmp(argv[1], "reset") == 0) {
        /* `all` wipes the graphed history too. They are separate stores -- the gauge's
         * accumulator and the 288-point ring -- but after a pack swap or a bench session
         * both describe a battery that is no longer there, and resetting one while the
         * graph still draws the other is how a stale curve gets believed. */
        const bool all = (argc >= 3 && strcmp(argv[2], "all") == 0);
        if (argc >= 3 && !all) {
            printf("usage: soc reset [all]   -- 'all' also clears the SoC history\n");
            return 1;
        }
        ESP_ERROR_CHECK(fg_reset());
        if (all) {
            soc_history_clear();
        }
        printf("count cleared%s. SoC will be re-seeded from the next voltage reading,\n",
               all ? " and the SoC history wiped" : "");
        printf("and shown with a '?' until a rest period or a full charge anchors it.\n");
        return 0;
    }

    if (argc < 3) {
        printf("usage: soc <set|cap|v0|v100|vfull|rint|taper|rest> <value>\n");
        return 1;
    }

    const long v = strtol(argv[2], NULL, 10);
    if (v < 0) {
        printf("value must not be negative\n");
        return 1;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (v > 1000) {
            printf("permille, so 0..1000. 500 is 50.0%%.\n");
            return 1;
        }
        ESP_ERROR_CHECK(fg_set_soc_permille((uint32_t)v));
        printf("SoC forced.\n");
        soc_show();
        return 0;
    }

    fg_config_t c = fg_get_config();
    if      (strcmp(argv[1], "cap")   == 0) c.design_capacity_uah = (uint32_t)v;
    else if (strcmp(argv[1], "v0")    == 0) c.v_0pct_uv           = (uint32_t)v;
    else if (strcmp(argv[1], "v100")  == 0) c.v_100pct_uv         = (uint32_t)v;
    else if (strcmp(argv[1], "vfull") == 0) c.v_full_uv           = (uint32_t)v;
    else if (strcmp(argv[1], "rint")  == 0) c.r_int_uohm          = (uint32_t)v;
    else if (strcmp(argv[1], "taper") == 0) c.i_taper_ua          = (uint32_t)v;
    else if (strcmp(argv[1], "rest")  == 0) c.t_rest_s            = (uint32_t)v;
    else if (strcmp(argv[1], "peukert") == 0) c.peukert_q8 = (uint16_t)v;
    else if (strcmp(argv[1], "irated")  == 0) c.i_rated_ua = (uint32_t)v;
    else if (strcmp(argv[1], "depth")   == 0)
        c.learn_min_depth_permille = (uint16_t)v;
    else if (strcmp(argv[1], "irest")   == 0) c.i_rest_permille = (uint16_t)v;
    else {
        printf("unknown: %s\n", argv[1]);
        return 1;
    }

    const esp_err_t err = fg_set_config(&c);
    if (err != ESP_OK) {
        printf("rejected: %s\n", esp_err_to_name(err));
        printf("The window must satisfy v0 < v100 <= vfull, and capacity must be\n");
        printf("non-zero -- otherwise the SoC scale would invert or divide by zero.\n");
        printf("Peukert k must be 256..512 (1.00..2.00): below 1.0 would mean a fast\n");
        printf("discharge yields MORE capacity. Learn depth is 100..1000 permille,\n");
        printf("and the rest current 1..200 permille of the design capacity.\n");
        return 1;
    }
    /* fg_set_config() only changes the running gauge: config.c owns persistence of
     * every setting, and without this the help text's promise ("stored in flash and
     * survives a reboot") was false -- a `soc v0` was quietly back to its old value
     * after the next reset. */
    cli_cal_autosave();
    soc_show();
    return 0;
}

/* --- SoC history ---------------------------------------------------------------- */

/*
 * Machine-readable, like `config`: the phone app and the remote display graph it.
 * Values are SoC in permille, oldest first, 24 to a line so no line outgrows a small
 * client's line buffer; a client concatenates every `soc=` line in order. `-` is a
 * gap -- a boot, or a moment the gauge had no SoC -- and must be drawn as a break.
 * The newest point was taken age_s seconds ago and each earlier one interval_s before
 * the next, which is all a client without a shared clock needs to place them.
 *
 * `state=` lines carry the fuel gauge's state at each point, one letter per point in
 * the same order, 24 to a line (fg_state_code(): U C A F D E S R; `-` for none). A
 * client concatenates them like the `soc=` lines; one that predates them skips the
 * unknown key.
 *
 * interval_s is a SETTING (`hist every <s>`) rather than the constant it once was, and
 * capacity x interval_s is the whole span the ring can hold. A client takes both from
 * here; one that hardcodes either draws the wrong hours against the wrong axis.
 */

/* The interval in the terms anyone actually asks it in: how far back the graph reaches. */
static void hist_span_line(void)
{
    const unsigned long p = (unsigned long)soc_history_period_s();
    printf("a point every %lu s: %d points span %lu h %lu min\n", p,
           SOC_HIST_POINTS, p * SOC_HIST_POINTS / 3600,
           (p * SOC_HIST_POINTS % 3600) / 60);
}

int cmd_hist(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        soc_history_clear();
        printf("SoC history cleared\n");
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "every") == 0) {
        if (argc < 3) {
            hist_span_line();
            printf("usage: hist every <%d..%d seconds>\n", SOC_HIST_PERIOD_MIN_S,
                   SOC_HIST_PERIOD_MAX_S);
            return 1;
        }
        const long v = strtol(argv[2], NULL, 10);
        if (v < SOC_HIST_PERIOD_MIN_S || v > SOC_HIST_PERIOD_MAX_S) {
            printf("interval must be %d..%d s. The ring is a fixed %d points, so this\n",
                   SOC_HIST_PERIOD_MIN_S, SOC_HIST_PERIOD_MAX_S, SOC_HIST_POINTS);
            printf("is the whole span/resolution trade: 300 covers 24 h, 600 covers 48 h.\n");
            return 1;
        }
        const uint32_t was = soc_history_period_s();
        soc_history_set_period_s((uint32_t)v);
        config()->hist_period_s = soc_history_period_s();
        if (soc_history_period_s() != was) {
            /* The stored points are spaced the old way, and a client places them by
             * counting intervals back from now: kept, they would be drawn at times
             * they were never taken. */
            soc_history_clear();
            printf("history emptied: anything it held was taken %lu s apart.\n",
                   (unsigned long)was);
        }
        hist_span_line();
        const esp_err_t err = config_commit();
        if (err != ESP_OK) {
            printf("WARNING: could not save (%s) -- RAM only, lost on reboot.\n",
                   esp_err_to_name(err));
        }
        return 0;
    }
    static uint16_t pts[SOC_HIST_POINTS];
    static char     st[SOC_HIST_POINTS];
    uint32_t        age = 0;
    const int       n   = soc_history_get(pts, st, &age);
    printf("interval_s=%lu\n", (unsigned long)soc_history_period_s());
    printf("capacity=%d\n", SOC_HIST_POINTS);
    printf("points=%d\n", n);
    printf("age_s=%lu\n", (unsigned long)age);
    for (int i = 0; i < n; i += 24) {
        printf("soc=");
        for (int j = i; j < n && j < i + 24; j++) {
            if (j > i) printf(",");
            if (pts[j] == SOC_HIST_NONE) printf("-");
            else                         printf("%u", (unsigned)pts[j]);
        }
        printf("\n");
    }
    for (int i = 0; i < n; i += 24) {
        const int k = (n - i < 24) ? n - i : 24;
        printf("state=%.*s\n", k, &st[i]);
    }
    return 0;
}

/* --- battery chemistry ------------------------------------------------------- */

static void battery_show(void)
{
    const fg_config_t        c = fg_get_config();
    const fg_chem_profile_t *p = fg_chem_profile((fg_chem_t)c.chemistry);
    char b1[24], b2[24], b3[24];
    if (!p) {
        printf("chemistry    unknown (%u) -- set one with 'battery <chemistry>'\n",
               (unsigned)c.chemistry);
        return;
    }
    printf("chemistry    %s, %u cells in series   (battery %s %u)\n", p->name,
           (unsigned)c.cells, p->key, (unsigned)c.cells);
    printf("resting      %s V = 0 %%  ..  %s V = 100 %%\n", FMT_V(b1, c.v_0pct_uv),
           FMT_V(b2, c.v_100pct_uv));
    printf("full at      %s V with charge current below %s A\n", FMT_V(b3, c.v_full_uv),
           FMT_A(b1, c.i_taper_ua));
    if (p->trust_lo_permille >= 1000) {
        printf("re-sync      from resting voltage anywhere on the curve\n");
    } else {
        printf("re-sync      from resting voltage only below %u %% and above %u %% --\n"
               "             the curve is too flat in between to correct a count with\n",
               (unsigned)(p->trust_lo_permille / 10), (unsigned)(p->trust_hi_permille / 10));
    }
}

int cmd_battery(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        char b1[24], b2[24];
        for (int i = 0; i < FG_CHEM_COUNT; i++) {
            const fg_chem_profile_t *p = fg_chem_profile((fg_chem_t)i);
            printf("  %-8s %-24s %s .. %s V/cell resting\n", p->key, p->name,
                   FMT_V(b1, p->ocv_cell_uv[0]), FMT_V(b2, p->ocv_cell_uv[FG_OCV_POINTS - 1]));
        }
        printf("usage: battery <chemistry> [cells]. Without cells, the count is guessed\n"
               "from the present resting voltage.\n");
        return 0;
    }

    if (argc >= 2) {
        fg_chem_t chem;
        if (!fg_chem_parse(argv[1], &chem)) {
            printf("unknown chemistry '%s' -- 'battery list' shows them\n", argv[1]);
            return 1;
        }
        const fg_chem_profile_t *p = fg_chem_profile(chem);

        long cells   = 0;
        bool guessed = false;
        if (argc >= 3) {
            char *end = NULL;
            cells = strtol(argv[2], &end, 10);
            if (!end || *end != '\0' || cells < 1 || cells > 32) {
                printf("cells is 1..32, the number in series\n");
                return 1;
            }
        } else {
            /* Nearest whole number of cells at mid charge. Right for a pack anywhere
             * near the middle of its range; a nearly flat or nearly full pack of a
             * wide-window chemistry can land one out, hence the note below. */
            fg_status_t st;
            fg_get(&st);
            const uint32_t mid = p->ocv_cell_uv[FG_OCV_POINTS / 2];
            cells   = (long)((st.ocv_uv + mid / 2) / mid);
            guessed = true;
            if (cells < 1 || cells > 32) {
                printf("cannot guess the cell count from %lu mV -- give it: battery %s <cells>\n",
                       (unsigned long)(st.ocv_uv / 1000), p->key);
                return 1;
            }
        }

        const esp_err_t err = fg_set_chemistry(chem, (uint8_t)cells);
        if (err != ESP_OK) {
            printf("rejected: %s\n", esp_err_to_name(err));
            return 1;
        }
        cli_cal_autosave();
        if (guessed) {
            char b1[24];
            fg_status_t st;
            fg_get(&st);
            printf("%ld cells, guessed from %s V. If that is wrong: battery %s <cells>\n",
                   cells, FMT_V(b1, st.ocv_uv), p->key);
        }
        printf("The charge count starts over from the resting voltage.\n\n");
    }

    battery_show();
    return 0;
}
