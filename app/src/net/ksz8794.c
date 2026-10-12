/*
 * KSZ8794CNX switch configuration over SPI.
 *
 * Copyright (c) 2026 Oskar von Heideken
 * SPDX-License-Identifier: Apache-2.0
 *
 * The KSZ8794's fabric forwards traffic on its own; there is no datapath
 * for the host to drive, which is why this is not a Zephyr switch driver.
 * What it does is fix the registers whose reset-strap defaults are wrong
 * for the wasp node board, and one of those is not optional:
 *
 *   Port 4 (the MCU-facing GMAC) comes up in RGMII mode. The strap pins
 *   LED3[1:0] select the mode and both have internal pull-ups, so they
 *   read '11' = RGMII; on this board those pins are wired through R22/R23
 *   to J5's RJ45 LEDs, so nothing pulls them down. The board is wired
 *   RMII. Until register 0x56 says RMII, port 4 neither speaks the right
 *   protocol nor emits the 50 MHz RMII reference clock the MCU's MAC runs
 *   on, and not one frame moves in either direction.
 *
 * Register and bit numbers below are from the KSZ8794CNX data sheet,
 * Microchip DS00002134E. Note that its register map (Table 4-2) wrongly
 * lists 0x56-0x58 as factory-test registers: it was copied from the
 * 5-port KSZ8795, so it labels two blocks "Port 4". The detailed register
 * descriptions have 0x50-0x56 as Port 4 Control 0-5 and Interface Control
 * 6, 0x40 as reserved, and the functional text refers to "Register 86
 * (0x56) bits[1:0]" for the interface mode in five separate places.
 */
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ksz8794.h"

LOG_MODULE_REGISTER(wasp_ksz, LOG_LEVEL_INF);

#define SWITCH_NODE DT_NODELABEL(eth_switch)

#if DT_NODE_HAS_STATUS(SWITCH_NODE, okay)

/*
 * SPI framing, from data sheet figures 3-7 and 3-8: a 24-bit frame of a
 * 3-bit opcode, a 12-bit register address, one turnaround bit, then 8
 * bits of data. So the 16-bit header is (opcode << 13) | (addr << 1).
 *
 * The prose in section 3.5.1.1 instead claims 8-bit opcodes 0x03 and
 * 0x02, which contradicts its own figures. The figures are right: the
 * same encoding appears in Linux's ksz8795_spi.c as SPI_ADDR_SHIFT 12 +
 * SPI_TURNAROUND_SHIFT 1 over opcodes 3 and 2.
 *
 * Data is latched on the rising SCL edge and driven on the falling edge,
 * which is SPI mode 0 — so no CPOL/CPHA flags below.
 */
#define KSZ_OP_READ  3U
#define KSZ_OP_WRITE 2U
#define KSZ_HDR(op, addr) ((uint16_t)(((op) << 13) | ((addr) << 1)))

/* Global registers */
#define REG_CHIP_ID0        0x00 /* RO, reads 0x87 */
#define REG_CHIP_ID1        0x01 /* [7:4] family (0x6 = 8794), [0] start switch */
#define REG_GLOBAL_CTRL4    0x06 /* SW4 duplex/speed *and* storm rate [10:8] */
#define REG_GLOBAL_CTRL5    0x07 /* storm rate [7:0] */

#define CHIP_ID0_KSZ8794    0x87
#define CHIP_ID1_FAMILY_MSK 0xF0
#define CHIP_ID1_FAMILY_94  0x60
#define CHIP_ID1_START_SW   BIT(0)

/* Global Control 4 bits that concern the MCU-facing RMII port. Both reset
 * to 0, which is already 100 Mbps full duplex — matching the DTS's
 * fixed-link "100BASE Full-Duplex". Asserted, not assumed, in configure().
 */
#define GC4_SW4_HALF_DUPLEX BIT(6)
#define GC4_SW4_10MBPS      BIT(4)
#define GC4_STORM_RATE_HI   GENMASK(2, 0)

/* Per-port Control 0: bit[7] enables ingress broadcast storm protection. */
#define REG_PORT_CTRL0(p)   (((p) == 4) ? 0x50 : ((p) * 0x10))
#define PORT_CTRL0_STORM_EN BIT(7)

/* Per-port Status 2: bit[5] link good, bit[6] autoneg done. 0x1E/0x2E/0x3E. */
#define REG_PORT_STATUS2(p) ((p) * 0x10 + 0x0E)
#define PORT_ST2_LINK_GOOD  BIT(5)
#define PORT_ST2_AN_DONE    BIT(6)

/* Port 4 Interface Control 6 (0x56). */
#define REG_PORT4_IF_CTRL6  0x56
#define IF_CTRL6_IF_MSK     GENMASK(1, 0)
#define IF_CTRL6_IF_MII     0x0
#define IF_CTRL6_IF_RMII    0x1
#define IF_CTRL6_IF_RGMII   0x3
#define IF_CTRL6_RMII_CLK   BIT(7) /* 1 = clock mode: switch drives RXC4 */

static const struct spi_dt_spec bus =
	SPI_DT_SPEC_GET(SWITCH_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);

static int ksz_read(uint8_t addr, uint8_t *val)
{
	const uint16_t hdr = KSZ_HDR(KSZ_OP_READ, addr);
	uint8_t tx[3] = { hdr >> 8, hdr & 0xFF, 0x00 };
	uint8_t rx[3] = { 0 };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };
	int ret = spi_transceive_dt(&bus, &txs, &rxs);

	if (ret == 0) {
		*val = rx[2];
	}
	return ret;
}

static int ksz_write(uint8_t addr, uint8_t val)
{
	const uint16_t hdr = KSZ_HDR(KSZ_OP_WRITE, addr);
	uint8_t tx[3] = { hdr >> 8, hdr & 0xFF, val };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };

	return spi_write_dt(&bus, &txs);
}

/* Read-modify-write. Several registers here mix unrelated fields —
 * Global Control 4 carries both the SW4 link parameters and the top bits
 * of the broadcast storm rate — so blind writes lose settings.
 */
static int ksz_update(uint8_t addr, uint8_t mask, uint8_t val)
{
	uint8_t cur, new;
	int ret = ksz_read(addr, &cur);

	if (ret < 0) {
		return ret;
	}
	new = (cur & ~mask) | (val & mask);
	if (new == cur) {
		return 0;
	}
	return ksz_write(addr, new);
}

/* Wait for the switch to answer on SPI. R6/C32 hold RST# low for ~100 ms
 * after power is stable (tSR needs only 10 ms), and the MCU can easily
 * boot and get here first, so poll rather than assume.
 */
static int ksz_wait_ready(void)
{
	const int64_t deadline = k_uptime_get() + 500;

	do {
		uint8_t id0;

		if (ksz_read(REG_CHIP_ID0, &id0) == 0 && id0 == CHIP_ID0_KSZ8794) {
			return 0;
		}
		k_msleep(10);
	} while (k_uptime_get() < deadline);

	return -ETIMEDOUT;
}

#if IS_ENABLED(CONFIG_WASP_KSZ8794_HW_RESET)
/*
 * Off by default, and not merely out of caution: REFCLKO feeds the MCU's
 * HSE through R26. Once the clock tree is moved to HSE (phase 2), holding
 * the switch in reset stops the clock the CPU is executing from. Only
 * ever safe while running on HSI.
 */
static int ksz_hw_reset(void)
{
	const struct gpio_dt_spec rst =
		GPIO_DT_SPEC_GET_OR(SWITCH_NODE, reset_gpios, { 0 });
	int ret;

	if (!gpio_is_ready_dt(&rst)) {
		LOG_ERR("reset-gpios not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&rst, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}
	k_msleep(15);                              /* tSR min 10 ms */
	ret = gpio_pin_set_dt(&rst, 0);            /* release (drive high) */
	if (ret < 0) {
		return ret;
	}
	k_msleep(10);
	return 0;
}
#endif /* CONFIG_WASP_KSZ8794_HW_RESET */

static void link_report_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)ksz8794_link_status();
}

static K_WORK_DELAYABLE_DEFINE(link_report, link_report_fn);

static int ksz8794_configure(void)
{
	uint8_t id1, if6, gc4;
	int ret;

	if (!spi_is_ready_dt(&bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

#if IS_ENABLED(CONFIG_WASP_KSZ8794_HW_RESET)
	ret = ksz_hw_reset();
	if (ret < 0) {
		LOG_ERR("hardware reset failed (%d)", ret);
		return ret;
	}
#endif

	ret = ksz_wait_ready();
	if (ret < 0) {
		LOG_ERR("no response on SPI3 — switch silent, Ethernet will not work");
		return ret;
	}

	ret = ksz_read(REG_CHIP_ID1, &id1);
	if (ret < 0) {
		return ret;
	}
	if ((id1 & CHIP_ID1_FAMILY_MSK) != CHIP_ID1_FAMILY_94) {
		LOG_ERR("unexpected chip ID1 0x%02x (want family 0x6 = 8794)", id1);
		return -ENODEV;
	}
	LOG_INF("KSZ8794 found, rev %u", (id1 >> 1) & 0x7);

	/*
	 * The fix. bits[1:0] = RMII, and bit[7] = clock mode so the switch
	 * drives the 50 MHz reference out of RXC4 into the MCU's PA1 —
	 * mandatory here because TXC4/REFCLKI4 is left unconnected, so
	 * normal (externally clocked) mode has nothing to lock to.
	 *
	 * bit[7] resets to 1 already; it is written anyway so the intent is
	 * recorded in one place rather than resting on a strap.
	 */
	ret = ksz_update(REG_PORT4_IF_CTRL6, IF_CTRL6_IF_MSK | IF_CTRL6_RMII_CLK,
			 IF_CTRL6_IF_RMII | IF_CTRL6_RMII_CLK);
	if (ret < 0) {
		LOG_ERR("could not set port 4 to RMII (%d)", ret);
		return ret;
	}

	ret = ksz_read(REG_PORT4_IF_CTRL6, &if6);
	if (ret < 0) {
		return ret;
	}
	if ((if6 & IF_CTRL6_IF_MSK) != IF_CTRL6_IF_RMII) {
		LOG_ERR("port 4 interface mode stuck at 0x%02x", if6);
		return -EIO;
	}
	LOG_INF("port 4: RMII, clock mode (0x56 = 0x%02x)", if6);

	/* 100 Mbps full duplex. Both bits reset to 0, so this only states
	 * the requirement; it is the half of the fixed-link in the DTS that
	 * lives on the switch side.
	 */
	ret = ksz_update(REG_GLOBAL_CTRL4, GC4_SW4_HALF_DUPLEX | GC4_SW4_10MBPS, 0);
	if (ret < 0) {
		return ret;
	}

	if (IS_ENABLED(CONFIG_WASP_KSZ8794_STORM_PROTECT)) {
		/*
		 * Broadcast storm protection, on the three external ports
		 * only. Port 4 is left alone on purpose: wasp's own node
		 * discovery is a UDP broadcast ANNOUNCE, and that ingresses
		 * at port 4.
		 *
		 * The rate is left at its reset value of 0x04A = 74
		 * 64-byte blocks per 50 ms interval, the data sheet's 1% of
		 * 100BT. Ample for ANNOUNCE traffic from a handful of nodes.
		 */
		for (int port = 1; port <= 3; port++) {
			ret = ksz_update(REG_PORT_CTRL0(port), PORT_CTRL0_STORM_EN,
					 PORT_CTRL0_STORM_EN);
			if (ret < 0) {
				LOG_WRN("port %d storm protect failed (%d)", port, ret);
			}
		}
		ret = ksz_read(REG_GLOBAL_CTRL4, &gc4);
		if (ret == 0) {
			uint8_t lo;

			if (ksz_read(REG_GLOBAL_CTRL5, &lo) == 0) {
				LOG_INF("storm protect on ports 1-3, rate 0x%03x",
					((gc4 & GC4_STORM_RATE_HI) << 8) | lo);
			}
		}
	}

	/*
	 * Not done here: self-address filtering (Global Control 18 bit[6]
	 * with the switch MAC in registers 0x68-0x6D), which docs/hardware.md
	 * lists as part of the plan. It drops frames whose source address
	 * matches the switch's own MAC *on egress*, which is what you want
	 * for a daisy chain that can loop a broadcast back. But if the
	 * configured MAC is the MCU's, then every frame the MCU transmits
	 * also matches on egress out of ports 1-3, and the data sheet does
	 * not say whether the ingress port is exempt. Untested, it is a
	 * plausible way to silently drop all transmits, so it waits until
	 * there is a second node to test a chain with.
	 */

	LOG_INF("switch configured; start-switch=%u",
		(id1 & CHIP_ID1_START_SW) ? 1 : 0);

	/* Autonegotiation on the external ports takes a second or two, so
	 * reporting the link here would only ever print "down". Defer it;
	 * knowing which jack came up is the first thing worth having when
	 * this board does not get an address.
	 */
	k_work_schedule(&link_report, K_SECONDS(3));
	return 0;
}

int ksz8794_link_status(void)
{
	int mask = 0;

	for (int port = 1; port <= 3; port++) {
		uint8_t st;
		int ret = ksz_read(REG_PORT_STATUS2(port), &st);

		if (ret < 0) {
			return ret;
		}
		if (st & PORT_ST2_LINK_GOOD) {
			mask |= BIT(port - 1);
		}
		LOG_INF("port %d (%s): link %s, autoneg %s", port,
			(port == 1) ? "J2" : (port == 2) ? "J4" : "J5",
			(st & PORT_ST2_LINK_GOOD) ? "up" : "down",
			(st & PORT_ST2_AN_DONE) ? "done" : "pending");
	}
	return mask;
}

/*
 * POST_KERNEL at 55: after the SPI driver (CONFIG_SPI_INIT_PRIORITY, which
 * defaults to KERNEL_INIT_PRIORITY_DEVICE = 50) and before the Ethernet
 * driver (CONFIG_ETH_INIT_PRIORITY = 60). The order matters in that
 * direction: the MAC's RMII reference clock is only 50 MHz once the switch
 * is in RMII clock mode, so the switch has to be fixed before the MAC
 * resets its DMA against that clock.
 */
SYS_INIT(ksz8794_configure, POST_KERNEL, 55);

#else /* no switch on this board */

int ksz8794_link_status(void)
{
	return -ENODEV;
}

#endif /* DT_NODE_HAS_STATUS(SWITCH_NODE, okay) */
