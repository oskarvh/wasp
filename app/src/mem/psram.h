/*
 * Quad PSRAM on OCTOSPI, memory-mapped.
 *
 * Copyright (c) 2026 Oskar von Heideken
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WASP_MEM_PSRAM_H
#define WASP_MEM_PSRAM_H

#include <stddef.h>
#include <stdbool.h>

/* True once the PSRAM is mapped and usable. */
bool wasp_psram_ready(void);

/* Base address of the memory-mapped window, or NULL if unavailable. */
void *wasp_psram_base(void);

/* Usable size in bytes, 0 if unavailable. */
size_t wasp_psram_size(void);

#endif /* WASP_MEM_PSRAM_H */
