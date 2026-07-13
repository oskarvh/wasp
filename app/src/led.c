/*
 * Status LED driver thread (see led.h for the patterns).
 *
 * A dedicated thread rather than a k_timer because setting the pin may
 * block: on the Pico W the LED hangs off the CYW43439 WiFi chip and a
 * write is a WHD ioctl over SPI — nothing an ISR may touch.
 */
#include <zephyr/kernel.h>

#include "led.h"

#if DT_NODE_EXISTS(DT_ALIAS(led0)) && defined(CONFIG_WASP_STATUS_LED)

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wasp_led, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static atomic_t led_state = ATOMIC_INIT(WASP_LED_NET_DOWN);
static atomic_t identify_until; /* k_uptime_get_32() deadline, 0 = inactive */

void wasp_led_set(enum wasp_led_state state)
{
	atomic_set(&led_state, state);
}

void wasp_led_identify(uint32_t duration_ms)
{
	atomic_set(&identify_until, (atomic_val_t)(k_uptime_get_32() + duration_ms));
}

/* Set the pin and report the tick's sleep time. Split into on-time and
 * off-time per period so state changes are picked up quickly. */
static void led_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* The CYW43 GPIO controller is only usable once the WiFi driver
	 * has initialized; be patient rather than sorry. */
	while (!gpio_is_ready_dt(&led)) {
		k_sleep(K_MSEC(100));
	}
	if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_WRN("cannot configure status LED, giving up");
		return;
	}
	LOG_INF("status LED on %s pin %u", led.port->name, led.pin);

	bool on = false;

	while (true) {
		uint32_t now = k_uptime_get_32();
		int32_t sleep_ms;

		if ((int32_t)(atomic_get(&identify_until) - now) > 0) {
			on = !on; /* 10 Hz strobe: unmissable */
			sleep_ms = 50;
		} else {
			switch (atomic_get(&led_state)) {
			case WASP_LED_NET_DOWN:
				on = !on;
				sleep_ms = 100;
				break;
			case WASP_LED_CONNECTED:
				on = true;
				sleep_ms = 200;
				break;
			case WASP_LED_READY:
			default:
				on = !on;
				sleep_ms = on ? 80 : 920;
				break;
			}
		}
		(void)gpio_pin_set_dt(&led, on);
		k_sleep(K_MSEC(sleep_ms));
	}
}

/* 2 KiB stack: a pin write on the Pico W goes through the WHD driver. */
K_THREAD_DEFINE(wasp_led_tid, 2048, led_thread_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(12), 0, 0);

#else /* no led0 alias or LED disabled */

void wasp_led_set(enum wasp_led_state state)
{
	ARG_UNUSED(state);
}

void wasp_led_identify(uint32_t duration_ms)
{
	ARG_UNUSED(duration_ms);
}

#endif
