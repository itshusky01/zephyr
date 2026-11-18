/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk3506_wdt

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/math/ilog2.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define RK3506_WDT_CR          0x00U
#define RK3506_WDT_TORR        0x04U
#define RK3506_WDT_CRR         0x0cU
#define RK3506_WDT_CR_EN       BIT(0)
#define RK3506_WDT_CR_RPL_MASK GENMASK(4, 2)
#define RK3506_WDT_CRR_RESTART 0x76U
#define RK3506_WDT_MIN_PERIOD  0xffffULL
#define RK3506_CRU_GLB_RST_CON 0x0c10U
#define RK3506_GLB_WDT0_GLOBAL_EN BIT(6)
#define RK3506_GLB_WDT1_GLOBAL_EN BIT(7)
#define RK3506_GLB_WDT0_FIRST_RESET BIT(11)
#define RK3506_GLB_WDT1_FIRST_RESET BIT(12)
#define RK3506_GLB_PMU_WDT_EN BIT(4)
#define RK3506_GLB_PMU_GLOBAL_EN BIT(3)
#define RK3506_GLB_PMU_FIRST_RESET BIT(2)

struct rk3506_wdt_config {
	DEVICE_MMIO_ROM;
	const struct device *clock_dev;
	clock_control_subsys_t tclk;
	clock_control_subsys_t pclk;
	struct reset_dt_spec pclk_reset;
	struct reset_dt_spec tclk_reset;
	uintptr_t cru_base;
	uint8_t reset_pulse_length;
	uint8_t wdt_index;
};

struct rk3506_wdt_data {
	DEVICE_MMIO_RAM;
	uint32_t clock_hz;
	uint32_t timeout_top;
	bool timeout_installed;
};

static mm_reg_t rk3506_wdt_reg(const struct device *dev, uint32_t offset)
{
	return DEVICE_MMIO_GET(dev) + offset;
}

static int rk3506_wdt_setup(const struct device *dev, uint8_t options)
{
	const struct rk3506_wdt_config *cfg = dev->config;
	struct rk3506_wdt_data *data = dev->data;
	uint32_t rpl;

	if ((options & (WDT_OPT_PAUSE_HALTED_BY_DBG | WDT_OPT_PAUSE_IN_SLEEP)) != 0U) {
		return -ENOTSUP;
	}
	if (!data->timeout_installed) {
		return -ENOTSUP;
	}

	rpl = ilog2(cfg->reset_pulse_length) - 1U;
	sys_write32(data->timeout_top, rk3506_wdt_reg(dev, RK3506_WDT_TORR));
	sys_write32((rpl << 2U) | RK3506_WDT_CR_EN,
		    rk3506_wdt_reg(dev, RK3506_WDT_CR));
	barrier_dsync_fence_full();
	sys_write32(RK3506_WDT_CRR_RESTART, rk3506_wdt_reg(dev, RK3506_WDT_CRR));
	return 0;
}

static int rk3506_wdt_install_timeout(const struct device *dev,
				      const struct wdt_timeout_cfg *cfg)
{
	struct rk3506_wdt_data *data = dev->data;
	uint64_t ticks;
	uint32_t top;

	if (cfg == NULL || cfg->callback != NULL || cfg->window.min != 0U ||
	    cfg->window.max == 0U) {
		return -ENOTSUP;
	}

	ticks = (uint64_t)data->clock_hz * cfg->window.max;
	ticks = DIV_ROUND_UP(ticks, 1000U);
	if (ticks > (1ULL << 31) - 1ULL) {
		return -EINVAL;
	}

	for (top = 0U; top < 16U; top++) {
		uint64_t period = (1ULL << (top + 16U)) - 1ULL;

		if (period >= MAX(ticks, RK3506_WDT_MIN_PERIOD)) {
			data->timeout_top = top;
			data->timeout_installed = true;
			return 0;
		}
	}

	return -EINVAL;
}

static int rk3506_wdt_feed(const struct device *dev, int channel_id)
{
	if (channel_id != 0) {
		return -EINVAL;
	}

	sys_write32(RK3506_WDT_CRR_RESTART, rk3506_wdt_reg(dev, RK3506_WDT_CRR));
	return 0;
}

static int rk3506_wdt_disable(const struct device *dev)
{
	const struct rk3506_wdt_config *cfg = dev->config;
	int ret;

	ret = reset_line_toggle_dt(&cfg->pclk_reset);
	if (ret != 0) {
		return ret;
	}
	ret = reset_line_toggle_dt(&cfg->tclk_reset);
	if (ret == 0) {
		((struct rk3506_wdt_data *)dev->data)->timeout_installed = false;
	}
	return ret;
}

static DEVICE_API(wdt, rk3506_wdt_api) = {
	.setup = rk3506_wdt_setup,
	.install_timeout = rk3506_wdt_install_timeout,
	.feed = rk3506_wdt_feed,
	.disable = rk3506_wdt_disable,
};

static int rk3506_wdt_init(const struct device *dev)
{
	const struct rk3506_wdt_config *cfg = dev->config;
	struct rk3506_wdt_data *data = dev->data;
	uint32_t rpl;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	if (!device_is_ready(cfg->clock_dev) || !device_is_ready(cfg->pclk_reset.dev) ||
	    !device_is_ready(cfg->tclk_reset.dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->pclk);
	if (ret != 0) {
		return ret;
	}
	ret = clock_control_on(cfg->clock_dev, cfg->tclk);
	if (ret != 0) {
		return ret;
	}
	ret = clock_control_get_rate(cfg->clock_dev, cfg->tclk, &data->clock_hz);
	if (ret != 0 || data->clock_hz == 0U) {
		return ret != 0 ? ret : -EIO;
	}

	(void)reset_line_toggle_dt(&cfg->pclk_reset);
	(void)reset_line_toggle_dt(&cfg->tclk_reset);
	uint32_t glb_rst = sys_read32(cfg->cru_base + RK3506_CRU_GLB_RST_CON);
	uint32_t global_mask = RK3506_GLB_PMU_WDT_EN |
			       RK3506_GLB_PMU_GLOBAL_EN |
			       RK3506_GLB_PMU_FIRST_RESET;

	if (cfg->wdt_index == 0U) {
		global_mask |= RK3506_GLB_WDT0_GLOBAL_EN |
			       RK3506_GLB_WDT0_FIRST_RESET;
	} else {
		global_mask |= RK3506_GLB_WDT1_GLOBAL_EN |
			       RK3506_GLB_WDT1_FIRST_RESET;
	}
	sys_write32(glb_rst | global_mask, cfg->cru_base + RK3506_CRU_GLB_RST_CON);
	rpl = ilog2(cfg->reset_pulse_length) - 1U;
	sys_write32(0U, rk3506_wdt_reg(dev, RK3506_WDT_TORR));
	sys_write32(rpl << 2U, rk3506_wdt_reg(dev, RK3506_WDT_CR));
	return 0;
}

#define RK3506_WDT_INIT(n)                                                                         \
	static const struct rk3506_wdt_config rk3506_wdt_config_##n = {                           \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                               \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(n, tclk)),                   \
		.tclk = (clock_control_subsys_t)(uintptr_t)DT_INST_CLOCKS_CELL_BY_NAME(n, tclk,    \
										  clkid),   \
		.pclk = (clock_control_subsys_t)(uintptr_t)DT_INST_CLOCKS_CELL_BY_NAME(n, pclk,    \
										  clkid),   \
		.pclk_reset = RESET_DT_SPEC_INST_GET_BY_IDX(n, 0),                                  \
		.tclk_reset = RESET_DT_SPEC_INST_GET_BY_IDX(n, 1),                                  \
		.cru_base = DT_REG_ADDR(DT_INST_PHANDLE(n, rockchip_cru)),                           \
		.reset_pulse_length = DT_INST_PROP(n, reset_pulse_length),                          \
		.wdt_index = DT_INST_PROP(n, rockchip_wdt_index),                                   \
	};                                                                                          \
	static struct rk3506_wdt_data rk3506_wdt_data_##n;                                        \
	DEVICE_DT_INST_DEFINE(n, rk3506_wdt_init, NULL, &rk3506_wdt_data_##n,                      \
			      &rk3506_wdt_config_##n, POST_KERNEL,                                  \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &rk3506_wdt_api);

DT_INST_FOREACH_STATUS_OKAY(RK3506_WDT_INIT)
