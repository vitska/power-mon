/*
 * display_debug.c — the debug screens of DESIGN.md §9.11 / Appendix D, on the
 * 128x32 OLED.
 *
 * This is debug mode as specified: the panel is held ON, the screens carry raw
 * pre-scaled values, and the renderer reads the same snapshot the console prints
 * from. That last point is the important one -- a debug view that recomputes its own
 * numbers is a debug view that lies about the bug being chased (§9.11).
 *
 * Appendix D's D1/D2/D3 are written for a finished device with a gauge, a tier
 * machine and an LP core. None of those exist at M1 and two of them never will
 * (§9.3), so the screens here show what M1 actually has: raw sensor state, role
 * resolution, error counters and window statistics. The set grows with the
 * milestones; the frame does not need rewriting for that.
 *
 * Power: this is unapologetically a bench mode. ~6 mA for the panel plus a core that
 * never light-sleeps, against a T2 floor of ~300 uA (§9.8). It is enabled by
 * Kconfig, defaults ON at M1 because M1 is a bench milestone, and DESIGN.md §9.11's
 * auto-expiry belongs with the tier machine in M6 -- there is nothing to expire back
 * into yet.
 */

#include "display_debug.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "fixed_fmt.h"
#include "fuelgauge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ssd1306.h"

static const char *TAG = "disp";

/* Burn-in mitigation (DESIGN.md §9.11): a 128x32 OLED holding static text for hours
 * retains it. Invert the whole panel briefly, periodically. Doubles as a visible
 * "the render loop is still running" indicator, which is why it is not subtle. */
#define BLINK_PERIOD_US ((int64_t)5 * 60 * 1000000)
#define BLINK_HOLD_MS   1000

struct {
    ssd1306_handle_t oled;
    app_ctx_t       *ctx;
    volatile bool    enabled;
    volatile int8_t  pinned_screen; /* -1 = auto-cycle */
    volatile uint8_t n_screens;
    volatile uint32_t passkey;      /* non-zero while pairing is in progress */
/*
 * Screen 0 is pinned by default rather than cycling. The panel's job at the bench is
 * to show volts and amps while a meter is being compared against it, and a readout
 * that rotates to error counters every five seconds is not that. 'disp screen auto'
 * turns cycling back on.
 */
} static s_disp = {.pinned_screen = 0, .n_screens = 3};

/* --- screens ------------------------------------------------------------------ */

/*
 * Screen 0 — volts and amps, double height. Two 16-px lines fill the panel exactly.
 *
 * Right-aligned so the decimal points and the unit letters stack, which is what makes
 * a changing value readable at a glance: the digits move, the frame does not. This is
 * Appendix D's "readable at arm's length" rule applied to the two numbers M1 actually
 * cares about -- there is no SoC to give the large digits to yet.
 *
 *   +---------------------+
 *   |    12.439V          |
 *   |     1.9588A         |
 *   +---------------------+
 */
static void draw_big(const power_sample_t *s, bool valid)
{
    char b[24];

    if (!valid) {
        ssd1306_printf_2x(s_disp.oled, 0, "  -- --");
        ssd1306_printf_2x(s_disp.oled, 1, " no data");
        return;
    }

    fg_status_t fg;
    fg_get(&fg);

    /*
     * SoC gets the large digits, which is Appendix D's rule -- it is the one number
     * someone should be able to read across a garage. Volts and amps sit beside it in
     * the small font, and the bottom two rows carry the amp-hours and the gauge state.
     *
     * A '?' after the percentage means the figure came from voltage alone, with no
     * trustworthy count behind it yet. Silently showing a guess as a measurement is
     * the one thing a gauge must not do.
     */
    /* 12 bytes, not 8: the compiler cannot see that soc_permille is bounded to 1000,
     * and a truncation warning here is worth a few bytes of stack rather than a
     * pragma. */
    char soc[12];
    snprintf(soc, sizeof(soc), "%3lu%%%s",
             (unsigned long)((fg.soc_permille + 5) / 10),
             fg.voltage_only ? "?" : "");
    ssd1306_text_2x(s_disp.oled, 0, 0, soc);

    ssd1306_text(s_disp.oled, 0, 11, FMT_V(b, s->v_pack_uv));
    ssd1306_text(s_disp.oled, 0, 18, "V");
    ssd1306_text(s_disp.oled, 1, 11, FMT_A(b, s->i_ua));
    ssd1306_text(s_disp.oled, 1, 19, "A");

    /* Amp-hours remaining out of the learned capacity: the number that answers "how
     * much is left" in the unit the battery is actually sold in. */
    const int64_t rem_uah = fg.charge_uas / 3600;
    char bcap[24];
    ssd1306_printf(s_disp.oled, 2, "%s/%sAh",
                   fixed_fmt(b, sizeof(b), rem_uah, 1000000, 1),
                   fixed_fmt(bcap, sizeof(bcap), fg.full_capacity_uah, 1000000, 0));
    ssd1306_printf(s_disp.oled, 3, "%-9s %sW", fg_state_str(fg.state),
                   fixed_fmt(b, sizeof(b), s->p_uw, 1000000, 1));
}

/*
 * Screen 1 — live electrical. The numbers the bench meter is being compared
 * against, at the precision the M1 exit criterion needs (DESIGN.md §11).
 *
 *   +---------------------+
 *   | 13.420V  -2.1500A  *|
 *   | P  -28.850W  pga/4  |
 *   | shunt   -21400uV SAT|
 *   | n12345   up 1:23:45 |
 *   +---------------------+
 */
static void draw_live(const power_sample_t *s, bool valid, char spin)
{
    char b1[24], b2[24];

    if (!valid) {
        ssd1306_printf(s_disp.oled, 0, "-- no sample --   %c", spin);
        ssd1306_printf(s_disp.oled, 1, "sensors not ready");
        ssd1306_printf(s_disp.oled, 2, "see 'scan' on the");
        ssd1306_printf(s_disp.oled, 3, "console");
        return;
    }

    ssd1306_printf(s_disp.oled, 0, "%sV %8sA %c",
                   FMT_V(b1, s->v_pack_uv), FMT_A(b2, s->i_ua), spin);
    ssd1306_printf(s_disp.oled, 1, "P %9sW pga%s",
                   FMT_W(b1, s->p_uw), ina219_pga_str(s->pga));
    ssd1306_printf(s_disp.oled, 2, "shunt %7suV%s",
                   FMT_MV(b1, s->v_shunt_uv), s->saturated ? " SAT" : "");

    const uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    ssd1306_printf(s_disp.oled, 3, "n%-7lu up%lu:%02lu:%02lu",
                   (unsigned long)s_disp.ctx->n_samples,
                   (unsigned long)(up_s / 3600), (unsigned long)((up_s / 60) % 60),
                   (unsigned long)(up_s % 60));
}

/*
 * Screen 2 — health. What is wrong, if anything: which sensors answered, whether
 * roles resolved, the error counters and the noise figure. This is the screen that
 * finds wiring faults (Appendix D, D1's job).
 *
 *   +---------------------+
 *   | A40 ok  B41 ok  N  *|
 *   | bus0 nf12 rng0 unr0 |
 *   | mn-0.0020 sd0.0001  |
 *   | off  -14uA  10.0mR  |
 *   +---------------------+
 */
static void draw_health(char spin)
{
    const app_ctx_t *ctx = s_disp.ctx;
    char             b1[24], b2[24];

    const bool have_pos = ctx->sensors && sensors_have_pos(ctx->sensors);
    const bool have_neg = ctx->sensors && sensors_have_neg(ctx->sensors);
    const bool resolved = ctx->sensors &&
                          sensors_get_role_state(ctx->sensors) == SENSORS_ROLE_RESOLVED;

    /* Mode letter, plus '?' when roles have not resolved -- the state in which the
     * gauge deliberately refuses to integrate (§2.10.6). */
    const char *mode = ctx->sensors ? sensors_mode_str(sensors_get_mode(ctx->sensors))
                                    : "none";

    ssd1306_printf(s_disp.oled, 0, "A%02X %s B%02X %s %c%c",
                   CONFIG_BATMON_ADDR_POS_POLE, have_pos ? "ok" : "--",
                   CONFIG_BATMON_ADDR_NEG_POLE, have_neg ? "ok" : "--",
                   resolved ? mode[0] : '?', spin);
    ssd1306_printf(s_disp.oled, 1, "bus%lu nf%lu rg%lu ur%lu",
                   (unsigned long)ctx->err_bus,
                   (unsigned long)ctx->err_not_finished,
                   (unsigned long)ctx->err_range_discard,
                   (unsigned long)ctx->err_unresolved);
    ssd1306_printf(s_disp.oled, 2, "mn%s sd%s",
                   FMT_A(b1, stats_mean_ua(&ctx->window)),
                   FMT_A(b2, stats_stddev_ua(&ctx->window)));
    ssd1306_printf(s_disp.oled, 3, "off%7suA %lumR",
                   FMT_MA(b1, ctx->last.idle_offset_ua),
                   (unsigned long)(CONFIG_BATMON_SHUNT_UOHM / 1000));
}

/*
 * Pairing screen. This is the reason the panel earns its place in the security
 * design (DESIGN.md §8.5): passkey-display pairing needs somewhere to display the
 * passkey, and a device with a screen can be authenticated rather than falling back
 * to Just Works. It takes over the display because a six-digit number the user is
 * waiting to type must not be rotated away after five seconds.
 */
static void draw_passkey(uint32_t pk)
{
    ssd1306_printf(s_disp.oled, 0, "   BLE PAIRING");
    ssd1306_printf(s_disp.oled, 1, "  passkey %06lu", (unsigned long)pk);
    ssd1306_printf(s_disp.oled, 2, " enter on the phone");
    ssd1306_printf(s_disp.oled, 3, " ");
}

/* --- task --------------------------------------------------------------------- */

static void display_task(void *arg)
{
    (void)arg;

    static const char spinner[] = {'|', '/', '-', '\\'};
    uint32_t          tick = 0;
    int64_t           next_blink_us = esp_timer_get_time() + BLINK_PERIOD_US;
    bool              was_enabled = true;
    int8_t            shown = -1;

    for (;;) {
        if (!s_disp.enabled) {
            if (was_enabled) {
                ssd1306_display_on(s_disp.oled, false);
                was_enabled = false;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!was_enabled) {
            ssd1306_display_on(s_disp.oled, true);
            was_enabled = true;
        }

        /* Pairing outranks everything, including a pinned screen. */
        const uint32_t pk = s_disp.passkey;
        if (pk != 0) {
            if (shown != 99) {
                ssd1306_clear(s_disp.oled);
                shown = 99;
            }
            draw_passkey(pk);
            if (ssd1306_flush(s_disp.oled) != ESP_OK) {
                ESP_LOGW(TAG, "passkey flush failed");
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BATMON_DISPLAY_REFRESH_MS));
            continue;
        }

        /* Which screen. Auto-cycle unless pinned from the console. */
        int8_t screen = s_disp.pinned_screen;
        if (screen < 0) {
            const uint32_t per =
                CONFIG_BATMON_DISPLAY_SCREEN_MS / CONFIG_BATMON_DISPLAY_REFRESH_MS;
            screen = (int8_t)((per ? (tick / per) : 0) % s_disp.n_screens);
        }
        /* A screen change must repaint every row, including rows whose text happens
         * to be identical -- otherwise the dirty tracking correctly decides there is
         * nothing to do and the old screen stays up. */
        if (screen != shown) {
            ssd1306_clear(s_disp.oled);
            shown = screen;
        }

        const char spin = spinner[tick & 3];
        switch (screen) {
        case 0:  draw_big(&s_disp.ctx->last, s_disp.ctx->last_valid);      break;
        case 1:  draw_live(&s_disp.ctx->last, s_disp.ctx->last_valid, spin); break;
        default: draw_health(spin);                                        break;
        }

        const esp_err_t err = ssd1306_flush(s_disp.oled);
        if (err != ESP_OK) {
            /* The panel shares the gauge's bus (§2.5). A display fault must never
             * escalate: log it, keep going, and let the sampler carry on. */
            ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
        }

        const int64_t now = esp_timer_get_time();
        if (now >= next_blink_us) {
            ssd1306_set_invert(s_disp.oled, true);
            vTaskDelay(pdMS_TO_TICKS(BLINK_HOLD_MS));
            ssd1306_set_invert(s_disp.oled, false);
            next_blink_us = now + BLINK_PERIOD_US;
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BATMON_DISPLAY_REFRESH_MS));
    }
}

/* --- public ------------------------------------------------------------------- */

esp_err_t display_debug_start(app_ctx_t *ctx)
{
    ssd1306_config_t cfg = SSD1306_CONFIG_DEFAULT();
    cfg.i2c_addr         = CONFIG_BATMON_DISPLAY_ADDR;
    cfg.scl_speed_hz     = CONFIG_BATMON_I2CA_FREQ_HZ;
    cfg.contrast         = CONFIG_BATMON_DISPLAY_CONTRAST;
#ifdef CONFIG_BATMON_DISPLAY_FLIP
    cfg.flip = true;
#endif

    const esp_err_t err = ssd1306_init(ctx->bus_a, &cfg, &s_disp.oled);
    if (err != ESP_OK) {
        /* DESIGN.md §10: "OLED absent or NACKs -> display_present = 0". Not fatal,
         * and specifically not worth a reboot loop on a bench board. */
        ESP_LOGW(TAG, "no display at 0x%02X (%s) -- continuing without it",
                 CONFIG_BATMON_DISPLAY_ADDR, esp_err_to_name(err));
        return err;
    }

    s_disp.ctx     = ctx;
    s_disp.enabled = true;

    /* Priority 3, below the sampler's 6: a redraw must never delay a sample
     * (DESIGN.md §2.5, §3.3). */
    if (xTaskCreate(display_task, "display", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "display on: screen 0 (volts/amps, 2x) pinned, %lu ms refresh. "
                  "'disp screen auto' to cycle all %d",
             (unsigned long)CONFIG_BATMON_DISPLAY_REFRESH_MS, s_disp.n_screens);
    return ESP_OK;
}

bool display_debug_present(void)
{
    return s_disp.oled != NULL;
}

void display_debug_enable(bool on)
{
    s_disp.enabled = on;
}

bool display_debug_enabled(void)
{
    return s_disp.enabled;
}

void display_debug_set_screen(int screen)
{
    s_disp.pinned_screen =
        (screen < 0 || screen >= s_disp.n_screens) ? -1 : (int8_t)screen;
}

int display_debug_get_screen(void)
{
    return s_disp.pinned_screen;
}

int display_debug_n_screens(void)
{
    return s_disp.n_screens;
}

void display_debug_show_passkey(uint32_t passkey)
{
    s_disp.passkey = passkey;
}

esp_err_t display_debug_set_contrast(uint8_t contrast)
{
    if (!s_disp.oled) {
        return ESP_ERR_INVALID_STATE;
    }
    return ssd1306_set_contrast(s_disp.oled, contrast);
}
