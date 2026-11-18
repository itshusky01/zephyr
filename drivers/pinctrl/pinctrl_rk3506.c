/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk3506_pinctrl

#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

enum rk3506_ioc_block {
	RK3506_IOC_PMU,
	RK3506_IOC_IOC1,
	RK3506_IOC_GRF,
};

struct rk3506_bank_layout {
	enum rk3506_ioc_block block;
	uint8_t pin_count;
	uint16_t iomux[4];
	uint16_t pull;
	uint16_t drive;
	uint16_t schmitt;
	uint16_t input;
	uint16_t slew;
	uint16_t open_drain;
};

struct rk3506_pinctrl_config {
	DEVICE_MMIO_NAMED_ROM(grf);
	DEVICE_MMIO_NAMED_ROM(ioc1);
	DEVICE_MMIO_NAMED_ROM(pmu);
};

struct rk3506_pinctrl_data {
	DEVICE_MMIO_NAMED_RAM(grf);
	DEVICE_MMIO_NAMED_RAM(ioc1);
	DEVICE_MMIO_NAMED_RAM(pmu);
};

#define RK3506_PINCTRL_CFG(dev)  ((const struct rk3506_pinctrl_config *)(dev)->config)
#define RK3506_PINCTRL_DATA(dev) ((struct rk3506_pinctrl_data *)(dev)->data)

/* Required by the DEVICE_MMIO_NAMED_* accessors. */
#define DEV_CFG(dev)  RK3506_PINCTRL_CFG(dev)
#define DEV_DATA(dev) RK3506_PINCTRL_DATA(dev)

static const struct rk3506_bank_layout rk3506_banks[] = {
	{
		.block = RK3506_IOC_PMU,
		.pin_count = 25U,
		.iomux = { 0x000U, 0x008U, 0x010U, 0x830U },
		.pull = 0x200U,
		.drive = 0x100U,
		.schmitt = 0x400U,
		.input = 0x300U,
		.slew = 0x600U,
		.open_drain = 0x700U,
	},
	{
		.block = RK3506_IOC_IOC1,
		.pin_count = 28U,
		.iomux = { 0x020U, 0x028U, 0x030U, 0x038U },
		.pull = 0x210U,
		.drive = 0x140U,
		.schmitt = 0x410U,
		.input = 0x310U,
		.slew = 0x610U,
		.open_drain = 0x710U,
	},
	{
		.block = RK3506_IOC_GRF,
		.pin_count = 17U,
		.iomux = { 0x040U, 0x048U, 0x050U, 0x058U },
		.pull = 0x220U,
		.drive = 0x180U,
		.schmitt = 0x420U,
		.input = 0x320U,
		.slew = 0x620U,
		.open_drain = 0x720U,
	},
	{
		.block = RK3506_IOC_GRF,
		.pin_count = 15U,
		.iomux = { 0x060U, 0x068U, 0x070U, 0x078U },
		.pull = 0x230U,
		.drive = 0x1c0U,
		.schmitt = 0x430U,
		.input = 0x330U,
		.slew = 0x630U,
		.open_drain = 0x730U,
	},
	{
		.block = RK3506_IOC_GRF,
		.pin_count = 12U,
		.pull = 0x840U,
		.drive = 0x840U,
		.schmitt = 0x840U,
		.input = 0x840U,
		.slew = 0x840U,
	},
};

static uintptr_t rk3506_block_base(const struct device *dev, enum rk3506_ioc_block block)
{
	switch (block) {
	case RK3506_IOC_PMU:
		return DEVICE_MMIO_NAMED_GET(dev, pmu);
	case RK3506_IOC_IOC1:
		return DEVICE_MMIO_NAMED_GET(dev, ioc1);
	case RK3506_IOC_GRF:
		return DEVICE_MMIO_NAMED_GET(dev, grf);
	}

	return 0U;
}

static void rk3506_write_field(uintptr_t base, uint32_t offset, uint32_t mask, uint32_t value)
{
	sys_write32((mask << 16) | (value & mask), base + offset);
}

static bool rk3506_is_gpio0d0(const pinctrl_soc_pin_t *pin)
{
	return pin->bank == 0U && pin->pin == 24U;
}

static bool rk3506_is_saradc_pin(const pinctrl_soc_pin_t *pin)
{
	return pin->bank == 4U && pin->pin >= 8U && pin->pin <= 11U;
}

static const struct rk3506_bank_layout *rk3506_get_layout(const pinctrl_soc_pin_t *pin)
{
	const struct rk3506_bank_layout *layout;

	if (pin->bank >= ARRAY_SIZE(rk3506_banks)) {
		return NULL;
	}

	layout = &rk3506_banks[pin->bank];
	if (pin->bank == 4U) {
		if (pin->pin <= 5U || (pin->pin >= 8U && pin->pin <= 11U)) {
			return layout;
		}
		return NULL;
	}

	return pin->pin < layout->pin_count ? layout : NULL;
}

static int rk3506_apply_mux(const pinctrl_soc_pin_t *pin)
{
	const struct rk3506_bank_layout *layout = rk3506_get_layout(pin);
	uintptr_t base;
	uint32_t reg;
	uint32_t bit;
	uint32_t mask;

	if (layout == NULL) {
		return -EINVAL;
	}

	base = rk3506_block_base(pin->dev, layout->block);

	if (pin->bank == 4U) {
		if (rk3506_is_saradc_pin(pin)) {
			rk3506_write_field(base, 0x840U, 0xf0U, 0xf0U);
		}
		return 0;
	}

	if (rk3506_is_gpio0d0(pin)) {
		reg = 0x830U;
		bit = 0U;
		mask = 0x3U;
	} else {
		reg = layout->iomux[pin->pin / 8U];
		if ((pin->pin % 8U) >= 4U) {
			reg += 4U;
		}
		bit = (pin->pin % 4U) * 4U;
		mask = 0xfU;
	}

	rk3506_write_field(base, reg, mask << bit, (pin->mux & mask) << bit);
	return 0;
}

static int rk3506_apply_bias(const pinctrl_soc_pin_t *pin)
{
	const struct rk3506_bank_layout *layout = rk3506_get_layout(pin);
	uintptr_t base;
	uint32_t reg;
	uint32_t bit;
	uint32_t value;

	if (pin->pull == RK3506_PINCTRL_PULL_NO_CHANGE) {
		return 0;
	}

	if (layout == NULL) {
		return -EINVAL;
	}

	base = rk3506_block_base(pin->dev, layout->block);

	if (rk3506_is_gpio0d0(pin)) {
		reg = 0x830U;
		bit = 5U;
		value = pin->pull == RK3506_PINCTRL_PULL_UP ? 3U :
			(pin->pull == RK3506_PINCTRL_PULL_DOWN ? 1U : 0U);
	} else if (rk3506_is_saradc_pin(pin)) {
		reg = 0x840U;
		bit = 13U;
		value = pin->pull == RK3506_PINCTRL_PULL_UP ? 3U :
			(pin->pull == RK3506_PINCTRL_PULL_DOWN ? 1U : 0U);
	} else {
		if (pin->bank == 4U) {
			return -EINVAL;
		}
		reg = layout->pull + ((pin->pin / 8U) * 4U);
		bit = (pin->pin % 8U) * 2U;
		value = pin->pull == RK3506_PINCTRL_PULL_UP ? 1U :
			(pin->pull == RK3506_PINCTRL_PULL_DOWN ? 2U : 0U);
	}

	rk3506_write_field(base, reg, 0x3U << bit, value << bit);
	return 0;
}

static int rk3506_apply_drive(const pinctrl_soc_pin_t *pin)
{
	const struct rk3506_bank_layout *layout = rk3506_get_layout(pin);
	uintptr_t base;
	uint32_t reg;
	uint32_t bit;
	uint32_t mask;
	uint32_t value;

	if (pin->drive == RK3506_PINCTRL_CONF_NO_CHANGE) {
		return 0;
	}

	if (layout == NULL) {
		return -EINVAL;
	}

	base = rk3506_block_base(pin->dev, layout->block);

	if (rk3506_is_gpio0d0(pin)) {
		if (pin->drive > 3U) {
			return -EINVAL;
		}
		reg = 0x830U;
		bit = 3U;
		mask = 0x3U;
		value = pin->drive;
	} else if (rk3506_is_saradc_pin(pin)) {
		if (pin->drive > 3U) {
			return -EINVAL;
		}
		reg = 0x840U;
		bit = 10U;
		mask = 0x3U;
		value = pin->drive;
	} else {
		if (pin->bank == 4U) {
			return -EINVAL;
		}
		if (pin->drive > 5U) {
			return -EINVAL;
		}
		reg = layout->drive + ((pin->pin / 2U) * 4U);
		bit = (pin->pin % 2U) * 8U;
		mask = 0x3fU;
		value = (1U << (pin->drive + 1U)) - 1U;
	}

	rk3506_write_field(base, reg, mask << bit, value << bit);
	return 0;
}

static int rk3506_apply_schmitt(const pinctrl_soc_pin_t *pin)
{
	const struct rk3506_bank_layout *layout = rk3506_get_layout(pin);
	uintptr_t base;
	uint32_t reg;
	uint32_t bit;
	uint32_t mask;
	uint32_t value;

	if (pin->schmitt == RK3506_PINCTRL_CONF_NO_CHANGE) {
		return 0;
	}

	if (layout == NULL) {
		return -EINVAL;
	}

	base = rk3506_block_base(pin->dev, layout->block);

	if (rk3506_is_gpio0d0(pin) || rk3506_is_saradc_pin(pin)) {
		reg = rk3506_is_gpio0d0(pin) ? 0x830U : 0x840U;
		bit = rk3506_is_gpio0d0(pin) ? 9U : 8U;
		mask = 0x3U;
		value = pin->schmitt ? 0x3U : 0U;
	} else {
		if (pin->bank == 4U) {
			return -EINVAL;
		}
		reg = layout->schmitt + ((pin->pin / 8U) * 4U);
		bit = pin->pin % 8U;
		mask = BIT(bit);
		value = pin->schmitt ? 1U : 0U;
	}

	rk3506_write_field(base, reg, mask, value << bit);
	return 0;
}

static int rk3506_apply_input(const pinctrl_soc_pin_t *pin)
{
	const struct rk3506_bank_layout *layout = rk3506_get_layout(pin);
	uintptr_t base;
	uint32_t reg;
	uint32_t bit;

	if (pin->input == RK3506_PINCTRL_CONF_NO_CHANGE) {
		return 0;
	}

	if (layout == NULL) {
		return -EINVAL;
	}

	base = rk3506_block_base(pin->dev, layout->block);

	if (rk3506_is_gpio0d0(pin)) {
		reg = 0x830U;
		bit = 2U;
	} else if (rk3506_is_saradc_pin(pin)) {
		reg = 0x840U;
		bit = pin->pin - 4U;
	} else {
		if (pin->bank == 4U) {
			return -EINVAL;
		}
		reg = layout->input + ((pin->pin / 8U) * 4U);
		bit = pin->pin % 8U;
	}

	rk3506_write_field(base, reg, BIT(bit), pin->input << bit);
	return 0;
}

static int rk3506_apply_open_drain(const pinctrl_soc_pin_t *pin)
{
	const struct rk3506_bank_layout *layout = rk3506_get_layout(pin);
	uintptr_t base;
	uint32_t reg;
	uint32_t bit;

	if (pin->open_drain == RK3506_PINCTRL_CONF_NO_CHANGE) {
		return 0;
	}

	if (layout == NULL || rk3506_is_gpio0d0(pin) || pin->bank == 4U) {
		return -EINVAL;
	}

	base = rk3506_block_base(pin->dev, layout->block);
	reg = layout->open_drain + ((pin->pin / 8U) * 4U);
	bit = pin->pin % 8U;

	rk3506_write_field(base, reg, BIT(bit), pin->open_drain << bit);
	return 0;
}

static int rk3506_apply_slew(const pinctrl_soc_pin_t *pin)
{
	const struct rk3506_bank_layout *layout = rk3506_get_layout(pin);
	uintptr_t base;
	uint32_t reg;
	uint32_t bit;

	if (pin->slew == RK3506_PINCTRL_CONF_NO_CHANGE) {
		return 0;
	}

	if (layout == NULL) {
		return -EINVAL;
	}

	base = rk3506_block_base(pin->dev, layout->block);

	if (rk3506_is_gpio0d0(pin)) {
		reg = 0x830U;
		bit = 8U;
	} else if (rk3506_is_saradc_pin(pin)) {
		reg = 0x840U;
		bit = 12U;
	} else {
		if (pin->bank == 4U) {
			return -EINVAL;
		}
		reg = layout->slew + ((pin->pin / 8U) * 4U);
		bit = pin->pin % 8U;
	}

	rk3506_write_field(base, reg, BIT(bit), (pin->slew != 0U) << bit);
	return 0;
}

static int (*const rk3506_pin_ops[])(const pinctrl_soc_pin_t *) = {
	rk3506_apply_mux,
	rk3506_apply_bias,
	rk3506_apply_drive,
	rk3506_apply_schmitt,
	rk3506_apply_input,
	rk3506_apply_open_drain,
	rk3506_apply_slew,
};

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	ARG_UNUSED(reg);

	for (uint8_t pin = 0U; pin < pin_cnt; pin++) {
		for (size_t op = 0U; op < ARRAY_SIZE(rk3506_pin_ops); op++) {
			int ret = rk3506_pin_ops[op](&pins[pin]);

			if (ret != 0) {
				return ret;
			}
		}
	}

	return 0;
}

static int rk3506_pinctrl_init(const struct device *dev)
{
	DEVICE_MMIO_NAMED_MAP(dev, grf, K_MEM_CACHE_NONE);
	DEVICE_MMIO_NAMED_MAP(dev, ioc1, K_MEM_CACHE_NONE);
	DEVICE_MMIO_NAMED_MAP(dev, pmu, K_MEM_CACHE_NONE);
	return 0;
}

#define RK3506_PINCTRL_INIT(n)							\
	static struct rk3506_pinctrl_data rk3506_pinctrl_data_##n;		\
	static const struct rk3506_pinctrl_config rk3506_pinctrl_cfg_##n = {	\
		DEVICE_MMIO_NAMED_ROM_INIT(grf, DT_INST_PHANDLE(n, rockchip_grf)), \
		DEVICE_MMIO_NAMED_ROM_INIT(ioc1, DT_INST_PHANDLE(n, rockchip_ioc1)), \
		DEVICE_MMIO_NAMED_ROM_INIT(pmu, DT_INST_PHANDLE(n, rockchip_pmu)), \
	};									\
										\
	DEVICE_DT_INST_DEFINE(n, rk3506_pinctrl_init, NULL,			\
			      &rk3506_pinctrl_data_##n, &rk3506_pinctrl_cfg_##n,	\
			      PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);

DT_INST_FOREACH_STATUS_OKAY(RK3506_PINCTRL_INIT)
