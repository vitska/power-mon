/*
 * led.c — see led.h.
 *
 * Still three on/off colours, but "on" is PWM at BOARD_LED_DUTY_PCT, not the pin held
 * fully on: at full drive the LED is glaring in a dim room and lights the bench around
 * it. LEDC, 5 kHz -- far above any flicker the eye or a phone camera picks up -- one
 * timer for all three channels.
 *
 * The LED is common anode, so a channel is lit with its pin LOW (BOARD_LED_ON 0). LEDC
 * inverts the output for that, so a duty here is always the share of time LIT.
 */

#include "led.h"

#include "board.h"

#if BOARD_HAS_RGB_LED

#include "driver/ledc.h"
#include "esp_timer.h"

#define LED_MODE   LEDC_LOW_SPEED_MODE
#define LED_TIMER  LEDC_TIMER_1             /* 0 left free for anything else */
#define LED_RES    LEDC_TIMER_8_BIT
#define LED_FULL   ((1u << 8) - 1)
#define LED_DUTY   ((LED_FULL * BOARD_LED_DUTY_PCT + 50) / 100)

enum { CH_R, CH_G, CH_B };
static const int      PINS[3] = {BOARD_LED_R, BOARD_LED_G, BOARD_LED_B};
static bool           s_on[3];

static void set(int ch, bool on)
{
    if (s_on[ch] == on) return; /* every 30 ms from the UI loop: touch LEDC on change only */
    s_on[ch] = on;
    ledc_set_duty(LED_MODE, (ledc_channel_t)ch, on ? LED_DUTY : 0);
    ledc_update_duty(LED_MODE, (ledc_channel_t)ch);
}

void led_init(void)
{
    const ledc_timer_config_t t = {
        .speed_mode      = LED_MODE,
        .duty_resolution = LED_RES,
        .timer_num       = LED_TIMER,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    for (int ch = 0; ch < 3; ch++) {
        const ledc_channel_config_t c = {
            .gpio_num   = PINS[ch],
            .speed_mode = LED_MODE,
            .channel    = (ledc_channel_t)ch,
            .timer_sel  = LED_TIMER,
            .duty       = 0,
            .flags.output_invert = BOARD_LED_ON == 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&c));
    }
    set(CH_R, true); /* not connected yet */
}

void led_update(const link_model_t *m)
{
    static uint32_t seen_rx, seen_tx;
    static int64_t  blink_until;
    const int64_t   now = esp_timer_get_time();

    /* Connected is anything from SETUP on: the link is up, whether or not the
     * handshake or the pairing has finished. */
    const bool connected = m->state != LINK_SCANNING && m->state != LINK_CONNECTING;
    set(CH_R, !connected);

    /* The same blink lengths as the header's icon (ui.c): a command outlasts the
     * notifications that answer it, so it shows. */
    if (m->tx_packets != seen_tx) {
        blink_until = now + 150 * 1000;
    } else if (m->rx_packets != seen_rx && now >= blink_until) {
        blink_until = now + 60 * 1000;
    }
    seen_tx = m->tx_packets;
    seen_rx = m->rx_packets;
    set(CH_B, now < blink_until);
}

#else

void led_init(void) {}
void led_update(const link_model_t *m) { (void)m; }

#endif
