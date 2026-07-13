/*
 * Status LED — one glance tells you what a node is doing:
 *
 *   fast blink (5 Hz)   no network yet (WiFi joining / DHCP)
 *   heartbeat (short blip every second)   online, waiting for a coordinator
 *   solid on            coordinator connected
 *   frantic strobe      IDENTIFY received — "here I am"
 *
 * Compiles to no-ops on boards without an led0 devicetree alias.
 */
#ifndef WASP_LED_H_
#define WASP_LED_H_

#include <stdint.h>

enum wasp_led_state {
	WASP_LED_NET_DOWN,  /* fast blink */
	WASP_LED_READY,     /* heartbeat */
	WASP_LED_CONNECTED, /* solid */
};

void wasp_led_set(enum wasp_led_state state);

/* Strobe for duration_ms so a human can spot the board, then revert. */
void wasp_led_identify(uint32_t duration_ms);

#endif /* WASP_LED_H_ */
