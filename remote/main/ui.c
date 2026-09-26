/*
 * ui.c — see ui.h.
 *
 * Layout, landscape 320 x 240:
 *
 *   +--------------------------------------------------------+  0
 *   | batmon-DCFA                              LIVE  (o)     |  header: tap -> Devices
 *   +----------------------------+---------------------------+ 22
 *   | STATE OF CHARGE            |  12.432 V                 |
 *   |  72.4 %                    |  -0.0089 A                |
 *   |  [#########-------]        |  -0.110 W                 |
 *   | TIME TO EMPTY              |  DISCHARGING              |
 *   |  3d 04h                    |  RESTING  FLOODED 6S      |
 *   +----------------------------+---------------------------+ 148
 *   | SOC HISTORY  6 h                               100      |
 *   |  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~    50      |  tap: 1 h / 6 h / 24 h
 *   |                                                  0      |
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

#include "history.h"
#include "lcd.h"
#include "link.h"
#include "touch.h"

typedef enum { SCR_MAIN, SCR_DEVICES, SCR_PASSKEY } screen_t;

#define STALE_US (5LL * 1000000) /* no record for this long: the numbers are old */

static screen_t s_screen;
static bool     s_full;              /* redraw everything on the next pass */
static int      s_span_min = 360;    /* graph span: 60, 360 or 1440 minutes */

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

static void fmt_duration(char *out, size_t n, float hours)
{
    if (!(hours >= 0) || hours > 99 * 24) {
        snprintf(out, n, ">99d");
    } else if (hours >= 24) {
        const int h = (int)hours;
        snprintf(out, n, "%dd %02dh", h / 24, h % 24);
    } else if (hours >= 1) {
        const int m = (int)(hours * 60);
        snprintf(out, n, "%dh %02dm", m / 60, m % 60);
    } else {
        snprintf(out, n, "%dm", (int)(hours * 60 + 0.5f));
    }
}

/* --- the dashboard ----------------------------------------------------------------- */

#define GX 4
#define GY 162
#define GW 284
#define GH 74

static void draw_graph(void)
{
    static uint16_t pts[HISTORY_POINTS];
    static int16_t  ycol[GW];
    history_get(pts, s_span_min);

    /* One column per pixel: the newest valid point that falls in it. With fewer
     * minutes than pixels (the 1 h span) a point spans several columns. */
    for (int c = 0; c < GW; c++) {
        const int i0 = c * s_span_min / GW;
        int       i1 = (c + 1) * s_span_min / GW;
        if (i1 <= i0) i1 = i0 + 1;
        uint16_t v = HISTORY_NONE;
        for (int i = i0; i < i1; i++) {
            if (pts[i] != HISTORY_NONE) v = pts[i];
        }
        ycol[c] = (v == HISTORY_NONE) ? -1 : (int16_t)(GH - 1 - (int)v * (GH - 1) / 1000);
    }

    const uint16_t bg = C_PANEL, grid = C_DIM, line = C_GREEN, fill = RGB(20, 70, 35);
    uint16_t *buf = lcd_strip();
    const int rows = LCD_STRIP_PX / GW;
    for (int y0 = 0; y0 < GH; y0 += rows) {
        const int h = (y0 + rows > GH) ? GH - y0 : rows;
        for (int r = 0; r < h; r++) {
            const int  y      = y0 + r;
            const bool gridln = (y == GH / 4 || y == GH / 2 || y == 3 * GH / 4);
            for (int c = 0; c < GW; c++) {
                uint16_t px = (gridln && (c & 3) == 0) ? grid : bg;
                const int yc = ycol[c];
                if (yc >= 0) {
                    if (y == yc || y == yc + 1) px = line;
                    else if (y > yc)            px = fill;
                }
                buf[r * GW + c] = px;
            }
        }
        lcd_blit(GX, GY + y0, GW, h, buf);
    }
    if (history_count() == 0) {
        const char *msg = "no data yet";
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
        f_glabel;
    static int     bar_last = -2;
    static uint16_t bar_col;
    static uint16_t dot_last = 1;

    const int64_t now  = esp_timer_get_time();
    const bool    live = m->state == LINK_READY && m->have_fast &&
                      now - m->last_data_us < STALE_US;
    char b[32];

    /* Header. */
    field(&f_name, 6, 3, 190, 2, C_WHITE, C_HEADER, m->name[0] ? m->name : "tap: pick a board");
    const char *st;
    uint16_t    sc;
    switch (m->state) {
    case LINK_SCANNING:   st = "SEARCHING";  sc = C_BLUE;   break;
    case LINK_CONNECTING: st = "CONNECTING"; sc = C_YELLOW; break;
    case LINK_SETUP:      st = "SETUP";      sc = C_YELLOW; break;
    case LINK_PAIRING:    st = "PAIRING";    sc = C_ORANGE; break;
    default:              st = live ? (m->secure ? "LIVE+PAIRED" : "LIVE") : "NO DATA";
                          sc = live ? C_GREEN : C_ORANGE;   break;
    }
    field(&f_status, 222, 8, 72, 1, sc, C_HEADER, st);
    if (s_full || dot_last != sc) {
        lcd_fill(300, 5, 12, 12, sc);
        dot_last = sc;
    }

    /* State of charge. */
    const bool  have_soc = live && m->have_calc;
    const float soc      = m->soc_pct;
    const uint16_t soc_c = !have_soc ? C_GREY : soc > 50 ? C_GREEN : soc > 20 ? C_YELLOW : C_RED;
    if (!have_soc)          snprintf(b, sizeof(b), "--");
    else if (soc >= 99.95f) snprintf(b, sizeof(b), "100");
    else                    snprintf(b, sizeof(b), "%.1f", soc);
    field(&f_soc, 6, 40, 150, 6, soc_c, C_BLACK, b);

    const int bar = have_soc ? (int)(soc * 162 / 100) : -1;
    if (s_full || bar != bar_last || bar_col != soc_c) {
        lcd_fill(6, 96, 164, 12, C_GREY);
        lcd_fill(7, 97, 162, 10, C_BLACK);
        if (bar > 0) lcd_fill(7, 97, bar, 10, soc_c);
        bar_last = bar;
        bar_col  = soc_c;
    }

    /* Time to empty / full, from the ~1 min average current rather than the latest
     * sample: a load switching on and off would otherwise make it jump by days. */
    const float i     = m->amps_avg;
    const float cap   = m->capacity_mah / 1000.0f;
    const char *label = "TIME ESTIMATE";
    if (!live || !m->have_calc) {
        snprintf(b, sizeof(b), "--");
    } else if (strcmp(m->mode, "FULL") == 0 && i > -0.005f) {
        snprintf(b, sizeof(b), "full");
    } else if (i < -0.005f) {
        label = "TIME TO EMPTY";
        fmt_duration(b, sizeof(b), m->charge_ah / -i);
    } else if (i > 0.005f && cap > 0) {
        label = "TIME TO FULL";
        const float left = cap - m->charge_ah;
        fmt_duration(b, sizeof(b), left > 0 ? left / i : 0);
    } else {
        snprintf(b, sizeof(b), "idle");
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

    const char *mode;
    uint16_t    mc;
    if (!live)                              { mode = "--";          mc = C_GREY;   }
    else if (strcmp(m->mode, "FULL") == 0)  { mode = "FULL";        mc = C_CYAN;   }
    else if (strcmp(m->mode, "EMPTY") == 0) { mode = "EMPTY";       mc = C_RED;    }
    else if (m->amps > 0.005f)              { mode = "CHARGING";    mc = C_GREEN;  }
    else if (m->amps < -0.005f)             { mode = "DISCHARGING"; mc = C_ORANGE; }
    else                                    { mode = "IDLE";        mc = C_GREY;   }
    field(&f_mode, 178, 104, 140, 2, mc, C_BLACK, mode);

    char chem[32] = "", sub[64];
    if (m->chem[0]) {
        snprintf(chem, sizeof(chem), " %s %dS", m->chem, m->cells);
        for (char *p = chem; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
    }
    snprintf(sub, sizeof(sub), "%s%s", live && m->have_calc ? m->mode : "", chem);
    field(&f_sub, 178, 126, 140, 1, C_GREY, C_BLACK, sub);

    /* Graph label, or why there is nothing to show. */
    if (m->state == LINK_READY) {
        snprintf(b, sizeof(b), "SOC HISTORY  %s", s_span_min == 60 ? "1 h" :
                                                  s_span_min == 360 ? "6 h" : "24 h");
        field(&f_glabel, 4, 152, 312, 1, C_GREY, C_BLACK, b);
    } else {
        field(&f_glabel, 4, 152, 312, 1, C_YELLOW, C_BLACK, m->note);
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

/* --- the loop ---------------------------------------------------------------------- */

static void go(screen_t scr)
{
    if (s_screen == SCR_DEVICES && scr != SCR_DEVICES) link_browse(false);
    if (scr == SCR_DEVICES) link_browse(true);
    s_screen = scr;
    s_full   = true;
}

void ui_run(void)
{
    link_model_t m;
    int64_t      next_minute = 0, next_list = 0;
    bool         recording   = false;

    s_screen = SCR_MAIN;
    s_full   = true;

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

        /* History: a point a minute from the first data onward, a gap while down. */
        const bool fresh = m.state == LINK_READY && m.have_calc &&
                           now - m.last_data_us < STALE_US;
        if (!recording && fresh) {
            recording   = true;
            next_minute = now; /* the first point straight away */
        }
        if (recording && now >= next_minute) {
            history_push(fresh ? (uint16_t)lroundf(m.soc_pct * 10.0f) : HISTORY_NONE);
            next_minute += 60LL * 1000000;
            if (s_screen == SCR_MAIN && !s_full) draw_graph();
        }

        /* What the dashboard shows, on the serial log every 30 s: the one way to
         * check a remote without looking at it. */
        static int64_t next_log;
        if (now >= next_log) {
            next_log = now + 30LL * 1000000;
            ESP_LOGI("ui", "%s %s: %.1f %% %.3f V %.4f A (avg %.4f) %s, cap %lu mAh, %s %dS",
                     m.name, fresh ? "live" : "no data", m.soc_pct, m.volts, m.amps,
                     m.amps_avg, m.mode, (unsigned long)m.capacity_mah, m.chem, m.cells);
        }

        int tx, ty;
        const bool tap = touch_tap(&tx, &ty);

        switch (s_screen) {
        case SCR_MAIN:
            if (tap && ty < 22) {
                go(SCR_DEVICES);
                break;
            }
            if (tap && ty >= 150) {
                s_span_min = s_span_min == 60 ? 360 : s_span_min == 360 ? 1440 : 60;
                s_full = true; /* graph and its label */
            }
            if (s_full) main_enter();
            main_draw(&m);
            s_full = false;
            break;

        case SCR_DEVICES:
            if (tap) {
                if (hit(tx, ty, 240, 0, 80, 22)) {
                    go(SCR_MAIN);
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
