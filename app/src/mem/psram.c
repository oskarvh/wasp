/*
 * Vilsion VTI7064 64 Mbit quad PSRAM on STM32 OCTOSPI1, memory-mapped.
 *
 * Copyright (c) 2026 Oskar von Heideken
 * SPDX-License-Identifier: Apache-2.0
 *
 * Why this is not Zephyr's st,stm32-ospi-psram driver: that one is an
 * octal HyperRAM/Xccela driver for AP Memory APS256XX-class parts. It
 * drives 8-line instruction, address and data phases, sets
 * MEMTYPE_APMEMORY, and configures APS256XX mode registers over a
 * DQS-clocked DTR bus. This is a 4-line SOIC-8 part. Its Kconfig also
 * gates on SOC_SERIES_STM32U5X, but relaxing that would only have moved
 * the failure later.
 *
 * Sequence: reset the device in SPI mode, read its ID, switch it to QPI,
 * then hand the OCTOSPI a read and a write command template and turn on
 * memory-mapped mode. After that the array behaves like RAM at
 * 0x90000000 and nothing in this file runs again.
 *
 * Command set (VTI7064xxxxx Rev 0.1, section 4.5), in QPI mode:
 *
 *   0xEB  fast read quad    quad cmd/addr/data, 6 wait cycles, 104 MHz
 *   0x38  quad write        quad cmd/addr/data, 0 wait cycles, 104 MHz
 *   0x35  enter QPI         (issued while still in SPI mode)
 *   0x66  reset enable      }  both accepted in either mode
 *   0x99  reset             }
 *   0x9F  read ID           SPI mode only
 *
 * ABANDONED ON V1 — bulk memory-mapped writes do not work, and will not.
 *
 * What works, measured: reset from either SPI or QPI state, read ID, QPI
 * entry, indirect QPI round trips, memory-mapped reads of the whole array,
 * and short memory-mapped writes at 64, 32, 16 and 8 bit widths.
 *
 * What does not: writes in bulk. Verified 4 Kbyte at a time, about one word
 * per 352 bytes reads back as zero across all 8 Mbyte, while the same
 * addresses pass when written and verified 1 Kbyte at a time.
 *
 * Why, and why it is not fixable here. On an AXI-attached OCTOSPI,
 * memory-mapped writes are issued in multiples of the 64-bit bus width. The
 * DQSE bit this driver has to set in OCTOSPI_WCCR — without which a write
 * returns an error response and strands the controller asserted and BUSY —
 * also stops AXI byte-enable masking from working, so lanes that should be
 * masked get written anyway. That is an acknowledged STM32 limitation for
 * quad SDR PSRAM, and ST's own answer to the identical symptom on an H7B0
 * with an APS6404L is to use an octal (OPI) part instead. Ruled out by
 * experiment first: sampling (identical at 68.75 MHz and 19.6 MHz), tCEM
 * enforcement and burst chopping (unchanged with the refresh counter off and
 * the chip-select boundary at 32 bytes rather than 1 Kbyte), and the D-cache
 * (unchanged with the region non-cacheable).
 *
 * Note that fitting the APS6404L the board actually specifies would not have
 * helped — same device class, and the ST report describing this failure is
 * an APS6404L. The fitted VTI7064MSME is not at fault.
 *
 * So: this driver stays, because it proves the hardware out at boot and
 * because V2 will want the DT and MPU groundwork, but wasp_psram_ready()
 * returns false unless CONFIG_WASP_PSRAM_ALLOW is set, and nothing
 * allocates here. The WAMR pool in particular must stay in SRAM: WASM
 * linear memory is a plain uint8 * that the interpreter stores through
 * directly, so there is no indirect-write path to route it through.
 * docs/hardware.md "PSRAM part selection" carries the V2 decision.
 *
 * The one hard timing constraint is tCEM, the maximum time chip select
 * may stay low: 4 us on this part, half the APS6404L's 8 us. Section 4.6
 * spells out the consequence of overrunning it — the activated wordline
 * is never terminated, internal refresh stays blocked, and the array
 * loses data with no error anywhere. OCTOSPI enforces it in hardware via
 * the DCR4 refresh counter, which drops chip select every Refresh+1
 * clocks and resumes the burst with a fresh command. That counter is
 * computed below from the clock the driver actually achieved.
 */
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/cache.h>
#include <zephyr/sys/util.h>

#include <stm32_ll_rcc.h>

#include "psram.h"

LOG_MODULE_REGISTER(wasp_psram, LOG_LEVEL_INF);

#define PSRAM_NODE DT_NODELABEL(psram)
#define OSPI_NODE  DT_PARENT(PSRAM_NODE)

#if DT_NODE_HAS_STATUS(PSRAM_NODE, okay)

#define CMD_READ_QUAD  0xEBU
#define CMD_WRITE_QUAD 0x38U
#define CMD_ENTER_QPI  0x35U
#define CMD_RESET_EN   0x66U
#define CMD_RESET      0x99U
#define CMD_READ_ID    0x9FU

/* 0xEB needs 6 wait cycles; the quad write needs none. */
#define DUMMY_CYCLES_READ  6U
#define DUMMY_CYCLES_WRITE 0U

#define PSRAM_SIZE_BYTES (DT_PROP(PSRAM_NODE, size) / 8U)
#define PSRAM_BASE_ADDR  DT_REG_ADDR_BY_IDX(OSPI_NODE, 1)
#define PSRAM_TCEM_NS    DT_PROP(PSRAM_NODE, wasp_tcem_ns)
#define PSRAM_CSBOUND    DT_PROP(PSRAM_NODE, st_csbound)
#define PSRAM_MAX_FREQ   DT_PROP(PSRAM_NODE, max_frequency)

PINCTRL_DT_DEFINE(OSPI_NODE);

static OSPI_HandleTypeDef hospi;
static bool psram_ok;
static uint32_t psram_clk_hz;

/* Issue a command with no address and no data, on `lines` IO lines. */
static int ospi_cmd(uint32_t instruction, uint32_t lines)
{
	OSPI_RegularCmdTypeDef cmd = { 0 };

	cmd.OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
	cmd.FlashId = HAL_OSPI_FLASH_ID_1;
	cmd.Instruction = instruction;
	cmd.InstructionMode = lines;
	cmd.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
	cmd.InstructionDtrMode = HAL_OSPI_INSTRUCTION_DTR_DISABLE;
	cmd.AddressMode = HAL_OSPI_ADDRESS_NONE;
	cmd.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
	cmd.DataMode = HAL_OSPI_DATA_NONE;
	cmd.DummyCycles = 0;
	cmd.DQSMode = HAL_OSPI_DQS_DISABLE;
	cmd.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;

	if (HAL_OSPI_Command(&hospi, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
		return -EIO;
	}
	return 0;
}

#define ospi_cmd_spi(i)  ospi_cmd((i), HAL_OSPI_INSTRUCTION_1_LINE)
#define ospi_cmd_quad(i) ospi_cmd((i), HAL_OSPI_INSTRUCTION_4_LINES)

/*
 * Put the device into SPI mode from either possible starting state.
 *
 * This matters more than it looks. The PSRAM has no reset pin, so a warm
 * MCU reset leaves it exactly as the previous run left it — which, after
 * any successful boot, is QPI mode. In QPI it expects commands on four
 * lines, so a 1-line 0x66/0x99 is noise to it and the subsequent 1-line
 * read ID comes back as zeros. (Observed: the first ever boot worked
 * because the part was cold and therefore in SPI mode; the next boot
 * reported "PSRAM not responding".)
 *
 * So reset twice, once each way. 0x99 restores the power-on default,
 * which section 4.4 gives as SPI mode, and both halves are harmless in
 * the state they are not meant for: a quad command sent to a device in
 * SPI mode puts two clocks of a partial opcode on IO0 and is discarded
 * when chip select rises, and 0x66/0x99 in SPI mode is a plain reset.
 */
static int psram_force_spi_mode(void)
{
	/* If it is in QPI, this is the reset that lands. */
	(void)ospi_cmd_quad(CMD_RESET_EN);
	(void)ospi_cmd_quad(CMD_RESET);
	k_busy_wait(100);

	/* If it was in SPI all along, this is. */
	if (ospi_cmd_spi(CMD_RESET_EN) < 0 || ospi_cmd_spi(CMD_RESET) < 0) {
		return -EIO;
	}
	k_busy_wait(100);
	return 0;
}

/*
 * 0x9F: serial command, 24-bit address, no wait cycles, serial data.
 * Only valid in SPI mode, so this doubles as the proof that the bus is
 * wired correctly before anything irreversible happens.
 */
static int psram_read_id(uint8_t *id, size_t len)
{
	OSPI_RegularCmdTypeDef cmd = { 0 };

	cmd.OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
	cmd.FlashId = HAL_OSPI_FLASH_ID_1;
	cmd.Instruction = CMD_READ_ID;
	cmd.InstructionMode = HAL_OSPI_INSTRUCTION_1_LINE;
	cmd.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
	cmd.InstructionDtrMode = HAL_OSPI_INSTRUCTION_DTR_DISABLE;
	cmd.Address = 0;
	cmd.AddressMode = HAL_OSPI_ADDRESS_1_LINE;
	cmd.AddressSize = HAL_OSPI_ADDRESS_24_BITS;
	cmd.AddressDtrMode = HAL_OSPI_ADDRESS_DTR_DISABLE;
	cmd.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
	cmd.DataMode = HAL_OSPI_DATA_1_LINE;
	cmd.DataDtrMode = HAL_OSPI_DATA_DTR_DISABLE;
	cmd.NbData = len;
	cmd.DummyCycles = 0;
	cmd.DQSMode = HAL_OSPI_DQS_DISABLE;
	cmd.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;

	if (HAL_OSPI_Command(&hospi, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
		return -EIO;
	}
	if (HAL_OSPI_Receive(&hospi, id, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
		return -EIO;
	}
	return 0;
}

/*
 * A QPI transfer in indirect mode: the controller drives one command and
 * moves `len` bytes itself, with no memory-mapped window involved.
 *
 * This exists to split one question into two. If the memory-mapped window
 * faults, the cause is either the device not understanding the commands
 * or the controller's memory-mapped setup, and from an imprecise bus
 * error those look identical. An indirect round trip answers the first
 * half on its own.
 */
static int psram_xfer_qpi(uint32_t instruction, uint32_t addr, uint8_t *buf,
			  size_t len, uint32_t dummy, bool write)
{
	OSPI_RegularCmdTypeDef cmd = { 0 };

	cmd.OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
	cmd.FlashId = HAL_OSPI_FLASH_ID_1;
	cmd.Instruction = instruction;
	cmd.InstructionMode = HAL_OSPI_INSTRUCTION_4_LINES;
	cmd.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
	cmd.InstructionDtrMode = HAL_OSPI_INSTRUCTION_DTR_DISABLE;
	cmd.Address = addr;
	cmd.AddressMode = HAL_OSPI_ADDRESS_4_LINES;
	cmd.AddressSize = HAL_OSPI_ADDRESS_24_BITS;
	cmd.AddressDtrMode = HAL_OSPI_ADDRESS_DTR_DISABLE;
	cmd.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
	cmd.DataMode = HAL_OSPI_DATA_4_LINES;
	cmd.DataDtrMode = HAL_OSPI_DATA_DTR_DISABLE;
	cmd.NbData = len;
	cmd.DummyCycles = dummy;
	cmd.DQSMode = HAL_OSPI_DQS_DISABLE;
	cmd.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;

	if (HAL_OSPI_Command(&hospi, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
		return -EIO;
	}
	if (write) {
		if (HAL_OSPI_Transmit(&hospi, buf, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
			return -EIO;
		}
	} else {
		if (HAL_OSPI_Receive(&hospi, buf, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
			return -EIO;
		}
	}
	return 0;
}

/* Round-trip 16 bytes through indirect QPI before trusting the window. */
static int psram_probe_qpi(void)
{
	uint8_t tx[16], rx[16];
	int ret;

	for (unsigned int i = 0; i < sizeof(tx); i++) {
		tx[i] = (uint8_t)(0xA0 ^ i);
	}
	memset(rx, 0, sizeof(rx));

	ret = psram_xfer_qpi(CMD_WRITE_QUAD, 0, tx, sizeof(tx),
			     DUMMY_CYCLES_WRITE, true);
	if (ret < 0) {
		LOG_ERR("indirect QPI write rejected by the controller");
		return ret;
	}
	ret = psram_xfer_qpi(CMD_READ_QUAD, 0, rx, sizeof(rx),
			     DUMMY_CYCLES_READ, false);
	if (ret < 0) {
		LOG_ERR("indirect QPI read rejected by the controller");
		return ret;
	}
	if (memcmp(tx, rx, sizeof(tx)) != 0) {
		LOG_ERR("indirect QPI round trip mismatch");
		LOG_HEXDUMP_ERR(tx, sizeof(tx), "wrote");
		LOG_HEXDUMP_ERR(rx, sizeof(rx), "read");
		return -EIO;
	}
	LOG_INF("indirect QPI round trip ok (0x%02x write, 0x%02x read, %u dummy)",
		CMD_WRITE_QUAD, CMD_READ_QUAD, DUMMY_CYCLES_READ);
	return 0;
}

/*
 * Hand the controller the two command templates and switch to
 * memory-mapped mode. Both are quad on every phase, because the device
 * is in QPI by this point and expects the instruction itself on 4 lines.
 */
static int psram_map(void)
{
	OSPI_RegularCmdTypeDef cmd = { 0 };
	OSPI_MemoryMappedTypeDef mm = { 0 };

	cmd.FlashId = HAL_OSPI_FLASH_ID_1;
	cmd.InstructionMode = HAL_OSPI_INSTRUCTION_4_LINES;
	cmd.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
	cmd.InstructionDtrMode = HAL_OSPI_INSTRUCTION_DTR_DISABLE;
	cmd.AddressMode = HAL_OSPI_ADDRESS_4_LINES;
	cmd.AddressSize = HAL_OSPI_ADDRESS_24_BITS;
	cmd.AddressDtrMode = HAL_OSPI_ADDRESS_DTR_DISABLE;
	cmd.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
	cmd.DataMode = HAL_OSPI_DATA_4_LINES;
	cmd.DataDtrMode = HAL_OSPI_DATA_DTR_DISABLE;
	cmd.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;

	/*
	 * DQS has to be enabled for the write configuration even though this
	 * package has no DQS pin and pinctrl assigns none.
	 *
	 * This is an STM32 OCTOSPI erratum, not a choice: with DQSE clear in
	 * OCTOSPI_WCCR, a memory-mapped write returns an error response, and
	 * the controller is left asserted and BUSY. ST's own errata for the
	 * H7Rx/7Sx describes the same "memory-mapped write error response
	 * when DQS output is disabled" and the same workaround, which is to
	 * set DQSE regardless of the GPIO configuration. Nothing is driven
	 * externally, since no pin is mapped to the function.
	 *
	 * Reads are unaffected and leave DQS off. The symptom this fixes is
	 * specific and otherwise baffling: memory-mapped reads return correct
	 * data while the first memory-mapped write takes an imprecise bus
	 * error with OCTOSPI_SR showing BUSY set and TEF clear.
	 */
	cmd.OperationType = HAL_OSPI_OPTYPE_WRITE_CFG;
	cmd.Instruction = CMD_WRITE_QUAD;
	cmd.DummyCycles = DUMMY_CYCLES_WRITE;
	cmd.DQSMode = HAL_OSPI_DQS_ENABLE;
	if (HAL_OSPI_Command(&hospi, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
		LOG_ERR("write template rejected");
		return -EIO;
	}

	cmd.OperationType = HAL_OSPI_OPTYPE_READ_CFG;
	cmd.Instruction = CMD_READ_QUAD;
	cmd.DummyCycles = DUMMY_CYCLES_READ;
	cmd.DQSMode = HAL_OSPI_DQS_DISABLE;
	if (HAL_OSPI_Command(&hospi, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
		LOG_ERR("read template rejected");
		return -EIO;
	}

	/*
	 * The timeout counter is not optional here, and not a power
	 * optimisation — it is what ends a write.
	 *
	 * In memory-mapped mode the controller leaves chip select asserted
	 * after a write access, because the next access may continue the
	 * same burst. Something has to decide the burst is over: a chip
	 * select boundary, the refresh counter, or this. With it disabled
	 * and the boundary 1 Kbyte away, a single word write leaves the
	 * controller asserted and BUSY forever, and the next access to the
	 * window takes an imprecise bus error. Observed exactly that:
	 * OCTOSPI_SR = 0x20, so BUSY set with TEF clear — the controller was
	 * not rejecting the write, it was still waiting for more of it.
	 *
	 * The period is in kernel clock cycles and is part of the tCEM
	 * budget, since chip select stays low while it counts down. A
	 * quarter of the refresh budget leaves ample room for the command,
	 * address and data phases ahead of it.
	 */
	mm.TimeOutActivation = HAL_OSPI_TIMEOUT_COUNTER_ENABLE;
	mm.TimeOutPeriod = MAX(hospi.Init.Refresh / 4U, 1U);
	if (HAL_OSPI_MemoryMapped(&hospi, &mm) != HAL_OK) {
		LOG_ERR("memory-mapped mode rejected");
		return -EIO;
	}
	return 0;
}

static int psram_init(void)
{
	const struct pinctrl_dev_config *pcfg = PINCTRL_DT_DEV_CONFIG_GET(OSPI_NODE);
	struct stm32_pclken pclk = STM32_CLOCK_INFO_BY_NAME(OSPI_NODE, ospix);
	struct stm32_pclken pclk_ker = STM32_CLOCK_INFO_BY_NAME(OSPI_NODE, ospi_ker);
	const struct device *clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
	OSPIM_CfgTypeDef ospim = { 0 };
	uint32_t ker_hz, prescaler, refresh;
	uint8_t id[8] = { 0 };
	int ret;

	ret = pinctrl_apply_state(pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("pinctrl failed (%d)", ret);
		return ret;
	}

	if (clock_control_on(clk, (clock_control_subsys_t)&pclk) != 0) {
		LOG_ERR("could not gate on OCTOSPI1");
		return -EIO;
	}
	if (clock_control_configure(clk, (clock_control_subsys_t)&pclk_ker, NULL) != 0) {
		LOG_ERR("could not select the OCTOSPI kernel clock");
		return -EIO;
	}
	if (clock_control_get_rate(clk, (clock_control_subsys_t)&pclk_ker, &ker_hz) != 0) {
		LOG_ERR("could not read the OCTOSPI kernel clock");
		return -EIO;
	}

	/* Slowest divider that still respects the device's ceiling. The
	 * OCTOSPI prescaler is any integer 1..256, not a power of two.
	 */
	for (prescaler = 1; prescaler <= 256U; prescaler++) {
		if ((ker_hz / prescaler) <= PSRAM_MAX_FREQ) {
			break;
		}
	}
	if (prescaler > 256U) {
		LOG_ERR("kernel clock %u Hz cannot be divided below %u Hz",
			ker_hz, (unsigned int)PSRAM_MAX_FREQ);
		return -EINVAL;
	}
	psram_clk_hz = ker_hz / prescaler;

	/*
	 * tCEM in clock cycles, less 10% so that clock tolerance and the
	 * command/address/dummy phases ahead of the data cannot push the
	 * real chip-select time past the limit. OCTOSPI releases chip
	 * select every Refresh+1 cycles.
	 */
	refresh = (uint32_t)(((uint64_t)PSRAM_TCEM_NS * psram_clk_hz) / 1000000000ULL);
	refresh = (refresh * 9U) / 10U;
	if (refresh == 0U) {
		LOG_ERR("tCEM %u ns is shorter than one clock at %u Hz",
			(unsigned int)PSRAM_TCEM_NS, psram_clk_hz);
		return -EINVAL;
	}

	hospi.Instance = (OCTOSPI_TypeDef *)DT_REG_ADDR(OSPI_NODE);
	hospi.Init.FifoThreshold = 4;
	hospi.Init.DualQuad = HAL_OSPI_DUALQUAD_DISABLE;
	/* Generic SDR quad part: not APMEMORY, which selects Xccela/HyperBus
	 * framing, and not MACRONIX, which enables that vendor's DTR quirks.
	 */
	hospi.Init.MemoryType = HAL_OSPI_MEMTYPE_MICRON;
	hospi.Init.DeviceSize = find_msb_set(PSRAM_SIZE_BYTES) - 1; /* 8 MB -> 23 */
	hospi.Init.ChipSelectHighTime = 2;	/* tCPH min 1 tCLK */
	hospi.Init.FreeRunningClock = HAL_OSPI_FREERUNCLK_DISABLE;
	hospi.Init.ClockMode = HAL_OSPI_CLOCK_MODE_0;
	hospi.Init.WrapSize = HAL_OSPI_WRAP_NOT_SUPPORTED;
	hospi.Init.ClockPrescaler = prescaler;
	hospi.Init.SampleShifting = HAL_OSPI_SAMPLE_SHIFTING_NONE;
	hospi.Init.DelayHoldQuarterCycle = HAL_OSPI_DHQC_DISABLE;
	hospi.Init.ChipSelectBoundary = PSRAM_CSBOUND;
	hospi.Init.DelayBlockBypass = HAL_OSPI_DELAY_BLOCK_BYPASSED;
	hospi.Init.MaxTran = 0;			/* inter-OCTOSPI arbitration only */
	hospi.Init.Refresh = refresh;

	if (HAL_OSPI_Init(&hospi) != HAL_OK) {
		LOG_ERR("HAL_OSPI_Init failed");
		return -EIO;
	}

	/* Port 1, low IO bank: CLK PF10, NCS PG6, IO0-3 PF8/PF9/PF7/PF6. */
	ospim.ClkPort = 1;
	ospim.NCSPort = 1;
	ospim.IOLowPort = HAL_OSPIM_IOPORT_1_LOW;
	ospim.DQSPort = 0;
	ospim.IOHighPort = HAL_OSPIM_IOPORT_NONE;
	if (HAL_OSPIM_Config(&hospi, &ospim, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
		LOG_ERR("HAL_OSPIM_Config failed");
		return -EIO;
	}

	if (psram_force_spi_mode() < 0) {
		LOG_ERR("reset not accepted — check wiring and the OSPI clock");
		return -EIO;
	}

	if (psram_read_id(id, sizeof(id)) < 0) {
		LOG_ERR("read ID failed");
		return -EIO;
	}

	/*
	 * The data sheet does not publish an expected ID value, so this is
	 * logged rather than checked. All-zero or all-ones still means the
	 * bus is dead, and that is worth failing on.
	 */
	if ((id[0] == 0x00 && id[1] == 0x00) || (id[0] == 0xFF && id[1] == 0xFF)) {
		LOG_ERR("ID reads %02x %02x — PSRAM not responding", id[0], id[1]);
		return -EIO;
	}
	LOG_INF("PSRAM ID %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);

	if (ospi_cmd_spi(CMD_ENTER_QPI) < 0) {
		LOG_ERR("enter QPI failed");
		return -EIO;
	}
	k_busy_wait(10);

	if (psram_probe_qpi() < 0) {
		return -EIO;
	}

	if (psram_map() < 0) {
		return -EIO;
	}

	/*
	 * Step through the window one capability at a time, announcing each
	 * before attempting it. A failure here is an imprecise bus error
	 * that halts the system, so the last line printed is the diagnosis.
	 * The indirect probe above left a known pattern at offset 0.
	 */
	{
		volatile uint32_t *w = (volatile uint32_t *)PSRAM_BASE_ADDR;
		uint32_t expect0 = 0xA3A2A1A0U;	/* 0xA0 ^ 0..3, little endian */
		uint32_t got;

		LOG_INF("mapped-read check...");
		sys_cache_data_invd_range((void *)PSRAM_BASE_ADDR, 32);
		got = w[0];
		if (got != expect0) {
			LOG_ERR("mapped read 0x%08x, indirect wrote 0x%08x", got, expect0);
			return -EIO;
		}
		LOG_INF("mapped read ok (0x%08x)", got);

		LOG_INF("mapped-write check...");
		w[1] = 0xDEADBEEFU;
		sys_cache_data_flush_range((void *)PSRAM_BASE_ADDR, 32);
		sys_cache_data_invd_range((void *)PSRAM_BASE_ADDR, 32);
		got = w[1];
		if (got != 0xDEADBEEFU) {
			LOG_ERR("mapped write read back 0x%08x", got);
			return -EIO;
		}
		LOG_INF("mapped write ok");
	}

	/*
	 * Access-width probe. The sweep below fails at a fixed, clock
	 * independent pattern, which rules out marginal timing and points at
	 * how accesses reach the device. An H723 report describes 64-bit
	 * memory-mapped accesses working while narrower ones take on padding
	 * zeros, so measure each width over the same kilobyte and let the
	 * numbers say which widths are trustworthy.
	 */
	LOG_INF("D-cache %s, I-cache %s",
		(SCB->CCR & SCB_CCR_DC_Msk) ? "on" : "off",
		(SCB->CCR & SCB_CCR_IC_Msk) ? "on" : "off");
	{
		const size_t n = 256;	/* 1 KB */
		volatile uint64_t *q = (volatile uint64_t *)PSRAM_BASE_ADDR;
		volatile uint32_t *w = (volatile uint32_t *)PSRAM_BASE_ADDR;
		volatile uint16_t *h = (volatile uint16_t *)PSRAM_BASE_ADDR;
		volatile uint8_t *b = (volatile uint8_t *)PSRAM_BASE_ADDR;
		uint32_t e8 = 0, e16 = 0, e32 = 0, e64 = 0;

		for (size_t i = 0; i < n / 2; i++) {
			q[i] = 0x1111111100000000ULL + i;
		}
		sys_cache_data_flush_and_invd_range((void *)PSRAM_BASE_ADDR, n * 4);
		for (size_t i = 0; i < n / 2; i++) {
			if (q[i] != 0x1111111100000000ULL + i) {
				e64++;
			}
		}

		for (size_t i = 0; i < n; i++) {
			w[i] = 0x22220000U + (uint32_t)i;
		}
		sys_cache_data_flush_and_invd_range((void *)PSRAM_BASE_ADDR, n * 4);
		for (size_t i = 0; i < n; i++) {
			if (w[i] != 0x22220000U + (uint32_t)i) {
				e32++;
			}
		}

		for (size_t i = 0; i < n * 2; i++) {
			h[i] = (uint16_t)(0x3300U + (i & 0xFF));
		}
		sys_cache_data_flush_and_invd_range((void *)PSRAM_BASE_ADDR, n * 4);
		for (size_t i = 0; i < n * 2; i++) {
			if (h[i] != (uint16_t)(0x3300U + (i & 0xFF))) {
				e16++;
			}
		}

		for (size_t i = 0; i < n * 4; i++) {
			b[i] = (uint8_t)(i ^ 0x44);
		}
		sys_cache_data_flush_and_invd_range((void *)PSRAM_BASE_ADDR, n * 4);
		for (size_t i = 0; i < n * 4; i++) {
			if (b[i] != (uint8_t)(i ^ 0x44)) {
				e8++;
			}
		}

		LOG_INF("1 KB by width — 64-bit:%u 32-bit:%u 16-bit:%u 8-bit:%u errors",
			e64, e32, e16, e8);
	}

	psram_ok = true;
	if (!IS_ENABLED(CONFIG_WASP_PSRAM_ALLOW)) {
		LOG_INF("reads verified; bulk writes broken on quad SDR, "
			"not offered for allocation");
	}
	LOG_INF("%u MB mapped at 0x%08x, %u.%02u MHz (/%u), tCEM %u ns -> refresh %u clk",
		(unsigned int)(PSRAM_SIZE_BYTES >> 20), (unsigned int)PSRAM_BASE_ADDR,
		psram_clk_hz / 1000000U, (psram_clk_hz % 1000000U) / 10000U,
		prescaler, (unsigned int)PSRAM_TCEM_NS, refresh);
	return 0;
}

/*
 * Prove the array, not the cache.
 *
 * 0x90000000 falls in the ARMv7-M default map's normal, cacheable region
 * and the H7's D-cache is enabled, so a small write-then-read test would
 * be answered entirely out of 16 KB of cache and would pass against a
 * completely dead device. Every pass below therefore sweeps all 8 MB,
 * which cannot be held in cache, and the address-uniqueness pass is what
 * catches the failure a pattern test misses: a stuck or swapped address
 * line aliases two addresses together, and identical data at both makes
 * that invisible.
 */
#if IS_ENABLED(CONFIG_WASP_PSRAM_SELFTEST)
static int psram_selftest(void)
{
	volatile uint32_t *p = (volatile uint32_t *)PSRAM_BASE_ADDR;
	const size_t words = PSRAM_SIZE_BYTES / sizeof(uint32_t);
	uint32_t errors = 0;
	int64_t t0;

	if (!psram_ok) {
		return 0;
	}

	/*
	 * Pass 1, chunk at a time: write a chunk, push it out of the cache,
	 * then verify it before moving on. Each word holds its own index, so
	 * aliasing between two addresses shows as a mismatch rather than a
	 * match.
	 *
	 * The chunking is deliberate and is itself the measurement. Writing
	 * all 8 MB and only then reading it back fails in a particular way:
	 * scattered words read as zero, spaced 352 bytes apart, with the same
	 * spacing at 68.75 MHz and at 19.6 MHz but a starting offset that
	 * moves from run to run. Not timing, then, and not a fixed bad
	 * address either. The remaining suspect is the device's own refresh:
	 * section 4.6 warns that internal refresh is blocked while a
	 * read or write wordline is active, and an uninterrupted 8 MB stream
	 * gives the array almost no chance to refresh rows written a second
	 * earlier. Verifying each chunk while it is still fresh tells the two
	 * stories apart.
	 */
	const size_t chunk = 4096 / sizeof(uint32_t);
	size_t first_err = SIZE_MAX, last_err = 0;
	uint32_t write_ms = 0, read_ms = 0;

	for (size_t base = 0; base < words; base += chunk) {
		size_t n = MIN(chunk, words - base);

		/*
		 * Pre-fill with a marker that the index pattern can never
		 * produce. A bad word that reads back 0xFFFFFFFF means its
		 * write was dropped; one that reads 0x00000000 means the read
		 * returned zeros over good data.
		 */
		for (size_t i = 0; i < n; i++) {
			p[base + i] = 0xFFFFFFFFU;
		}
		sys_cache_data_flush_and_invd_range((void *)&p[base], n * sizeof(uint32_t));

		t0 = k_uptime_get();
		for (size_t i = 0; i < n; i++) {
			p[base + i] = (uint32_t)(base + i);
		}
		sys_cache_data_flush_and_invd_range((void *)&p[base], n * sizeof(uint32_t));
		write_ms += (uint32_t)(k_uptime_get() - t0);

		t0 = k_uptime_get();
		for (size_t i = 0; i < n; i++) {
			uint32_t got = p[base + i];

			if (got != (uint32_t)(base + i)) {
				if (errors < 4) {
					LOG_ERR("addr pass: [0x%06x] = 0x%08x, want 0x%08x",
						(unsigned int)((base + i) * 4), got,
						(unsigned int)(base + i));
				}
				if (first_err == SIZE_MAX) {
					first_err = base + i;
				}
				last_err = base + i;
				errors++;
			}
		}
		read_ms += (uint32_t)(k_uptime_get() - t0);
	}
	if (errors != 0) {
		LOG_ERR("chunked pass: %u errors, first word 0x%06x last 0x%06x",
			errors, (unsigned int)(first_err * 4), (unsigned int)(last_err * 4));
	}

	/* Pass 2: 0x55/0xAA over every bit of every word, to catch a stuck
	 * or shorted data line that pass 1's small indices never drive.
	 */
	for (size_t i = 0; i < words; i++) {
		p[i] = (i & 1) ? 0xAAAAAAAAU : 0x55555555U;
	}
	for (size_t i = 0; i < words; i++) {
		uint32_t want = (i & 1) ? 0xAAAAAAAAU : 0x55555555U;

		if (p[i] != want) {
			errors++;
		}
	}

	/* Pass 3: byte and halfword access. OCTOSPI has to turn these into
	 * sub-word bursts, and a wrong write template shows up here first.
	 */
	volatile uint8_t *b = (volatile uint8_t *)PSRAM_BASE_ADDR;
	volatile uint16_t *h = (volatile uint16_t *)PSRAM_BASE_ADDR;

	for (uint32_t i = 0; i < 256; i++) {
		b[i] = (uint8_t)(i ^ 0x5A);
	}
	for (uint32_t i = 0; i < 256; i++) {
		if (b[i] != (uint8_t)(i ^ 0x5A)) {
			errors++;
		}
	}
	for (uint32_t i = 0; i < 128; i++) {
		h[i] = (uint16_t)(i * 0x0101U + 1U);
	}
	for (uint32_t i = 0; i < 128; i++) {
		if (h[i] != (uint16_t)(i * 0x0101U + 1U)) {
			errors++;
		}
	}

	if (errors != 0) {
		LOG_ERR("PSRAM self-test FAILED: %u errors", errors);
		psram_ok = false;
		return -EIO;
	}

	LOG_INF("PSRAM self-test passed: %u MB write %u ms (%u MB/s), "
		"read %u ms (%u MB/s)",
		(unsigned int)(PSRAM_SIZE_BYTES >> 20), write_ms,
		write_ms ? (unsigned int)((PSRAM_SIZE_BYTES >> 20) * 1000U / write_ms) : 0U,
		read_ms,
		read_ms ? (unsigned int)((PSRAM_SIZE_BYTES >> 20) * 1000U / read_ms) : 0U);
	return 0;
}
/* After psram_init (priority 1) and after the console is up. */
SYS_INIT(psram_selftest, POST_KERNEL, 2);
#endif /* CONFIG_WASP_PSRAM_SELFTEST */

bool wasp_psram_ready(void)
{
	/* Mapped and short-checked is not the same as safe to allocate from.
	 * See the header comment: bulk writes are broken on v1 hardware.
	 */
	return psram_ok && IS_ENABLED(CONFIG_WASP_PSRAM_ALLOW);
}

void *wasp_psram_base(void)
{
	return psram_ok ? (void *)PSRAM_BASE_ADDR : NULL;
}

size_t wasp_psram_size(void)
{
	return psram_ok ? PSRAM_SIZE_BYTES : 0;
}

/*
 * POST_KERNEL rather than PRE_KERNEL_1, because the HAL's OSPI calls time
 * out against HAL_GetTick and k_busy_wait needs the system clock.
 *
 * Priority 1 puts this immediately after the payload heap (priority 0)
 * and before every K_THREAD_DEFINE'd worker. Anything that wants to
 * allocate out of PSRAM therefore has to initialise later than priority 1
 * — worth remembering when the WAMR pool eventually moves here.
 */
SYS_INIT(psram_init, POST_KERNEL, 1);

#else
bool wasp_psram_ready(void) { return false; }
void *wasp_psram_base(void) { return NULL; }
size_t wasp_psram_size(void) { return 0; }
#endif /* DT_NODE_HAS_STATUS(PSRAM_NODE, okay) */
