/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/arch/arm64/arm-smccc.h>
#include <zephyr/arch/arm/cortex_a_r/sys_io.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "mcu_rk3506.h"

#define RK_SIP_MCU_CFG 0x82000028U

#define RK3506_MCU_CFG_CODE_START_ADDR 0x01U

#define RK3506_CRU_BASE 0xff9a0000U
#define RK3506_CRU_GATE_CON5 0x0814U
#define RK3506_CRU_GATE_CON8 0x0820U

#define RK3506_GRF_BASE 0xff288000U
#define RK3506_GRF_SOC_CON36 0x0090U
#define RK3506_GRF_SOC_STATUS2 0x0108U

#define RK3506_PMU_BASE 0xff900000U
#define RK3506_PMU_INT_MASK_CON 0x000cU

#define RK3506_CRU_GATE_HCLK_M0 BIT(10)
#define RK3506_CRU_GATE_SWCLKTK_M0 BIT(11)
#define RK3506_CRU_GATE_STCLK_M0 BIT(2)

#define RK3506_CRU_GATE_CON5_M0_MASK \
	((RK3506_CRU_GATE_HCLK_M0 | RK3506_CRU_GATE_SWCLKTK_M0) << 16)
#define RK3506_CRU_GATE_CON5_M0_ENABLE RK3506_CRU_GATE_CON5_M0_MASK
#define RK3506_CRU_GATE_CON5_M0_DISABLE \
	(RK3506_CRU_GATE_CON5_M0_MASK | RK3506_CRU_GATE_HCLK_M0 | \
	 RK3506_CRU_GATE_SWCLKTK_M0)

#define RK3506_CRU_GATE_CON8_M0_MASK (RK3506_CRU_GATE_STCLK_M0 << 16)
#define RK3506_CRU_GATE_CON8_M0_ENABLE RK3506_CRU_GATE_CON8_M0_MASK
#define RK3506_CRU_GATE_CON8_M0_DISABLE \
	(RK3506_CRU_GATE_CON8_M0_MASK | RK3506_CRU_GATE_STCLK_M0)

#define RK3506_PMU_MCU_RST_DIS_CFG BIT(2)
#define RK3506_PMU_MCU_RST_DIS_CFG_MASK (RK3506_PMU_MCU_RST_DIS_CFG << 16)
#define RK3506_PMU_MCU_RST_ASSERT RK3506_PMU_MCU_RST_DIS_CFG_MASK
#define RK3506_PMU_MCU_GLOBAL_IRQ_MASK \
	(BIT(1) | RK3506_PMU_MCU_RST_DIS_CFG)
#define RK3506_PMU_MCU_GLOBAL_IRQ_MASK_WRITE \
	(RK3506_PMU_MCU_GLOBAL_IRQ_MASK << 16 | RK3506_PMU_MCU_RST_DIS_CFG)

#define RK3506_M0_STALIB 0x0bcd3d80U

#define RK3506_GRF_MCU_HALT BIT(1)
#define RK3506_GRF_MCU_TXEV BIT(2)
#define RK3506_GRF_MCU_LOCKUP BIT(3)
#define RK3506_GRF_MCU_HCLK_GATE BIT(4)
#define RK3506_GRF_MCU_WAKEUP BIT(5)
#define RK3506_GRF_MCU_SLEEPING BIT(6)

BUILD_ASSERT(sizeof(struct rk3506_m0_image_header) == RK3506_M0_IMAGE_HEADER_SIZE);

static bool a7_direct_load_blocked;

static int rk3506_mcu_config(uint32_t mcu_id, uint32_t func, uint32_t arg)
{
	struct arm_smccc_res res;

	arm_smccc_smc(RK_SIP_MCU_CFG, mcu_id, func, arg, 0, 0, 0, 0, &res);
	return res.a0 == 0UL ? 0 : (int)res.a0;
}

static bool rk3506_m0_clocks_enabled(void)
{
	uint32_t gate5 = sys_read32(RK3506_CRU_BASE + RK3506_CRU_GATE_CON5);
	uint32_t gate8 = sys_read32(RK3506_CRU_BASE + RK3506_CRU_GATE_CON8);

	return (gate5 & (RK3506_CRU_GATE_HCLK_M0 | RK3506_CRU_GATE_SWCLKTK_M0)) == 0U &&
	       (gate8 & RK3506_CRU_GATE_STCLK_M0) == 0U;
}

static void rk3506_m0_hold_reset(void)
{
	sys_write32(RK3506_PMU_MCU_RST_ASSERT,
		    RK3506_PMU_BASE + RK3506_PMU_INT_MASK_CON);
	sys_write32(RK3506_CRU_GATE_CON5_M0_DISABLE,
		    RK3506_CRU_BASE + RK3506_CRU_GATE_CON5);
	sys_write32(RK3506_CRU_GATE_CON8_M0_DISABLE,
		    RK3506_CRU_BASE + RK3506_CRU_GATE_CON8);
	barrier_dsync_fence_full();
}

static int rk3506_m0_release(uintptr_t entry)
{
	int ret;

	ret = rk3506_mcu_config(RK3506_M0_MCU_ID,
				RK3506_MCU_CFG_CODE_START_ADDR, (uint32_t)entry);
	if (ret != 0) {
		return ret;
	}

	sys_write32(RK3506_CRU_GATE_CON5_M0_ENABLE,
		    RK3506_CRU_BASE + RK3506_CRU_GATE_CON5);
	sys_write32(RK3506_CRU_GATE_CON8_M0_ENABLE,
		    RK3506_CRU_BASE + RK3506_CRU_GATE_CON8);
	sys_write32(RK3506_M0_STALIB, RK3506_GRF_BASE + RK3506_GRF_SOC_CON36);
	sys_write32(RK3506_PMU_MCU_GLOBAL_IRQ_MASK_WRITE,
		    RK3506_PMU_BASE + RK3506_PMU_INT_MASK_CON);
	barrier_dsync_fence_full();

	return 0;
}

int rk3506_m0_start(uintptr_t entry)
{
	int ret;

	if (a7_direct_load_blocked) {
		return -EALREADY;
	}
	if (entry < RK3506_M0_IMAGE_BASE ||
	    entry - RK3506_M0_IMAGE_BASE >= RK3506_M0_IMAGE_MAX_SIZE) {
		return -EINVAL;
	}

	/*
	 * M0 is a SIP-controlled MCU, not an AMP peer CPU. Rockchip firmware
	 * expects SIP_MCU_CFG(BUSMCU0, CODE_START_ADDR, entry), followed by
	 * clock and reset release. SIP 0x82000022/CPU_ON only applies to A-core
	 * AMP endpoints and returns -EIO for the M0 endpoint.
	 */
	ret = rk3506_m0_release(entry);
	if (ret != 0) {
		return ret;
	}

	a7_direct_load_blocked = true;

	return 0;
}

int rk3506_m0_boot_image(uintptr_t load_addr, const void *image, size_t size,
			 uintptr_t entry)
{
	const uint8_t *src = image;
	size_t offset = 0U;

	if (image == NULL || size == 0U ||
	    load_addr < RK3506_M0_IMAGE_BASE ||
	    size > RK3506_M0_IMAGE_MAX_SIZE ||
	    load_addr - RK3506_M0_IMAGE_BASE >
		    RK3506_M0_IMAGE_MAX_SIZE - size) {
		return -EINVAL;
	}
	if (a7_direct_load_blocked) {
		return -EALREADY;
	}

	rk3506_m0_hold_reset();

	/*
	 * The M0 SRAM is mapped as device memory on the A7 side. Use explicit
	 * sys_io word writes instead of memcpy(), which may issue normal-memory
	 * accesses that do not reach the device mapping reliably.
	 */
	for (; offset + sizeof(uint32_t) <= size; offset += sizeof(uint32_t)) {
		uint32_t word;

		memcpy(&word, &src[offset], sizeof(word));
		sys_write32(word, load_addr + offset);
		if (sys_read32(load_addr + offset) != word) {
			rk3506_m0_hold_reset();
			return -EIO;
		}
	}
	for (; offset < size; offset++) {
		sys_write8(src[offset], load_addr + offset);
		if (sys_read8(load_addr + offset) != src[offset]) {
			rk3506_m0_hold_reset();
			return -EIO;
		}
	}
	barrier_dsync_fence_full();

	return rk3506_m0_start(entry);
}

int rk3506_m0_boot_container(const void *container, size_t size)
{
	const struct rk3506_m0_image_header *header = container;
	const uint8_t *payload;
	struct rk3506_m0_image_header copy;

	if (container == NULL || size < sizeof(copy)) {
		return -EINVAL;
	}

	memcpy(&copy, header, sizeof(copy));
	if (copy.magic != RK3506_M0_IMAGE_MAGIC ||
	    copy.version != RK3506_M0_IMAGE_VERSION ||
	    copy.header_size != sizeof(copy) || copy.image_size == 0U ||
	    copy.header_size > size ||
	    size - copy.header_size != copy.image_size) {
		return -EINVAL;
	}
	if (copy.load_addr < RK3506_M0_IMAGE_BASE ||
	    copy.image_size > RK3506_M0_IMAGE_MAX_SIZE ||
	    copy.load_addr - RK3506_M0_IMAGE_BASE >
		    RK3506_M0_IMAGE_MAX_SIZE - copy.image_size ||
	    copy.entry_addr < copy.load_addr ||
	    copy.entry_addr - copy.load_addr >= copy.image_size) {
		return -EINVAL;
	}

	payload = (const uint8_t *)container + copy.header_size;
	if (crc32_ieee(payload, copy.image_size) != copy.image_crc32) {
		return -EIO;
	}

	return rk3506_m0_boot_image(copy.load_addr, payload, copy.image_size,
				    copy.entry_addr);
}

int rk3506_m0_stop(void)
{
	rk3506_m0_hold_reset();
	return 0;
}

int rk3506_m0_restart(uintptr_t entry)
{
	int ret;

	if (!a7_direct_load_blocked) {
		return -EINVAL;
	}
	if (entry < RK3506_M0_IMAGE_BASE ||
	    entry - RK3506_M0_IMAGE_BASE >= RK3506_M0_IMAGE_MAX_SIZE) {
		return -EINVAL;
	}

	rk3506_m0_hold_reset();
	ret = rk3506_m0_release(entry);
	if (ret != 0) {
		rk3506_m0_hold_reset();
	}

	return ret;
}

int rk3506_m0_probe(void)
{
	int ret;

	rk3506_m0_hold_reset();
	ret = rk3506_mcu_config(RK3506_M0_MCU_ID,
				RK3506_MCU_CFG_CODE_START_ADDR,
				RK3506_M0_IMAGE_BASE);
	rk3506_m0_hold_reset();
	return ret;
}

int rk3506_m0_get_status(struct rk3506_m0_status *status)
{
	uint32_t gate5;
	uint32_t gate8;
	uint32_t pmu;

	if (status == NULL) {
		return -EINVAL;
	}

	gate5 = sys_read32(RK3506_CRU_BASE + RK3506_CRU_GATE_CON5);
	gate8 = sys_read32(RK3506_CRU_BASE + RK3506_CRU_GATE_CON8);
	pmu = sys_read32(RK3506_PMU_BASE + RK3506_PMU_INT_MASK_CON);
	status->raw_status = sys_read32(RK3506_GRF_BASE + RK3506_GRF_SOC_STATUS2);
	status->hclk_enabled = (gate5 & RK3506_CRU_GATE_HCLK_M0) == 0U;
	status->swclktck_enabled = (gate5 & RK3506_CRU_GATE_SWCLKTK_M0) == 0U;
	status->stclk_enabled = (gate8 & RK3506_CRU_GATE_STCLK_M0) == 0U;
	status->reset_released =
		(pmu & RK3506_PMU_MCU_RST_DIS_CFG) != 0U;
	status->sleeping = (status->raw_status & RK3506_GRF_MCU_SLEEPING) != 0U;
	status->lockup = (status->raw_status & RK3506_GRF_MCU_LOCKUP) != 0U;
	status->halted = (status->raw_status & RK3506_GRF_MCU_HALT) != 0U;
	status->wakeup = (status->raw_status & RK3506_GRF_MCU_WAKEUP) != 0U;
	status->txev = (status->raw_status & RK3506_GRF_MCU_TXEV) != 0U;
	status->hclk_gate_ok =
		(status->raw_status & RK3506_GRF_MCU_HCLK_GATE) != 0U;
	status->a7_direct_load_blocked = a7_direct_load_blocked;
	status->running = status->reset_released && rk3506_m0_clocks_enabled() &&
			  !status->lockup && !status->halted;

	return 0;
}
