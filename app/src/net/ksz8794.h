/*
 * KSZ8794CNX switch configuration over SPI.
 *
 * Copyright (c) 2026 Oskar von Heideken
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WASP_NET_KSZ8794_H
#define WASP_NET_KSZ8794_H

#include <stdbool.h>
#include <stdint.h>

/* Log the link state of the three external ports. Returns a bitmask of
 * ports with a good link (bit 0 = port 1 = J2) or a negative errno. Safe
 * to call at any time; the switch is readable whenever SPI3 is up.
 */
int ksz8794_link_status(void);

#endif /* WASP_NET_KSZ8794_H */
