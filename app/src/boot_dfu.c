/*
 * Entry into the SoC's ROM USB bootloader, for reflashing without a
 * debugger.
 *
 * Copyright (c) 2026 Oskar von Heideken
 * SPDX-License-Identifier: Apache-2.0
 *
 * The STM32H7 carries a DFU-capable bootloader in system memory. Holding
 * SW2 (BOOT0 high, against R27's 10k pull-down) through a reset runs it
 * with no help from us at all. This file is the other way in: a
 * coordinator can ask for it over the network, the way an RP2040 node is
 * already asked for BOOTSEL, so a board on a shelf can be reflashed
 * without anybody reaching for the button.
 *
 * Jumping straight there from a running system is the obvious
 * implementation and the wrong one. By the time a request arrives this
 * MCU is at 550 MHz on a PLL fed from the Ethernet switch, with both
 * caches on, Ethernet DMA running and interrupts live; the ROM
 * bootloader expects none of that, and tearing it all down by hand is a
 * long list of things to get right once and then quietly break.
 *
 * So instead: leave a magic word in uninitialised RAM, reset, and jump
 * before anything has been set up. NVIC_SystemReset does not clear SRAM
 * and __noinit is not zeroed by the C startup, so the word survives the
 * reset that discards all the state we would otherwise have had to undo.
 * At PRE_KERNEL_1 priority 0 the clock driver (priority 1) has not run,
 * so the chip is still on its reset defaults — which is exactly the
 * state the ROM bootloader was written for.
 *
 * The address is not taken on trust. 0x1FF09800 is widely reported for
 * the H7, but reports are mostly from H743 silicon, so the vector is
 * read and sanity-checked first: a stack pointer somewhere in RAM and a
 * Thumb entry point inside system memory. Read over SWD on this board it
 * is SP=0x24003f40, PC=0x1ff1393d, which passes. If some future part
 * disagrees, the check fails and the node boots normally instead of
 * hanging somewhere unreachable.
 */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "wasp.h"

LOG_MODULE_REGISTER(wasp_dfu, LOG_LEVEL_INF);

#if defined(CONFIG_SOC_SERIES_STM32H7X)

#include <cmsis_core.h>

/* System memory vector table. Verified by reading it on the target. */
#define BOOTLOADER_VECTORS 0x1FF09800UL
#define SYSMEM_BASE        0x1FF00000UL
#define SYSMEM_END         0x1FF20000UL

/* Any even, unlikely value; it only has to not occur by accident in RAM
 * that nothing initialises.
 */
#define DFU_MAGIC 0xB0075EL

static uint32_t dfu_request __noinit;

/*
 * Diagnostics, surviving the reset alongside the request so the outcome
 * of an attempt can be reported once logging is up. Two distinct tags,
 * because "the vector looked wrong" and "we jumped and ended up back
 * here" are very different failures and the first version of this could
 * not tell them apart.
 */
static uint32_t dfu_dbg_tag __noinit;
static uint32_t dfu_dbg_sp __noinit;
static uint32_t dfu_dbg_pc __noinit;
#define DFU_DBG_REJECTED 0x5A5AA5A5UL
#define DFU_DBG_JUMPED   0x1234BEEFUL

bool wasp_boot_dfu_supported(void)
{
	return true;
}

void wasp_boot_dfu(void)
{
	dfu_request = DFU_MAGIC;
	sys_reboot(SYS_REBOOT_COLD);
}

static int dfu_entry(void)
{
	uint32_t sp, pc;

	if (dfu_request != DFU_MAGIC) {
		return 0;
	}
	/* Clear first: if the jump below is wrong, the next reset must boot
	 * the application rather than retry forever.
	 */
	dfu_request = 0;

	sp = *(volatile uint32_t *)BOOTLOADER_VECTORS;
	pc = *(volatile uint32_t *)(BOOTLOADER_VECTORS + 4);
	dfu_dbg_sp = sp;
	dfu_dbg_pc = pc;

	/* A plausible Cortex-M vector: stack in RAM, Thumb entry inside
	 * system memory. Anything else and we carry on booting.
	 */
	if (sp < CONFIG_SRAM_BASE_ADDRESS ||
	    sp > CONFIG_SRAM_BASE_ADDRESS + KB(CONFIG_SRAM_SIZE) ||
	    pc < SYSMEM_BASE || pc >= SYSMEM_END || (pc & 1U) == 0U) {
		dfu_dbg_tag = DFU_DBG_REJECTED;
		return 0;
	}

	/* Past the check, so any later appearance of this tag means the
	 * bootloader handed control back rather than staying resident.
	 */
	dfu_dbg_tag = DFU_DBG_JUMPED;

	__disable_irq();

	/* Caches are enabled before PRE_KERNEL_1. Clean the data cache
	 * rather than merely disabling it, or dirty lines are dropped.
	 */
	SCB_CleanInvalidateDCache();
	SCB_DisableDCache();
	SCB_DisableICache();

	/* Nothing has been configured yet, but the reset defaults still
	 * leave SysTick and any latched NVIC state to clear.
	 */
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;
	for (unsigned int i = 0; i < ARRAY_SIZE(NVIC->ICER); i++) {
		NVIC->ICER[i] = 0xFFFFFFFFU;
		NVIC->ICPR[i] = 0xFFFFFFFFU;
	}

	SCB->VTOR = BOOTLOADER_VECTORS;
	__DSB();
	__ISB();
	__set_MSP(sp);
	((void (*)(void))pc)();

	CODE_UNREACHABLE;
}

/* Report what the last attempt saw, if there was one. */
static int dfu_report(void)
{
	uint32_t tag = dfu_dbg_tag;

	/* Clear whatever is there, not just a value we recognise: these
	 * live in uninitialised RAM and survive a reflash, so a tag left by
	 * a previous firmware would otherwise be reported on every boot.
	 */
	dfu_dbg_tag = 0;

	if (tag == DFU_DBG_REJECTED) {
		LOG_WRN("DFU: vector at 0x%08x failed its sanity check "
			"(SP=0x%08x PC=0x%08x) — booted normally instead",
			(unsigned int)BOOTLOADER_VECTORS, dfu_dbg_sp, dfu_dbg_pc);
	} else if (tag == DFU_DBG_JUMPED) {
		/*
		 * The jump ran and the ROM bootloader gave control back. Per
		 * AN2606 the H7 bootloader samples its interfaces and leaves
		 * for the application when it finds no host, so the usual
		 * cause is the board's USB-C not being a data link to a PC —
		 * a charger or a power-only cable looks the same to it.
		 */
		LOG_WRN("DFU: jumped to the ROM bootloader and it returned. "
			"It leaves for the application when no USB host "
			"answers — check USB-C (J7) is a data cable to a PC.");
	}
	return 0;
}
SYS_INIT(dfu_report, POST_KERNEL, 4);

/*
 * PRE_KERNEL_1 priority 0: ahead of the clock driver at priority 1, so
 * the PLLs are still off and the chip is on its reset-default HSI. That
 * ordering is the whole design — see the file comment.
 */
SYS_INIT(dfu_entry, PRE_KERNEL_1, 0);

#else

bool wasp_boot_dfu_supported(void)
{
	return false;
}

void wasp_boot_dfu(void)
{
}

#endif /* CONFIG_SOC_SERIES_STM32H7X */
