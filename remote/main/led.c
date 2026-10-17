/*
 * led.c — see led.h.
 *
 * Plain GPIO, no PWM: three on/off colours say all this needs to. The LED is common
 * anode, so a channel is lit with its pin LOW; BOARD_LED_ON carries that.
 */

#include "led.h"

#include "board.h"

#if BOARD_HAS_RGB_LED

#include "driver/gpio.h"
#include "esp_timer.h"

static void set(int pin, bool on)
{
    gpio_set_level(pin, on ? BOARD_LED_ON : !BOARD_LED_ON);
}

void led_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_LED_R) | (1ULL << BOARD_LED_G) | (1ULL << BOARD_LED_B),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    set(BOARD_LED_R, true); /* not connected yet */
    set(BOARD_LED_G, false);
    set(BOARD_LED_B, false);
}

void led_update(const link_model_t *m)
{
    static uint32_t seen_rx, seen_tx;
    static int64_t  blink_until;
    const int64_t   now = esp_timer_get_time();

    /* Connected is anything from SETUP on: the link is up, whether or not the
     * handshake or the pairing has finished. */
    const bool connected = m->state != LINK_SCANNING && m->state != LINK_CONNECTING;
    set(BOARD_LED_R, !connected);

    /* The same blink lengths as the header's icon (ui.c): a command outlasts the
     * notifications that answer it, so it shows. */
    if (m->tx_packets != seen_tx) {
        blink_until = now + 150 * 1000;
    } else if (m->rx_packets != seen_rx && now >= blink_until) {
        blink_until = now + 60 * 1000;
    }
    seen_tx = m->tx_packets;
    seen_rx = m->rx_packets;
    set(BOARD_LED_B, now < blink_until);
}

#else

void led_init(void) {}
void led_update(const link_model_t *m) { (void)m; }

#endif
