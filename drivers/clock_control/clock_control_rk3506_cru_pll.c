/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "clock_control_rk3506_cru_pll.h"

#define RK3506_CRU_PLL_CON(n)   (0x10000U + ((n) * 4U))
#define RK3506_CRU_MODE_CON     0x0280U
#define RK3506_CRU_PLL_LOCK     BIT(10)
#define RK3506_CRU_V0PLL_CON    8U

struct rk3506_cru_pll_setting {
	uint32_t rate;
	uint16_t fbdiv;
	uint8_t refdiv;
	uint8_t postdiv1;
	uint8_t postdiv2;
	bool dsmpd;
	uint32_t frac;
};

static const struct rk3506_cru_pll_setting rk3506_v0pll_settings[] = {
	{ 903168000U, 75U, 1U, 2U, 1U, false, 4429185U },
	{ 1179648000U, 49U, 1U, 1U, 1U, false, 2550137U },
};

static int rk3506_cru_wait_v0pll_lock(uintptr_t base)
{
	for (uint32_t i = 0U; i < 5000U; i++) {
		if ((sys_read32(base + RK3506_CRU_PLL_CON(RK3506_CRU_V0PLL_CON) + 4U) &
		     RK3506_CRU_PLL_LOCK) != 0U) {
			return 0;
		}
		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

static void rk3506_cru_set_v0pll_mode(uintptr_t base, uint32_t mode)
{
	uint32_t mode_mask = 0x3U << 2U;
	uint32_t value = sys_read32(base + RK3506_CRU_MODE_CON);

	value &= ~mode_mask;
	value |= mode << 2U;
	sys_write32(value | (mode_mask << 16U), base + RK3506_CRU_MODE_CON);
}

static int rk3506_cru_write_v0pll(uintptr_t base,
				  const struct rk3506_cru_pll_setting *setting)
{
	uint32_t con0;
	uint32_t con1;
	uint32_t con2;

	con0 = setting->fbdiv | ((uint32_t)setting->postdiv1 << 12U);
	con1 = setting->refdiv | ((uint32_t)setting->postdiv2 << 6U) |
	       ((uint32_t)setting->dsmpd << 12U);
	con2 = sys_read32(base + RK3506_CRU_PLL_CON(RK3506_CRU_V0PLL_CON) + 8U);
	con2 &= ~0xffffffU;
	con2 |= setting->frac & 0xffffffU;

	sys_write32((con0 & 0xffffU) | 0xffff0000U,
		    base + RK3506_CRU_PLL_CON(RK3506_CRU_V0PLL_CON));
	sys_write32((con1 & 0xffffU) | 0xffff0000U,
		    base + RK3506_CRU_PLL_CON(RK3506_CRU_V0PLL_CON) + 4U);
	sys_write32(con2, base + RK3506_CRU_PLL_CON(RK3506_CRU_V0PLL_CON) + 8U);

	return 0;
}

int rk3506_cru_set_v0pll_rate(uintptr_t base, uint32_t rate)
{
	const struct rk3506_cru_pll_setting *setting = NULL;
	uint32_t old_con0 = sys_read32(base + RK3506_CRU_PLL_CON(RK3506_CRU_V0PLL_CON));
	uint32_t old_con1 = sys_read32(base + RK3506_CRU_PLL_CON(RK3506_CRU_V0PLL_CON) + 4U);
	uint32_t old_con2 = sys_read32(base + RK3506_CRU_PLL_CON(RK3506_CRU_V0PLL_CON) + 8U);
	int ret;

	for (size_t i = 0U; i < ARRAY_SIZE(rk3506_v0pll_settings); i++) {
		if (rk3506_v0pll_settings[i].rate == rate) {
			setting = &rk3506_v0pll_settings[i];
			break;
		}
	}

	if (setting == NULL) {
		return -ENOTSUP;
	}

	rk3506_cru_set_v0pll_mode(base, 0U);
	k_busy_wait(10);
	(void)rk3506_cru_write_v0pll(base, setting);
	ret = rk3506_cru_wait_v0pll_lock(base);
	rk3506_cru_set_v0pll_mode(base, 1U);
	if (ret == 0) {
		ret = rk3506_cru_wait_v0pll_lock(base);
	}

	if (ret != 0) {
		(void)rk3506_cru_write_v0pll(base,
			&(const struct rk3506_cru_pll_setting) {
				.fbdiv = old_con0 & 0xfffU,
				.postdiv1 = (old_con0 >> 12U) & 0x7U,
				.refdiv = old_con1 & 0x3fU,
				.postdiv2 = (old_con1 >> 6U) & 0x7U,
				.dsmpd = (old_con1 & BIT(12)) != 0U,
				.frac = old_con2 & 0xffffffU,
			});
		rk3506_cru_set_v0pll_mode(base, 1U);
	}

	return ret;
}
