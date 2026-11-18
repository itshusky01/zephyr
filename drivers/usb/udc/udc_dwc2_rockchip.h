/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_USB_UDC_DWC2_ROCKCHIP_H
#define ZEPHYR_DRIVERS_USB_UDC_DWC2_ROCKCHIP_H

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define RK3506_USB2PHY_OTG0_SUS	0x0060U
#define RK3506_USB2PHY_OTG1_SUS	0x0070U
#define RK3506_USB2PHY_SUS_MASK	GENMASK(8, 0)
#define RK3506_USB2PHY_SUS_ON	0x1d1U
#define RK3506_USB2PHY_OTG0_HS	0x0030U
#define RK3506_USB2PHY_OTG1_HS	0x0430U
#define RK3506_USB2PHY_OTG0_TX	0x0094U
#define RK3506_USB2PHY_OTG1_TX	0x0494U
#define RK3506_USB2PHY_CLKOUT	0x041cU

struct rk3506_usb_dwc2 {
	const struct device *const clock_dev;
	const clock_control_subsys_t *const clock_ids;
	size_t clock_count;
	const struct reset_dt_spec ctrl_reset;
	const struct device *const phy_clock_dev;
	const clock_control_subsys_t *const phy_clock_ids;
	size_t phy_clock_count;
	const struct reset_dt_spec *const phy_resets;
	size_t phy_reset_count;
	mm_reg_t *const phy_base;
	uintptr_t phy_phys;
	size_t phy_size;
	uint8_t phy_port;
};

static inline void rk3506_usb2phy_update(uintptr_t reg, uint32_t mask, uint32_t value)
{
	uint32_t val = sys_read32(reg);

	val = (val & ~mask) | (value & mask);
	sys_write32(val, reg);
}

static inline int rk3506_usb_dwc2_pre_enable(const struct rk3506_usb_dwc2 *cfg)
{
	int ret;

	device_map(cfg->phy_base, cfg->phy_phys, cfg->phy_size, K_MEM_CACHE_NONE);

	if (!device_is_ready(cfg->phy_clock_dev)) {
		return -ENODEV;
	}

	for (size_t i = 0U; i < cfg->phy_clock_count; i++) {
		ret = clock_control_on(cfg->phy_clock_dev, cfg->phy_clock_ids[i]);
		if (ret != 0) {
			return ret;
		}
	}

	for (size_t i = 0U; i < cfg->phy_reset_count; i++) {
		if (device_is_ready(cfg->phy_resets[i].dev)) {
			ret = reset_line_toggle_dt(&cfg->phy_resets[i]);
			if (ret != 0) {
				return ret;
			}
		}
	}

	/* Enable the 480 MHz PHY clock output. */
	rk3506_usb2phy_update((*cfg->phy_base) + RK3506_USB2PHY_CLKOUT,
			      GENMASK(7, 2), 0x27U << 2);
	k_busy_wait(1300);

	/* RK3506 PHY tuning from the Rockchip USB2 PHY driver. */
	sys_clear_bits((*cfg->phy_base) + RK3506_USB2PHY_OTG0_HS, BIT(2));
	sys_clear_bits((*cfg->phy_base) + RK3506_USB2PHY_OTG1_HS, BIT(2));
	rk3506_usb2phy_update((*cfg->phy_base) + RK3506_USB2PHY_OTG0_HS,
			      GENMASK(6, 4), 5U << 4);
	rk3506_usb2phy_update((*cfg->phy_base) + RK3506_USB2PHY_OTG1_HS,
			      GENMASK(6, 4), 5U << 4);
	rk3506_usb2phy_update((*cfg->phy_base) + RK3506_USB2PHY_OTG0_TX,
			      GENMASK(6, 3), 3U << 3);
	rk3506_usb2phy_update((*cfg->phy_base) + RK3506_USB2PHY_OTG1_TX,
			      GENMASK(6, 3), 3U << 3);

	if (!device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}

	for (size_t i = 0U; i < cfg->clock_count; i++) {
		ret = clock_control_on(cfg->clock_dev, cfg->clock_ids[i]);
		if (ret != 0) {
			return ret;
		}
	}

	if (device_is_ready(cfg->ctrl_reset.dev)) {
		ret = reset_line_toggle_dt(&cfg->ctrl_reset);
		if (ret != 0) {
			return ret;
		}
	}

	uintptr_t sus_reg = (*cfg->phy_base) +
		(cfg->phy_port == 0U ? RK3506_USB2PHY_OTG0_SUS : RK3506_USB2PHY_OTG1_SUS);

	rk3506_usb2phy_update(sus_reg, RK3506_USB2PHY_SUS_MASK, RK3506_USB2PHY_SUS_ON);
	k_busy_wait(2000);

	return 0;
}


#define RK3506_USB_DWC2_DEFINE(n)							\
	static mm_reg_t rockchip_phy_base_##n;						\
										\
	static const clock_control_subsys_t rockchip_usb_clocks_##n[] = {			\
		(clock_control_subsys_t)(uintptr_t)					\
			DT_INST_CLOCKS_CELL_BY_IDX(n, 0, clkid),				\
		(clock_control_subsys_t)(uintptr_t)					\
			DT_INST_CLOCKS_CELL_BY_IDX(n, 1, clkid),				\
		(clock_control_subsys_t)(uintptr_t)					\
			DT_INST_CLOCKS_CELL_BY_IDX(n, 2, clkid),				\
	};										\
											\
	static const clock_control_subsys_t rockchip_phy_clocks_##n[] = {			\
		(clock_control_subsys_t)(uintptr_t)					\
			DT_CLOCKS_CELL_BY_NAME(DT_PARENT(DT_INST_PHANDLE(n, phys)),	\
					       phyclk, clkid),				\
		(clock_control_subsys_t)(uintptr_t)					\
			DT_CLOCKS_CELL_BY_NAME(DT_PARENT(DT_INST_PHANDLE(n, phys)),	\
					       apb_pclk, clkid),				\
	};										\
											\
	static const struct reset_dt_spec rockchip_phy_resets_##n[] = {			\
		RESET_DT_SPEC_GET_BY_IDX(DT_PARENT(DT_INST_PHANDLE(n, phys)), 0),	\
		RESET_DT_SPEC_GET_BY_IDX(DT_PARENT(DT_INST_PHANDLE(n, phys)), 1),	\
	};										\
											\
	static const struct rk3506_usb_dwc2 rockchip_usb_cfg_##n = {			\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(n, otg)),		\
		.clock_ids = rockchip_usb_clocks_##n,					\
		.clock_count = ARRAY_SIZE(rockchip_usb_clocks_##n),			\
		.ctrl_reset = RESET_DT_SPEC_INST_GET_BY_IDX(n, 0),			\
		.phy_clock_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR_BY_NAME(			\
			DT_PARENT(DT_INST_PHANDLE(n, phys)), phyclk)),			\
		.phy_clock_ids = rockchip_phy_clocks_##n,				\
		.phy_clock_count = ARRAY_SIZE(rockchip_phy_clocks_##n),		\
		.phy_resets = rockchip_phy_resets_##n,				\
		.phy_reset_count = ARRAY_SIZE(rockchip_phy_resets_##n),		\
		.phy_base = &rockchip_phy_base_##n,				\
		.phy_phys = DT_REG_ADDR(DT_PARENT(DT_INST_PHANDLE(n, phys))),	\
		.phy_size = DT_REG_SIZE(DT_PARENT(DT_INST_PHANDLE(n, phys))),	\
		.phy_port = DT_INST_PROP_OR(n, rockchip_phy_port, 0),		\
	};										\
											\
	static int rockchip_usb_pre_enable_##n(const struct device *dev)			\
	{										\
		int ret;									\
											\
		ret = rk3506_usb_dwc2_pre_enable(&rockchip_usb_cfg_##n);		\
		return ret;								\
	}										\
											\
	static int rockchip_usb_caps_##n(const struct device *dev)			\
	{										\
		struct udc_data *data = dev->data;					\
		data->caps.hs = IS_ENABLED(CONFIG_UDC_DRIVER_HIGH_SPEED_SUPPORT_ENABLED); \
		return 0;								\
	}										\
											\
	const struct dwc2_vendor_quirks dwc2_vendor_quirks_##n = {			\
		.pre_enable = rockchip_usb_pre_enable_##n,				\
		.caps = rockchip_usb_caps_##n,						\
	};

DT_INST_FOREACH_STATUS_OKAY(RK3506_USB_DWC2_DEFINE)

#endif /* ZEPHYR_DRIVERS_USB_UDC_DWC2_ROCKCHIP_H */
