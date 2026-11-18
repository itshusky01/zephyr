/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/sys_io.h>

#define RESET_REASON_NODE DT_NODELABEL(reset_reason)
#define PMUGRF_ADDR DT_REG_ADDR(DT_PHANDLE(RESET_REASON_NODE, rockchip_pmugrf))
#define STATUS_OFFSET DT_PROP(RESET_REASON_NODE, rockchip_status_offset)
#define CLEAR_OFFSET DT_PROP(RESET_REASON_NODE, rockchip_clear_offset)
#define SCRATCH_OFFSET DT_PROP(RESET_REASON_NODE, rockchip_scratch_offset)
#define SCRATCH_MAGIC DT_PROP(RESET_REASON_NODE, rockchip_scratch_magic)
#define RK3506_PMU_WDT_RESET BIT(1)
#define RK3506_PMU_TSADC_RESET BIT(2)
#define RK3506_PMU_FIRST_RESET BIT(0)

static mm_reg_t rk3506_reset_status_addr(void)
{
	return PMUGRF_ADDR + STATUS_OFFSET;
}

int z_impl_hwinfo_get_reset_cause(uint32_t *cause)
{
	uint32_t status = sys_read32(rk3506_reset_status_addr());

	if (cause == NULL) {
		return -EINVAL;
	}

	*cause = 0U;
	if (sys_read32(PMUGRF_ADDR + SCRATCH_OFFSET) == SCRATCH_MAGIC) {
		*cause |= RESET_WATCHDOG;
	} else if ((status & RK3506_PMU_WDT_RESET) != 0U) {
		*cause |= RESET_WATCHDOG;
	} else if ((status & RK3506_PMU_TSADC_RESET) != 0U) {
		*cause |= RESET_TEMPERATURE;
	} else if ((status & RK3506_PMU_FIRST_RESET) != 0U) {
		*cause |= RESET_SOFTWARE;
	} else {
		*cause |= RESET_POR;
	}

	return 0;
}

int z_impl_hwinfo_clear_reset_cause(void)
{
	sys_write32(RK3506_PMU_WDT_RESET | RK3506_PMU_TSADC_RESET |
		    RK3506_PMU_FIRST_RESET,
		    PMUGRF_ADDR + CLEAR_OFFSET);
	sys_write32(0U, PMUGRF_ADDR + SCRATCH_OFFSET);
	return 0;
}

int z_impl_hwinfo_get_supported_reset_cause(uint32_t *supported)
{
	if (supported == NULL) {
		return -EINVAL;
	}

	*supported = RESET_WATCHDOG | RESET_TEMPERATURE | RESET_SOFTWARE | RESET_POR;
	return 0;
}
