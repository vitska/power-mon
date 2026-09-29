/*
 * ui.c — see ui.h.
 *
 * Layout, landscape 320 x 240:
 *
 *   +--------------------------------------------------------+  0
 *   | batmon-DCFA                      9/s 0.11.6  ...ll (o) |  header: tap -> Settings
 *   +----------------------------+---------------------------+ 22
 *   | STATE OF CHARGE            |  12.432 V                 |
 *   |  72.4 %                    |  -0.0089 A                |
 *   |  [##.##.##.##.##|##.##.--.--.--] |  -0.110 W           |
 *   | TIME TO EMPTY              |  DISCHARGE   (device state)|
 *   |  3d 04h                    |  FLOODED 6S               |
 *   |                            |  CAP 44.0AH (43.97)       |
 *   +----------------------------+---------------------------+ 148
 *   | SOC 24 h              24.1C 46.2% 1003.5hPa      100      |
 *   |  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~    50      |  tap: 1 h / 6 h / 24 h
 *   |                                                  0      |  graph: the monitor's own
 *                                                                history (`hist`)
 *   +--------------------------------------------------------+ 240
 *
 * Every field is redrawn only when the text it shows changes, which is what lets a
 * framebuffer-less display keep up: most refreshes send nothing at all.
 */

#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd.h"
#include "link.h"
#include "ota.h"
#include "rcon.h"
#include "touch.h"

typedef enum { SCR_MAIN, SCR_SETTINGS, SCR_DEVICES, SCR_NUMPAD, SCR_CONFIRM, SCR_UPDATE,
               SCR_PASSKEY } screen_t;

#define STALE_US (5LL * 1000000) /* no record for this long: the numbers are old */

static screen_t s_screen;
static bool     s_full;              /* redraw everything on the next pass */
/* Graph span in hours. Not a fixed menu any more: the monitor's interval is settable,
 * so the widest useful span is whatever its ring covers, and the tap cycles that, a
 * half of it and a quarter. 0 until a fetch says what the ring holds. */
static int      s_span_h;
static int      s_span_full_h;       /* what the monitor's ring covers, from the last fetch */

/* --- small widgets --------------------------------------------------------------- */

static bool hit(int tx, int ty, int x, int y, int w, int h)
{
    return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

static void button(int x, int y, int w, int h, const char *label, int scale, uint16_t bg,
                   uint16_t fg)
{
    lcd_fill(x, y, w, h, C_GREY);
    lcd_fill(x + 1, y + 1, w - 2, h - 2, bg);
    const int tw = lcd_text_w(label, scale), th = 8 * scale;
    lcd_text(x + (w - tw) / 2, y + (h - th) / 2, label, scale, fg, bg);
}

/* A text field that redraws only when its content or colour changes. */
typedef struct {
    char     text[32];
    uint16_t color;
} field_t;

static void field(field_t *f, int x, int y, int w, int scale, uint16_t fg, uint16_t bg,
                  const char *text)
{
    if (!s_full && f->color == fg && strcmp(f->text, text) == 0) {
        return;
    }
    snprintf(f->text, sizeof(f->text), "%s", text);
    f->color = fg;
    lcd_text_field(x, y, w, text, scale, fg, bg);
}

/* "0d 00:00:00", always -- days is a plain integer with no cap, so an implausible
 * estimate from a near-zero current is visibly implausible ("41d 03:00:00") rather
 * than hidden behind a rounded-off ">99d". */
static void fmt_duration(char *out, size_t n, float hours)
{
    if (!(hours >= 0)) {
        snprintf(out, n, "--");
        return;
    }
    const long total_s = (long)(hours * 3600.0f + 0.5f);
    const long d        = total_s / 86400;
    const long rem      = total_s % 86400;
    snprintf(out, n, "%ldd %02ld:%02ld:%02ld", d, rem / 3600, (rem % 3600) / 60, rem % 60);
}

/* --- the Bluetooth activity icon ---------------------------------------------------- */

/* The Bluetooth rune, 8 x 13. Drawn one pixel wider to the right than the pattern, so
 * the strokes are two pixels thick at the header's small size. */
static const char *const BT_RUNE[13] = {
    "...#....",
    "...##...",
    "...#.#..",
    "#..#..#.",
    ".#.#.#..",
    "..###...",
    "...#....",
    "..###...",
    ".#.#.#..",
    "#..#..#.",
    "...#.#..",
    "...##...",
    "...#....",
};
#define BT_X 299
#define BT_Y 4
#define BT_W 10
#define BT_H 13

static void bt_icon(uint16_t color)
{
    static uint16_t px[BT_W * BT_H];
    for (int y = 0; y < BT_H; y++) {
        for (int x = 0; x < BT_W; x++) {
            const bool on = (x < 8 && BT_RUNE[y][x] == '#') ||
                            (x > 0 && x - 1 < 8 && BT_RUNE[y][x - 1] == '#');
            px[y * BT_W + x] = on ? color : C_HEADER;
        }
    }
    lcd_blit(BT_X, BT_Y, BT_W, BT_H, px);
}

/* --- the signal-strength antenna ------------------------------------------------------ */

/* Five ascending bars, classic phone-status-bar shape, immediately left of the
 * Bluetooth icon. `bars` (0..5) are filled blue; the rest stay dim outlines, so the
 * five-bar frame is always visible and only the fill changes. */
#define ANT_X    283
#define ANT_Y    4
#define ANT_MAXH 13
#define ANT_BAR_W 2
#define ANT_GAP   1

static void ant_icon(int bars)
{
    if (bars < 0) bars = 0;
    if (bars > 5) bars = 5;
    for (int i = 0; i < 5; i++) {
        const int h = 3 + i * ((ANT_MAXH - 3) * 2 / 8); /* 3,5,8,10,13 */
        const int x = ANT_X + i * (ANT_BAR_W + ANT_GAP);
        lcd_fill(x, ANT_Y, ANT_BAR_W, ANT_MAXH - h, C_HEADER);
        lcd_fill(x, ANT_Y + ANT_MAXH - h, ANT_BAR_W, h, i < bars ? C_BLUE : C_DIM);
    }
}

/* RSSI to a bar count. These are BLE link thresholds, not Wi-Fi's -- a subscribed,
 * working link is routinely -70 to -85 dBm, so the scale is shifted down accordingly. */
static int rssi_bars(int8_t dbm)
{
    if (dbm >= -60) return 5;
    if (dbm >= -70) return 4;
    if (dbm >= -80) return 3;
    if (dbm >= -90) return 2;
    if (dbm >= -100) return 1;
    return 0;
}

/* --- the dashboard ----------------------------------------------------------------- */

/* The SoC gauge: ten bricks, one per 10 %. */
#define SOC_BRICKS 10

#define GX 4
#define GY 162
#define GW 284
#define GH 74
/* The gauge-state colour, the same one the mode label uses, from the monitor's
 * one-letter code (fg_state_code()). A point with no state draws plain green. */
static uint16_t state_colour(char c)
{
    switch (c) {
    case 'F':           return C_CYAN;
    case 'D':           return C_ORANGE;
    case 'E':           return C_RED;
    case 'S': case 'R': return C_GREY;
    case 'C': case 'A':
    default:            return C_GREEN;
    }
}

/* The fill under the line: the line colour at about a third of its brightness. */
static uint16_t dim565(uint16_t c)
{
    return (uint16_t)(((((c >> 11) & 31) / 3) << 11) | ((((c >> 5) & 63) / 3) << 5) |
                      ((c & 31) / 3));
}

/* What the monitor's ring covers, in hours, rounded down and at least one: the widest
 * span worth offering. 0 until a fetch has said. */
static int hist_cover_h(const link_hist_t *h)
{
    const uint32_t s = h->interval_s * h->capacity;
    return s < 3600 ? (s ? 1 : 0) : (int)(s / 3600);
}

/*
 * The monitor's own history (`hist`): a point every interval_s, fetched on connect and
 * every two minutes. The graph's right edge is "now"; each point sits where its age
 * puts it, so a span shows exactly that many hours whatever the ring holds, and minutes
 * the monitor did not record stay empty.
 */
static void draw_graph(void)
{
    static link_hist_t h;
    static int16_t     ycol[GW];
    static char        scol[GW];
    link_history(&h);

    /* First sight of a history, or one whose interval changed under us: show all of it. */
    const int cover = hist_cover_h(&h);
    s_span_full_h   = cover;
    if (cover > 0 && (s_span_h <= 0 || s_span_h > cover)) {
        s_span_h = cover;
    }
    const int64_t span_s = (int64_t)(s_span_h > 0 ? s_span_h : 48) * 3600;
    for (int c = 0; c < GW; c++) { ycol[c] = -1; scol[c] = 0; }
    for (int i = 0; i < h.count; i++) {
        if (h.pts[i] == LINK_HIST_NONE) continue;
        /* Age of point i: the newest is age_s old, each earlier one interval older. */
        const int64_t age = (int64_t)h.age_s + (int64_t)(h.count - 1 - i) * h.interval_s;
        if (age > span_s) continue;
        const int c = GW - 1 - (int)(age * (GW - 1) / span_s);
        ycol[c] = (int16_t)(GH - 1 - (int)h.pts[i] * (GH - 1) / 1000);
        scol[c] = h.st[i];
    }
    /* Neighbouring points are one interval apart, which on the 12 h span is ~4 px:
     * interpolate between them so it draws a line, not a row of dots. Anything
     * further apart is a real gap in the data and stays one. */
    const int join = (int)((int64_t)h.interval_s * (GW - 1) / span_s) + 1;
    for (int c = 0, last = -1; c < GW; c++) {
        if (ycol[c] < 0) continue;
        if (last >= 0 && c - last > 1 && c - last <= join) {
            for (int k = last + 1; k < c; k++) {
                ycol[k] = (int16_t)(ycol[last] + (ycol[c] - ycol[last]) * (k - last) / (c - last));
                scol[k] = scol[last]; /* a state holds until the next point */
            }
        }
        last = c;
    }

    /*
     * Each column's line and fill take the colour of the gauge's state there, so the
     * curve itself says when the pack was charging, full, discharging or resting.
     *
     * Swapped to the panel's byte order HERE, once per column, because the band below
     * is written straight into the DMA buffer: 284 swaps instead of 21 016, and the
     * per-pixel copy that used to sit between this and the wire is gone entirely.
     */
    static uint16_t lcol[GW], fcol[GW];
    for (int c = 0; c < GW; c++) {
        const uint16_t line = state_colour(scol[c]);
        lcol[c] = lcd_px(line);
        fcol[c] = lcd_px(dim565(line));
    }
    const uint16_t bg = lcd_px(C_PANEL), grid = lcd_px(C_DIM);
    const int rows = LCD_STRIP_PX / GW;
    for (int y0 = 0; y0 < GH; y0 += rows) {
        const int h = (y0 + rows > GH) ? GH - y0 : rows;
        /* Re-fetched every band: the buffers alternate, so the previous pointer now
         * belongs to the transfer still on the wire. */
        uint16_t *buf = lcd_strip();
        for (int r = 0; r < h; r++) {
            const int  y      = y0 + r;
            const bool gridln = (y == GH / 4 || y == GH / 2 || y == 3 * GH / 4);
            for (int c = 0; c < GW; c++) {
                uint16_t px = (gridln && (c & 3) == 0) ? grid : bg;
                const int yc = ycol[c];
                if (yc >= 0) {
                    if (y == yc || y == yc + 1) px = lcol[c];
                    else if (y > yc)            px = fcol[c];
                }
                buf[r * GW + c] = px;
            }
        }
        lcd_blit_strip(GX, GY + y0, GW, h);
    }
    const char *msg = !h.supported ? "monitor firmware too old for history (< 0.9.0)"
                    : h.count == 0 ? "no history yet -- the monitor records one now and then"
                                   : NULL;
    if (msg) {
        lcd_text(GX + (GW - lcd_text_w(msg, 1)) / 2, GY + GH / 2 - 4, msg, 1, C_GREY, bg);
    }
}

static void main_enter(void)
{
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    lcd_fill(0, 0, LCD_W, 22, C_HEADER);
    lcd_fill(0, 148, LCD_W, 1, C_DIM);
    lcd_fill(GX, GY, GW, GH, C_PANEL);
    lcd_text(6, 28, "STATE OF CHARGE", 1, C_GREY, C_BLACK);
    lcd_text(156, 64, "%", 3, C_WHITE, C_BLACK);
    lcd_text(GX + GW + 4, GY, "100", 1, C_GREY, C_BLACK);
    lcd_text(GX + GW + 4, GY + GH / 2 - 4, "50", 1, C_GREY, C_BLACK);
    lcd_text(GX + GW + 4, GY + GH - 8, "0", 1, C_GREY, C_BLACK);
    draw_graph();
}

static void main_draw(const link_model_t *m)
{
    static field_t f_name, f_status, f_soc, f_tlabel, f_time, f_v, f_a, f_w, f_mode, f_sub,
        f_glabel, f_cap;
    static int     bar_last = -2;   /* bricks lit at the last draw */
    static uint16_t bar_col;
    static uint16_t icon_last = 1;
    static int      bars_last = -1;
    static uint32_t seen_rx, seen_tx, rate_base;
    static int64_t  flash_until, rate_t;
    static uint16_t flash_col;
    static int      rate;

    const int64_t now  = esp_timer_get_time();
    const bool    live = m->state == LINK_READY && m->have_fast &&
                      now - m->last_data_us < STALE_US;
    char b[32];

    /* Header. */
    field(&f_name, 6, 3, 186, 2, C_WHITE, C_HEADER, m->name[0] ? m->name : "tap: pick a board");
    const char *st;
    uint16_t    sc;
    switch (m->state) {
    case LINK_SCANNING:   st = "SEARCHING";  sc = C_BLUE;   break;
    case LINK_CONNECTING: st = "CONNECTING"; sc = C_YELLOW; break;
    case LINK_SETUP:      st = "SETUP";      sc = C_YELLOW; break;
    case LINK_PAIRING:    st = "PAIRING";    sc = C_ORANGE; break;
    default:              st = "NO DATA";    sc = C_ORANGE; break;
    }
    /*
     * A working link says so by working: the packets tick up and the numbers move, so
     * the word that used to sit here ("LIVE", "PAIRED") spent the header's only spare
     * room restating that. What cannot be deduced by watching goes there instead -- the
     * MONITOR's firmware version, which decides what half this screen can even show
     * (time estimates need 0.11.3, the history 0.9.0) and is otherwise only findable
     * from the phone app. Encrypted versus merely connected, the other thing "PAIRED"
     * carried, is now the colour: green when the link is authenticated, cyan when it is
     * not. Every state that is NOT a working link still says so in words, because there
     * a word is the whole message.
     */
    if (live) {
        sc = m->secure ? C_GREEN : C_CYAN;
    }

    /* Packets per second, so a stalled stream shows as a number, not just as values
     * that stop changing -- a quiet battery looks exactly like a dead link otherwise. */
    if (now - rate_t >= 1000000) {
        rate      = (int)(m->rx_packets - rate_base);
        rate_base = m->rx_packets;
        rate_t    = now;
    }
    char status[24];
    if (live && m->firmware[0]) snprintf(status, sizeof(status), "%d/s %.8s", rate,
                                         m->firmware);
    else if (live)              snprintf(status, sizeof(status), "%d/s", rate);
    else if (m->state == LINK_READY) snprintf(status, sizeof(status), "%s %d/s", st, rate);
    else                        snprintf(status, sizeof(status), "%s", st);
    field(&f_status, 196, 8, 84, 1, sc, C_HEADER, status);

    /* The icon is always Bluetooth-blue -- state lives in the status text's colour
     * above, not in the icon -- and flashes white for every notification that
     * arrives, yellow for every command sent. At 10 Hz telemetry it flickers
     * steadily; when it stops, so has the data. */
    if (m->tx_packets != seen_tx) {
        seen_tx     = m->tx_packets;
        flash_col   = C_YELLOW;
        flash_until = now + 150 * 1000;
    } else if (m->rx_packets != seen_rx && now >= flash_until) {
        flash_col   = C_WHITE;
        flash_until = now + 60 * 1000;
    }
    seen_rx = m->rx_packets;
    const uint16_t ic = now < flash_until ? flash_col : C_BLUE;
    if (s_full || icon_last != ic) {
        bt_icon(ic);
        icon_last = ic;
    }

    const int bars = m->have_rssi ? rssi_bars(m->rssi_dbm) : 0;
    if (s_full || bars_last != bars) {
        ant_icon(bars);
        bars_last = bars;
    }

    /* State of charge. */
    const bool  have_soc = live && m->have_calc;
    const float soc      = m->soc_pct;
    /*
     * Colour is about the pack, not the link: green from 50 %, yellow from 30, red
     * below. The number and the bar take the same colour, so the two can never
     * disagree about how worried to look.
     *
     * Below 10 % it blinks, half a second each way. By then the reading is not a
     * status any more, it is a thing to act on, and a static red carries no more
     * urgency across a room than a static green -- motion is the only channel left
     * that a glance picks up without reading the digits.
     */
    uint16_t soc_c = !have_soc  ? C_GREY
                     : soc >= 50.0f ? C_GREEN
                     : soc >= 30.0f ? C_YELLOW
                                    : C_RED;
    if (have_soc && soc < 10.0f && ((now / 500000) & 1)) {
        soc_c = C_RED_DIM;
    }
    if (!have_soc)          snprintf(b, sizeof(b), "--");
    else if (soc >= 99.95f) snprintf(b, sizeof(b), "100");
    else                    snprintf(b, sizeof(b), "%.1f", soc);
    field(&f_soc, 6, 40, 150, 6, soc_c, C_BLACK, b);

    /*
     * Ten bricks, one per 10 %, lit to the NEAREST tenth rather than the one below.
     * Truncating cost the tenth brick at 99 %, and a pack one percent off full that
     * displays as nine tenths reads as a fault in the gauge -- the eye checks a full
     * pack against "all ten lit", not against the digits. Rounding puts each brick's
     * boundary in the middle of its band instead: the top one goes out below 95 %, the
     * next below 85, and so on down to the first below 5.
     *
     * The floor of one brick survives that: a pack at 2 % would round to none and look
     * identical to a flat one, and those are not the same thing. It is also what the
     * sub-10 % blink needs in order to blink.
     *
     * Unlit bricks are drawn dim rather than left black: the ten slots stay visible,
     * so the lit ones read as a proportion at a glance instead of as a bar of unknown
     * length.
     */
    int lit = !have_soc ? -1 : (int)((soc + 5.0f) / 10.0f);
    if (lit > SOC_BRICKS) lit = SOC_BRICKS;
    if (have_soc && lit == 0 && soc > 0.0f) lit = 1;
    if (s_full || lit != bar_last || bar_col != soc_c) {
        lcd_fill(6, 96, 164, 12, C_GREY);   /* a one-pixel frame around the slots */
        lcd_fill(7, 97, 162, 10, C_BLACK);
        for (int i = 0; i < SOC_BRICKS; i++) {
            /* Pitch from the box width, not a constant: 162 does not divide by ten,
             * and rounding each edge separately spreads the odd pixels evenly rather
             * than piling them all into the last brick. */
            const int x0 = 7 + (i * 162) / SOC_BRICKS;
            const int x1 = 7 + ((i + 1) * 162) / SOC_BRICKS - (i + 1 < SOC_BRICKS ? 2 : 0);
            lcd_fill(x0, 97, x1 - x0, 10, i < lit ? soc_c : C_DIM);
        }
        /*
         * Half-scale mark, through the gap between the fifth and sixth bricks and the
         * full height of the frame. Counting five bricks is slower than seeing which
         * side of the middle the lit ones end on, and half is the threshold most of
         * the decisions about a lead-acid pack are made against.
         */
        lcd_fill(7 + (162 * (SOC_BRICKS / 2)) / SOC_BRICKS - 2, 96, 2, 12, C_GREY);
        bar_last = lit;
        bar_col  = soc_c;
    }

    /*
     * The estimate is the MONITOR's, computed in its firmware from its own state and
     * averaged current, and shown here as sent -- so it always agrees with the mode
     * label and with the phone app. Monitor firmware before 0.11.3 sends none.
     */
    const char *label = "TIME ESTIMATE";
    if (!live || !m->have_calc || !m->have_est) {
        snprintf(b, sizeof(b), "--");
    } else if (strcmp(m->mode, "FULL") == 0) {
        snprintf(b, sizeof(b), "full");
    } else if (strcmp(m->mode, "EMPTY") == 0) {
        snprintf(b, sizeof(b), "empty");
    } else if (m->t_full_s >= 0) {
        label = "TIME TO FULL";
        fmt_duration(b, sizeof(b), m->t_full_s / 3600.0f);
    } else if (m->t_empty_s >= 0) {
        label = "TIME TO EMPTY";
        fmt_duration(b, sizeof(b), m->t_empty_s / 3600.0f);
    } else if (m->settle_s >= 0) {
        label = "SETTLING, RESTED IN";
        snprintf(b, sizeof(b), "%ld:%02ld", (long)(m->settle_s / 60), (long)(m->settle_s % 60));
    } else if (strcmp(m->mode, "REST") == 0) {
        snprintf(b, sizeof(b), "rested");
    } else {
        snprintf(b, sizeof(b), "--");
    }
    field(&f_tlabel, 6, 116, 164, 1, C_GREY, C_BLACK, label);
    field(&f_time, 6, 128, 164, 2, live ? C_WHITE : C_GREY, C_BLACK, b);

    /* Measurements and mode. */
    const uint16_t vc = live ? C_WHITE : C_GREY;
    if (live) snprintf(b, sizeof(b), "%.3f V", m->volts); else snprintf(b, sizeof(b), "-- V");
    field(&f_v, 178, 30, 140, 2, vc, C_BLACK, b);
    if (live) snprintf(b, sizeof(b), "%.4f A", m->amps); else snprintf(b, sizeof(b), "-- A");
    field(&f_a, 178, 54, 140, 2, vc, C_BLACK, b);
    if (live && m->have_calc) snprintf(b, sizeof(b), "%.2f W", m->watts);
    else                      snprintf(b, sizeof(b), "-- W");
    field(&f_w, 178, 78, 140, 2, vc, C_BLACK, b);

    /* The device's own state, shown as sent -- not re-derived here from current. */
    const char *mode = (live && m->mode[0]) ? m->mode : "--";
    uint16_t    mc   = C_GREY;
    if      (!live)                                                          mc = C_GREY;
    else if (strcmp(m->mode, "FULL") == 0)                                   mc = C_CYAN;
    else if (strcmp(m->mode, "EMPTY") == 0)                                  mc = C_RED;
    else if (strcmp(m->mode, "CHARGE") == 0 || strcmp(m->mode, "ABSORB") == 0) mc = C_GREEN;
    else if (strcmp(m->mode, "DISCHARGE") == 0)                              mc = C_ORANGE;
    field(&f_mode, 178, 104, 140, 2, mc, C_BLACK, mode);

    char sub[64] = "";
    if (m->chem[0]) {
        snprintf(sub, sizeof(sub), "%s %dS", m->chem, m->cells);
        for (char *p = sub; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
    }
    field(&f_sub, 178, 126, 140, 1, C_GREY, C_BLACK, sub);

    /*
     * Capacity: what the pack was SET to -- its nameplate, `soc.cap_uah` -- and in
     * brackets what the monitor has MEASURED it to be, `soc.learned_uah`. Both, because
     * either alone is a half-answer: the nameplate is a claim about a new battery, the
     * learned figure is this one after however many cycles, and the gap between them is
     * the pack's state of health, readable at a glance only when they sit side by side.
     * They are equal until the gauge completes its first deep-enough span, and reading
     * them equal is itself the answer to "has it learned yet".
     *
     * Neither is computed here. The monitor's gauge does the learning between two
     * reference points (fuelgauge.c learn_capacity()) and reports both through `config`;
     * two devices watching one pack must never arrive at two capacities.
     *
     * Decimals are not cosmetic here. The learned figure leaves the nameplate by a
     * fraction of an Ah at first, so one decimal shows the pair as identical for the
     * whole early life of a pack -- exactly when someone is watching to see whether the
     * gauge has started learning at all. Two decimals on the measured value, one on the
     * set value: the same as the phone's Configure tab, deliberately, because two
     * screens rounding one pack's capacity differently is a bug report waiting to
     * happen.
     */
    char cap[32] = "";
    if (m->capacity_design_uah && m->learn_count == 0) {
        /* The monitor has measured nothing yet, so its "learned" figure is the
         * nameplate copied. Printing it as a measurement invites exactly the reading
         * it cannot support -- that the pack has been checked and came out at its
         * rating. */
        snprintf(cap, sizeof(cap), "CAP %.1fAH (learning)",
                 m->capacity_design_uah / 1000000.0);
    } else if (m->capacity_design_uah && m->capacity_learned_uah) {
        snprintf(cap, sizeof(cap), "CAP %.1fAH (%.2f)", m->capacity_design_uah / 1000000.0,
                 m->capacity_learned_uah / 1000000.0);
    } else if (m->capacity_design_uah) {
        snprintf(cap, sizeof(cap), "CAP %.1fAH", m->capacity_design_uah / 1000000.0);
    }
    field(&f_cap, 178, 136, 140, 1, C_GREY, C_BLACK, cap);

    /* Graph label, or why there is nothing to show; the environment on the right. */
    static field_t f_env;
    if (m->state == LINK_READY) {
        if (s_span_h > 0) {
            snprintf(b, sizeof(b), "SOC %d h", s_span_h);
        } else {
            snprintf(b, sizeof(b), "SOC"); /* no history fetched yet: no span to name */
        }
        field(&f_glabel, 4, 152, 96, 1, C_GREY, C_BLACK, b);
        char env[48] = "", part[16];
        if (m->have_env) {
            if (!isnan(m->temp_c)) {
                snprintf(part, sizeof(part), "%.1fC ", m->temp_c);
                strcat(env, part);
            }
            if (!isnan(m->humid_pct)) {
                snprintf(part, sizeof(part), "%.1f%% ", m->humid_pct);
                strcat(env, part);
            }
            if (!isnan(m->press_hpa)) {
                snprintf(part, sizeof(part), "%.1fhPa", m->press_hpa);
                strcat(env, part);
            }
        }
        /* Right-aligned in a fixed field, so a shorter reading erases a longer one. */
        char right[32];
        snprintf(right, sizeof(right), "%27.27s", env);
        field(&f_env, 154, 152, 162, 1, C_CYAN, C_BLACK, right);
    } else {
        field(&f_glabel, 4, 152, 312, 1, C_YELLOW, C_BLACK, m->note);
        f_env.text[0] = '\x01'; /* force a redraw once the label shrinks back */
    }
}

/* --- the board picker -------------------------------------------------------------- */

#define ROW_Y0 26
#define ROW_H  36
#define ROWS   5

static link_found_t s_rows[ROWS];
static int          s_nrows = -1;
static int64_t      s_forget_armed_us;

static void devices_draw(bool force)
{
    link_found_t now[ROWS];
    const int    n = link_found(now, ROWS);

    if (force) {
        lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
        lcd_fill(0, 0, LCD_W, 22, C_HEADER);
        lcd_text(6, 3, "DEVICES", 2, C_WHITE, C_HEADER);
        button(240, 0, 80, 22, "BACK", 2, C_HEADER, C_WHITE);
        s_forget_armed_us = 0;
        button(4, 206, 150, 32, "FORGET", 2, RGB(90, 30, 30), C_WHITE);
        lcd_text(164, 212, "scanning...", 1, C_GREY, C_BLACK);
        lcd_text(164, 224, "tap a board to use it", 1, C_GREY, C_BLACK);
    }
    if (!force && n == s_nrows && memcmp(now, s_rows, sizeof(link_found_t) * n) == 0) {
        return;
    }
    s_nrows = n;
    memcpy(s_rows, now, sizeof(link_found_t) * n);

    for (int i = 0; i < ROWS; i++) {
        const int y = ROW_Y0 + i * ROW_H;
        lcd_fill(0, y, LCD_W, ROW_H - 2, i < n ? C_PANEL : C_BLACK);
        if (i >= n) continue;
        const link_found_t *d = &s_rows[i];
        lcd_text(8, y + 4, d->name, 2, d->connected ? C_GREEN : C_WHITE, C_PANEL);
        char sub[48];
        snprintf(sub, sizeof(sub), "%d dBm%s%s", d->rssi, d->saved ? "   saved" : "",
                 d->connected ? "   connected" : "");
        lcd_text(8, y + 24, sub, 1, C_GREY, C_PANEL);
        lcd_text(300, y + 10, ">", 2, C_GREY, C_PANEL);
    }
    if (n == 0) {
        lcd_text(8, ROW_Y0 + 10, "No boards found yet. Is one powered up?", 1, C_GREY, C_BLACK);
    }
}

/* --- the pairing keypad -------------------------------------------------------------- */

static char s_digits[7];

static void keypad_digits(void)
{
    char shown[16];
    char d[6];
    for (int i = 0; i < 6; i++) d[i] = s_digits[i] ? s_digits[i] : '_';
    snprintf(shown, sizeof(shown), "%c %c %c %c %c %c", d[0], d[1], d[2], d[3], d[4], d[5]);
    const int w = lcd_text_w(shown, 3);
    lcd_fill(0, 46, LCD_W, 24, C_BLACK);
    lcd_text((LCD_W - w) / 2, 46, shown, 3, C_WHITE, C_BLACK);
}

static const char *KEYS[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "OK"};
#define KEY_X(c) (5 + (c) * 105)
#define KEY_Y(r) (76 + (r) * 41)
#define KEY_W    100
#define KEY_H    38

static void passkey_enter(const char *who)
{
    memset(s_digits, 0, sizeof(s_digits));
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    char title[48];
    snprintf(title, sizeof(title), "PAIR WITH %s", who);
    lcd_text(6, 6, title, 1, C_WHITE, C_BLACK);
    lcd_text(6, 22, "Type the 6-digit code on its display", 1, C_GREY, C_BLACK);
    button(236, 0, 84, 30, "CANCEL", 1, RGB(90, 30, 30), C_WHITE);
    for (int k = 0; k < 12; k++) {
        button(KEY_X(k % 3), KEY_Y(k / 3), KEY_W, KEY_H, KEYS[k], k == 9 ? 2 : 3,
               k == 11 ? RGB(30, 90, 40) : C_PANEL, C_WHITE);
    }
    keypad_digits();
}

static void passkey_tap(int tx, int ty)
{
    if (hit(tx, ty, 236, 0, 84, 30)) {
        link_passkey_cancel();
        return;
    }
    for (int k = 0; k < 12; k++) {
        if (!hit(tx, ty, KEY_X(k % 3), KEY_Y(k / 3), KEY_W, KEY_H)) continue;
        const size_t n = strlen(s_digits);
        if (k == 9) {
            if (n) s_digits[n - 1] = '\0';
        } else if (k == 11) {
            if (n == 6) link_passkey_submit((uint32_t)atoi(s_digits));
        } else if (n < 6) {
            s_digits[n] = KEYS[k][0];
        }
        keypad_digits();
        return;
    }
}

/* --- settings: board choice and calibration ------------------------------------------ */

/*
 * Calibration, as the phone app offers it (CLI.md §6): the two zero points, then the
 * two known values from a meter. Every action goes through a confirmation that states
 * its physical precondition -- the firmware cannot check any of them, and a zero point
 * taken with current flowing poisons the offset for good.
 */
typedef enum { ACT_ZERO_I, ACT_ZERO_V, ACT_TOP_I, ACT_TOP_V } action_t;

typedef struct {
    const char *title;    /* settings row */
    const char *sub;      /* settings row, second line */
    const char *confirm;  /* confirmation heading; %s is the entered value, if any */
    const char *body;     /* the precondition, in words */
    const char *unit;     /* "A" / "V" for the known-value actions, NULL otherwise */
    int         timeout_ms;
} action_def_t;

/* Timeouts follow CLI.md's samples x per-sample x 2 + 2 s, as the phone app does. */
static const action_def_t ACTIONS[] = {
    [ACT_ZERO_I] = {"ZERO CURRENT", "load disconnected",
                    "Zero the current?",
                    "THE LOAD MUST BE DISCONNECTED. With any current flowing this "
                    "poisons the offset permanently, and the firmware cannot tell. "
                    "One instant reading -- no averaging, no wait.",
                    NULL, 4000},
    [ACT_ZERO_V] = {"ZERO VOLTAGE", "VBUS tied to ground",
                    "Zero the voltage?",
                    "VBUS must be TIED TO GROUND, not just disconnected: a floating "
                    "input reads as a real voltage and gets baked in as the offset. "
                    "One instant reading -- no averaging, no wait.",
                    NULL, 4000},
    [ACT_TOP_I]  = {"MEASURED CURRENT", "type what your meter reads",
                    "Solve shunt from %s A?",
                    "The board works out the shunt resistance that makes it read this "
                    "current -- the shunt's value need not be known. Same direction as "
                    "the board measures. One instant reading -- no averaging, no wait; "
                    "applied and saved right away.",
                    "A", 4000},
    [ACT_TOP_V]  = {"MEASURED VOLTAGE", "type what your meter reads",
                    "Set voltage gain from %s V?",
                    "One instant reading -- no averaging, no wait; applied and saved "
                    "right away.",
                    "V", 4000},
};

#define SET_ROW_Y(i) (24 + (i) * 31)
#define SET_ROW_H    29
#define SET_ROWS     6 /* the board, the four calibration actions, firmware update */
#define ROW_UPDATE   5

static action_t s_action;
static char     s_entry[12];
static char     s_command[48];

static void settings_status(const link_model_t *m, bool force)
{
    static char last[2][56];
    link_cmd_t  c;
    link_command_status(&c);
    char l0[56] = "", l1[56] = "";

    if (c.busy) {
        const int el = (int)((esp_timer_get_time() - c.started_us) / 1000000);
        snprintf(l0, sizeof(l0), "running '%.30s'  %d s", c.cmd, el);
        snprintf(l1, sizeof(l1), "keep everything still until it answers");
    } else if (c.done) {
        if (!c.answered) {
            snprintf(l0, sizeof(l0), "'%.30s': no answer", c.cmd);
            snprintf(l1, sizeof(l1), "timed out, or the link dropped");
        } else {
            snprintf(l0, sizeof(l0), "'%.30s': %s", c.cmd, c.exit == 0 ? "done" : "REFUSED");
            /* The last line that says something: the refusal reason or "Saved". */
            for (int i = 2; i >= 0; i--) {
                if (c.reply[i][0]) {
                    snprintf(l1, sizeof(l1), "%.52s", c.reply[i]);
                    break;
                }
            }
        }
    } else if (m->state != LINK_READY) {
        snprintf(l0, sizeof(l0), "not connected: calibration is unavailable");
    }
    if (!force && strcmp(l0, last[0]) == 0 && strcmp(l1, last[1]) == 0) {
        return;
    }
    snprintf(last[0], sizeof(last[0]), "%s", l0);
    snprintf(last[1], sizeof(last[1]), "%s", l1);
    const uint16_t col = c.done && (!c.answered || c.exit != 0) ? C_ORANGE : C_GREY;
    lcd_text_field(4, 212, 316, l0, 1, c.busy ? C_YELLOW : col, C_BLACK);
    lcd_text_field(4, 226, 316, l1, 1, col, C_BLACK);
}

static void settings_row(int i, const char *title, const char *sub, bool enabled)
{
    const int      y  = SET_ROW_Y(i);
    const uint16_t fg = enabled ? C_WHITE : C_GREY;
    lcd_fill(0, y, LCD_W, SET_ROW_H, C_PANEL);
    lcd_text(8, y + 2, title, 2, fg, C_PANEL);
    lcd_text(8, y + 20, sub, 1, C_GREY, C_PANEL);
    lcd_text(300, y + 7, ">", 2, C_GREY, C_PANEL);
}

static void settings_draw(const link_model_t *m, bool force)
{
    static bool ready_last;
    static char name_last[24];
    const bool  ready = m->state == LINK_READY;
    if (force || ready != ready_last || strcmp(name_last, m->name) != 0) {
        if (force) {
            lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
            lcd_fill(0, 0, LCD_W, 22, C_HEADER);
            lcd_text(6, 3, "SETTINGS", 2, C_WHITE, C_HEADER);
            button(240, 0, 80, 22, "BACK", 2, C_HEADER, C_WHITE);
        }
        char board[40];
        snprintf(board, sizeof(board), "BOARD %s", m->name[0] ? m->name : "(none)");
        settings_row(0, board, "connect to a different board", true);
        for (int a = 0; a < 4; a++) {
            settings_row(a + 1, ACTIONS[a].title, ACTIONS[a].sub, ready);
        }
        settings_row(ROW_UPDATE, "FIRMWARE UPDATE", "update this display from the phone app", true);
        ready_last = ready;
        snprintf(name_last, sizeof(name_last), "%s", m->name);
    }
    settings_status(m, force);
}

/* --- the number keypad, for meter readings ----------------------------------------- */

static const char *NKEYS[16] = {"7", "8", "9", "DEL", "4", "5", "6", "-",
                                "1", "2", "3", ".", "BACK", "0", "USE", "OK"};
#define NK_X(c) (4 + (c) * 79)
#define NK_Y(r) (76 + (r) * 41)
#define NK_W    76
#define NK_H    38

static void numpad_entry(void)
{
    char shown[24];
    snprintf(shown, sizeof(shown), "%s %s", s_entry[0] ? s_entry : "_", ACTIONS[s_action].unit);
    lcd_fill(0, 44, LCD_W, 28, C_BLACK);
    lcd_text((LCD_W - lcd_text_w(shown, 3)) / 2, 46, shown, 3, C_WHITE, C_BLACK);
}

static void numpad_live(const link_model_t *m)
{
    static char last[32];
    char b[32];
    const bool amps = s_action == ACT_TOP_I;
    if (m->have_fast) snprintf(b, sizeof(b), "board reads %.4f %s", amps ? m->amps : m->volts,
                               amps ? "A" : "V");
    else              snprintf(b, sizeof(b), "board reads --");
    if (strcmp(b, last) == 0) return;
    snprintf(last, sizeof(last), "%s", b);
    lcd_text_field(6, 24, 300, b, 1, C_GREY, C_BLACK);
}

static void numpad_enter(const link_model_t *m)
{
    memset(s_entry, 0, sizeof(s_entry));
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    char title[40];
    snprintf(title, sizeof(title), "%s (%s)", ACTIONS[s_action].title, ACTIONS[s_action].unit);
    lcd_text(6, 6, title, 2, C_WHITE, C_BLACK);
    for (int k = 0; k < 16; k++) {
        const bool digit = NKEYS[k][0] >= '0' && NKEYS[k][0] <= '9' && !NKEYS[k][1];
        const uint16_t bg = k == 15 ? RGB(30, 90, 40) : k == 12 ? RGB(90, 30, 30) : C_PANEL;
        button(NK_X(k % 4), NK_Y(k / 4), NK_W, NK_H, NKEYS[k], digit ? 3 : 2, bg, C_WHITE);
    }
    numpad_entry();
    numpad_live(m);
}

static void confirm_enter(void);

/* Returns true when the keypad is done (OK or BACK). */
static bool numpad_tap(int tx, int ty, const link_model_t *m)
{
    for (int k = 0; k < 16; k++) {
        if (!hit(tx, ty, NK_X(k % 4), NK_Y(k / 4), NK_W, NK_H)) continue;
        const size_t n   = strlen(s_entry);
        const char  *key = NKEYS[k];
        if (strcmp(key, "DEL") == 0) {
            if (n) s_entry[n - 1] = '\0';
        } else if (strcmp(key, "-") == 0) {
            /* A sign toggle rather than a character: it can only ever lead. */
            if (s_entry[0] == '-') memmove(s_entry, s_entry + 1, n);
            else if (n < sizeof(s_entry) - 1) { memmove(s_entry + 1, s_entry, n + 1); s_entry[0] = '-'; }
        } else if (strcmp(key, ".") == 0) {
            if (!strchr(s_entry, '.') && n < sizeof(s_entry) - 1) s_entry[n] = '.';
        } else if (strcmp(key, "USE") == 0) {
            /* The board's own reading, as a starting point to type over. */
            if (m->have_fast) {
                snprintf(s_entry, sizeof(s_entry), s_action == ACT_TOP_I ? "%.4f" : "%.3f",
                         s_action == ACT_TOP_I ? m->amps : m->volts);
            }
        } else if (strcmp(key, "BACK") == 0) {
            return true;
        } else if (strcmp(key, "OK") == 0) {
            char *end = NULL;
            const double v = strtod(s_entry, &end);
            if (s_entry[0] && end && *end == '\0') {
                snprintf(s_command, sizeof(s_command), "cal top %s %ld",
                         s_action == ACT_TOP_I ? "i" : "v", lround(v * 1e6));
                confirm_enter();
            }
            return false;
        } else if (n < sizeof(s_entry) - 1) {
            s_entry[n] = key[0];
        }
        numpad_entry();
        return false;
    }
    return false;
}

/* --- the confirmation ---------------------------------------------------------------- */

static bool s_confirming;

/* Word-wraps `text` into lines of at most `cols` characters, drawing each. */
static int draw_wrapped(int x, int y, int cols, const char *text, uint16_t fg)
{
    char line[64];
    const char *p = text;
    while (*p) {
        int take = (int)strlen(p);
        if (take > cols) {
            take = cols;
            while (take > 0 && p[take] != ' ') take--;
            if (take == 0) take = cols;
        }
        snprintf(line, sizeof(line), "%.*s", take, p);
        lcd_text(x, y, line, 1, fg, C_BLACK);
        y += 12;
        p += take;
        while (*p == ' ') p++;
    }
    return y;
}

static void confirm_enter(void)
{
    s_confirming = true;
    const action_def_t *a = &ACTIONS[s_action];
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    char title[48];
    snprintf(title, sizeof(title), a->confirm, s_entry);
    lcd_text(6, 8, title, 2, C_YELLOW, C_BLACK);
    int y = draw_wrapped(6, 36, 51, a->body, C_WHITE);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "sends: %s", s_command);
    lcd_text(6, y + 8, cmd, 1, C_GREY, C_BLACK);
    button(10, 196, 140, 40, "CANCEL", 2, RGB(90, 30, 30), C_WHITE);
    button(170, 196, 140, 40, "RUN", 2, RGB(30, 90, 40), C_WHITE);
}

/* Returns true when the confirmation is finished, either way. */
static bool confirm_tap(int tx, int ty)
{
    if (hit(tx, ty, 10, 196, 140, 40)) {
        s_confirming = false;
        return true;
    }
    if (hit(tx, ty, 170, 196, 140, 40)) {
        link_command(s_command, ACTIONS[s_action].timeout_ms);
        s_confirming = false;
        return true;
    }
    return false;
}

/* --- firmware update of the remote itself --------------------------------------------- */

/*
 * While this screen is open the remote advertises as batmon-remote-XXXX and the phone
 * app can update it exactly as it updates the monitor (rcon.c). Leaving the screen
 * stops advertising, so a remote is never open to a new image by accident. A freshly
 * updated remote boots straight into this screen: the app has to be able to find it
 * again to confirm the new image, and so does the person holding it.
 */
static void update_draw(bool force)
{
    static field_t f_ver, f_link, f_i1, f_i2, f_prog, f_p1, f_p2;
    static int     bar_last = -2;
    static bool    btn_last;
    ota_status_t   o;
    rcon_status_t  r;
    ota_get_status(&o);
    rcon_status(&r);
    char b[64];

    if (force) {
        lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
        lcd_fill(0, 0, LCD_W, 22, C_HEADER);
        lcd_text(6, 3, "FIRMWARE UPDATE", 2, C_WHITE, C_HEADER);
        button(240, 0, 80, 22, "BACK", 2, C_HEADER, C_WHITE);
        bar_last = -2;
        btn_last = !o.pending;
    }

    snprintf(b, sizeof(b), "%s on %s", o.version, o.running);
    field(&f_ver, 6, 30, 308, 2, C_WHITE, C_BLACK, b);

    if (r.connected)        snprintf(b, sizeof(b), "phone connected");
    else if (r.advertising) snprintf(b, sizeof(b), "visible as %s", r.name);
    else                    snprintf(b, sizeof(b), "starting...");
    field(&f_link, 6, 54, 308, 1, r.connected ? C_GREEN : C_CYAN, C_BLACK, b);

    field(&f_i1, 6, 70, 308, 1, C_GREY, C_BLACK, "In the power-mon app: Bluetooth button, pick");
    snprintf(b, sizeof(b), "%s, then Firmware -> Update.", r.name);
    field(&f_i2, 6, 82, 308, 1, C_GREY, C_BLACK, b);

    /* Progress, from the OTA session itself rather than from anything the app says. */
    int bar = -1;
    if (o.phase == OTA_RECEIVING && o.size) {
        bar = (int)((uint64_t)o.received * 306 / o.size);
        snprintf(b, sizeof(b), "receiving %lu of %lu KB", (unsigned long)(o.received / 1024),
                 (unsigned long)(o.size / 1024));
    } else if (strcmp(o.boot, o.running) != 0) {
        snprintf(b, sizeof(b), "new image written -- restarting into it");
    } else {
        snprintf(b, sizeof(b), "waiting for the app");
    }
    field(&f_prog, 6, 104, 308, 1, bar >= 0 ? C_YELLOW : C_GREY, C_BLACK, b);
    if (bar != bar_last) {
        lcd_fill(6, 116, 308, 14, bar >= 0 ? C_GREY : C_BLACK);
        if (bar >= 0) {
            lcd_fill(7, 117, 306, 12, C_BLACK);
            if (bar > 0) lcd_fill(7, 117, bar, 12, C_GREEN);
        }
        bar_last = bar;
    }

    if (o.pending) {
        field(&f_p1, 6, 146, 308, 2, C_YELLOW, C_BLACK, "ON PROBATION");
        if (o.probation_left_s >= 0) {
            snprintf(b, sizeof(b), "Rolls back in %d min %02d s unless kept.",
                     o.probation_left_s / 60, o.probation_left_s % 60);
        } else {
            snprintf(b, sizeof(b), "Rolls back at the next reset unless kept.");
        }
        field(&f_p2, 6, 168, 308, 1, C_WHITE, C_BLACK, b);
    } else {
        snprintf(b, sizeof(b), "previous image: %s", o.spare_version[0] ? o.spare_version : "none");
        field(&f_p1, 6, 146, 308, 2, C_GREY, C_BLACK, "");
        field(&f_p2, 6, 168, 308, 1, C_GREY, C_BLACK, b);
    }
    if (force || btn_last != o.pending) {
        if (o.pending) {
            button(10, 196, 140, 40, "KEEP", 2, RGB(30, 90, 40), C_WHITE);
            button(170, 196, 140, 40, "ROLL BACK", 2, o.can_rollback ? RGB(90, 30, 30) : C_DIM,
                   C_WHITE);
        } else {
            lcd_fill(0, 196, LCD_W, 44, C_BLACK);
        }
        btn_last = o.pending;
    }
}

/* Returns true when the screen should be left. */
static bool update_tap(int tx, int ty)
{
    if (hit(tx, ty, 240, 0, 80, 22)) return true;
    ota_status_t o;
    ota_get_status(&o);
    if (o.pending && hit(tx, ty, 10, 196, 140, 40)) {
        ota_confirm();
    } else if (o.pending && o.can_rollback && hit(tx, ty, 170, 196, 140, 40)) {
        ota_rollback();
    }
    return false;
}

/* --- the loop ---------------------------------------------------------------------- */

static void go(screen_t scr)
{
    if (s_screen == SCR_DEVICES && scr != SCR_DEVICES) link_browse(false);
    if (scr == SCR_DEVICES) link_browse(true);
    /* Visible to the phone app only while the update screen is open. */
    if (s_screen == SCR_UPDATE && scr != SCR_UPDATE) rcon_advertise(false);
    if (scr == SCR_UPDATE) rcon_advertise(true);
    s_screen = scr;
    s_full   = true;
}

void ui_run(void)
{
    link_model_t m;
    int64_t      next_list = 0;

    s_screen = SCR_MAIN;
    s_full   = true;

    /* A new image on probation: open the update screen, so the app can find the remote
     * to confirm it and the person holding it can see what is going on. */
    ota_status_t boot;
    ota_get_status(&boot);
    if (boot.pending) {
        go(SCR_UPDATE);
    }

    for (;;) {
        link_get(&m);
        const int64_t now = esp_timer_get_time();

        /* Pairing interrupts whatever is on screen, and ends back on the dashboard. */
        char who[24];
        const bool wants = link_passkey_wanted(who, sizeof(who));
        if (wants && s_screen != SCR_PASSKEY) {
            go(SCR_PASSKEY);
        } else if (!wants && s_screen == SCR_PASSKEY) {
            go(SCR_MAIN);
        }

        const bool fresh = m.state == LINK_READY && m.have_calc &&
                           now - m.last_data_us < STALE_US;

        /* Redraw the graph when a fetch brought something new, and once a minute
         * anyway: its right edge is "now", so the points move left as time passes. */
        static uint32_t hist_seq_drawn;
        static int64_t  next_graph;
        if (s_screen == SCR_MAIN && !s_full) {
            static link_hist_t hh;
            link_history(&hh);
            if (hh.seq != hist_seq_drawn || now >= next_graph) {
                hist_seq_drawn = hh.seq;
                next_graph     = now + 60LL * 1000000;
                draw_graph();
            }
        }

        /* What the dashboard shows, on the serial log every 30 s: the one way to
         * check a remote without looking at it. */
        static int64_t next_log;
        if (now >= next_log) {
            next_log = now + 30LL * 1000000;
            link_hist_t lh;
            link_history(&lh);
            ESP_LOGI("ui", "%s %s: %.1f %% %.3f V %.4f A (avg %.4f) %s, cap %lu mAh, %s %dS, "
                           "rssi %s%d dBm (%d bars)",
                     m.name, fresh ? "live" : "no data", m.soc_pct, m.volts, m.amps,
                     m.amps_avg, m.mode, (unsigned long)m.capacity_mah, m.chem, m.cells,
                     m.have_rssi ? "" : "none/", m.have_rssi ? m.rssi_dbm : 0,
                     m.have_rssi ? rssi_bars(m.rssi_dbm) : 0);
            ESP_LOGI("ui", "env %s: %.2f C %.1f %% %.2f hPa; history %s, %d points, newest %lu s old",
                     m.have_env ? "yes" : "no", m.temp_c, m.humid_pct, m.press_hpa,
                     lh.supported ? "supported" : "unsupported", lh.count,
                     (unsigned long)lh.age_s);
        }

        int tx, ty;
        const bool tap = touch_tap(&tx, &ty);

        switch (s_screen) {
        case SCR_MAIN:
            if (tap && ty < 22) {
                go(SCR_SETTINGS);
                break;
            }
            if (tap && ty >= 150) {
                /* full span -> a quarter -> a half -> full again */
                const int full = s_span_full_h > 0 ? s_span_full_h : 48;
                s_span_h = s_span_h == full        ? (full / 4 > 0 ? full / 4 : full)
                           : s_span_h == full / 4  ? (full / 2 > 0 ? full / 2 : full)
                                                   : full;
                s_full = true; /* graph and its label */
            }
            if (s_full) main_enter();
            main_draw(&m);
            s_full = false;
            break;

        case SCR_DEVICES:
            if (tap) {
                if (hit(tx, ty, 240, 0, 80, 22)) {
                    go(SCR_SETTINGS);
                    break;
                }
                if (hit(tx, ty, 4, 206, 150, 32)) {
                    if (s_forget_armed_us && now - s_forget_armed_us < 3000000) {
                        link_forget();
                        go(SCR_MAIN);
                        break;
                    }
                    s_forget_armed_us = now;
                    button(4, 206, 150, 32, "TAP AGAIN", 2, C_RED, C_WHITE);
                }
                for (int i = 0; i < s_nrows; i++) {
                    if (hit(tx, ty, 0, ROW_Y0 + i * ROW_H, LCD_W, ROW_H)) {
                        link_select(&s_rows[i]);
                        go(SCR_MAIN);
                        break;
                    }
                }
                if (s_screen != SCR_DEVICES) break;
            }
            if (s_forget_armed_us && now - s_forget_armed_us >= 3000000) {
                s_forget_armed_us = 0;
                button(4, 206, 150, 32, "FORGET", 2, RGB(90, 30, 30), C_WHITE);
            }
            if (s_full || now >= next_list) {
                devices_draw(s_full);
                s_full    = false;
                next_list = now + 1000000;
            }
            break;

        case SCR_SETTINGS:
            if (tap) {
                if (hit(tx, ty, 240, 0, 80, 22)) {
                    go(SCR_MAIN);
                    break;
                }
                for (int i = 0; i < SET_ROWS; i++) {
                    if (!hit(tx, ty, 0, SET_ROW_Y(i), LCD_W, SET_ROW_H)) continue;
                    if (i == 0) {
                        go(SCR_DEVICES);
                    } else if (i == ROW_UPDATE) {
                        go(SCR_UPDATE);
                    } else if (m.state == LINK_READY) {
                        link_cmd_t c;
                        link_command_status(&c);
                        if (c.busy) break; /* one calibration at a time */
                        s_action   = (action_t)(i - 1);
                        memset(s_entry, 0, sizeof(s_entry));
                        if (s_action == ACT_ZERO_I || s_action == ACT_ZERO_V) {
                            snprintf(s_command, sizeof(s_command), "cal zero %s",
                                     s_action == ACT_ZERO_I ? "i" : "v");
                            go(SCR_CONFIRM);
                        } else {
                            go(SCR_NUMPAD);
                        }
                    }
                    break;
                }
                if (s_screen != SCR_SETTINGS) break;
            }
            settings_draw(&m, s_full);
            s_full = false;
            break;

        case SCR_NUMPAD:
            if (s_full) {
                numpad_enter(&m);
                s_full = false;
            }
            numpad_live(&m);
            if (tap) {
                if (numpad_tap(tx, ty, &m)) {
                    go(SCR_SETTINGS);
                } else if (s_confirming) {
                    s_screen = SCR_CONFIRM; /* confirm_enter() has already drawn it */
                }
            }
            break;

        case SCR_CONFIRM:
            if (s_full) {
                confirm_enter();
                s_full = false;
            }
            if (tap && confirm_tap(tx, ty)) {
                go(SCR_SETTINGS);
            }
            break;

        case SCR_UPDATE: {
            static int64_t next_update_draw;
            if (tap && update_tap(tx, ty)) {
                go(SCR_SETTINGS);
                break;
            }
            if (s_full || now >= next_update_draw) {
                update_draw(s_full);
                s_full           = false;
                next_update_draw = now + 250 * 1000;
            }
            break;
        }

        case SCR_PASSKEY:
            if (s_full) {
                passkey_enter(who);
                s_full = false;
            }
            if (tap) passkey_tap(tx, ty);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
