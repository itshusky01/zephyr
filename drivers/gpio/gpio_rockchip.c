/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_gpio_bank

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/irq.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "gpio_rockchip.h"

enum rk_gpio_layout {
	RK_GPIO_LAYOUT_LEGACY,
	RK_GPIO_LAYOUT_SPLIT,
};

enum rk_gpio_port_reg {
	RK_GPIO_PORT_OUTPUT,
	RK_GPIO_PORT_DIRECTION,
};

enum rk_gpio_irq_reg {
	RK_GPIO_IRQ_ENABLE,
	RK_GPIO_IRQ_MASK,
	RK_GPIO_IRQ_EDGE,
	RK_GPIO_IRQ_POLARITY,
	RK_GPIO_IRQ_BOTH,
	RK_GPIO_IRQ_STATUS,
	RK_GPIO_IRQ_ACK,
};

struct rk_gpio_config {
	struct gpio_driver_config common;
	DEVICE_MMIO_NAMED_ROM(mmio);
	bool swap_pins;
	void (*irq_connect)(void);
};

struct rk_gpio_data {
	struct gpio_driver_data common;
	DEVICE_MMIO_NAMED_RAM(mmio);
	enum rk_gpio_layout layout;
	sys_slist_t callbacks;
	struct k_spinlock lock;
};

#define RK_GPIO_CFG(dev)  ((const struct rk_gpio_config *)(dev)->config)
#define RK_GPIO_DATA(dev) ((struct rk_gpio_data *)(dev)->data)
#define RK_GPIO_BASE(dev) DEVICE_MMIO_NAMED_GET(dev, mmio)

/* Required by the DEVICE_MMIO_NAMED_* accessors. */
#define DEV_CFG(dev)  RK_GPIO_CFG(dev)
#define DEV_DATA(dev) RK_GPIO_DATA(dev)

static uint32_t rk_gpio_swap_bank4(uint32_t value)
{
	static const uint8_t swap[] = { 1U, 0U, 3U, 2U, 5U, 4U };
	uint32_t result = value & ~BIT_MASK(RK_GPIO_BANK4_SWAP_PINS);

	for (uint32_t pin = 0U; pin < ARRAY_SIZE(swap); pin++) {
		if ((value & BIT(pin)) != 0U) {
			result |= BIT(swap[pin]);
		}
	}

	return result;
}

static uint32_t rk_gpio_map_pins(const struct device *dev, uint32_t value)
{
	return RK_GPIO_CFG(dev)->swap_pins ? rk_gpio_swap_bank4(value) : value;
}

static uint32_t rk_gpio_port_reg(const struct device *dev, enum rk_gpio_port_reg reg)
{
	if (RK_GPIO_DATA(dev)->layout == RK_GPIO_LAYOUT_SPLIT) {
		return reg == RK_GPIO_PORT_OUTPUT ? RK_GPIO_SPLIT_OUTPUT_REG :
						   RK_GPIO_SPLIT_DIRECTION_REG;
	}

	return reg == RK_GPIO_PORT_OUTPUT ? RK_GPIO_LEGACY_OUTPUT_REG :
					   RK_GPIO_LEGACY_DIRECTION_REG;
}

static uint32_t rk_gpio_irq_reg(const struct device *dev, enum rk_gpio_irq_reg reg)
{
	if (RK_GPIO_DATA(dev)->layout == RK_GPIO_LAYOUT_SPLIT) {
		switch (reg) {
		case RK_GPIO_IRQ_ENABLE:
			return RK_GPIO_SPLIT_IRQ_EN_REG;
		case RK_GPIO_IRQ_MASK:
			return RK_GPIO_SPLIT_IRQ_MASK_REG;
		case RK_GPIO_IRQ_EDGE:
			return RK_GPIO_SPLIT_IRQ_EDGE_REG;
		case RK_GPIO_IRQ_POLARITY:
			return RK_GPIO_SPLIT_IRQ_POL_REG;
		case RK_GPIO_IRQ_BOTH:
			return RK_GPIO_SPLIT_IRQ_BOTH_REG;
		case RK_GPIO_IRQ_STATUS:
			return RK_GPIO_SPLIT_IRQ_STAT_REG;
		case RK_GPIO_IRQ_ACK:
			return RK_GPIO_SPLIT_IRQ_ACK_REG;
		}
	} else {
		switch (reg) {
		case RK_GPIO_IRQ_ENABLE:
			return RK_GPIO_LEGACY_IRQ_EN_REG;
		case RK_GPIO_IRQ_MASK:
			return RK_GPIO_LEGACY_IRQ_MASK_REG;
		case RK_GPIO_IRQ_EDGE:
			return RK_GPIO_LEGACY_IRQ_EDGE_REG;
		case RK_GPIO_IRQ_POLARITY:
			return RK_GPIO_LEGACY_IRQ_POL_REG;
		case RK_GPIO_IRQ_BOTH:
		case RK_GPIO_IRQ_STATUS:
			return RK_GPIO_LEGACY_IRQ_STAT_REG;
		case RK_GPIO_IRQ_ACK:
			return RK_GPIO_LEGACY_IRQ_ACK_REG;
		}
	}

	return 0U;
}

static uint32_t rk_gpio_read_port(const struct device *dev, enum rk_gpio_port_reg reg)
{
	uintptr_t base = RK_GPIO_BASE(dev);
	uint32_t offset = rk_gpio_port_reg(dev, reg);
	uint32_t value;

	if (RK_GPIO_DATA(dev)->layout == RK_GPIO_LAYOUT_SPLIT) {
		value = (sys_read32(base + offset + 4U) << 16) |
			sys_read32(base + offset);
	} else {
		value = sys_read32(base + offset);
	}

	return rk_gpio_map_pins(dev, value);
}

static void rk_gpio_write_port(const struct device *dev, enum rk_gpio_port_reg reg,
			       uint32_t value)
{
	uintptr_t base = RK_GPIO_BASE(dev);
	uint32_t offset = rk_gpio_port_reg(dev, reg);

	value = rk_gpio_map_pins(dev, value);

	if (RK_GPIO_DATA(dev)->layout == RK_GPIO_LAYOUT_SPLIT) {
		sys_write32((value & 0xffffU) | 0xffff0000U, base + offset);
		sys_write32((value >> 16) | 0xffff0000U, base + offset + 4U);
	} else {
		sys_write32(value, base + offset);
	}
}

static void rk_gpio_update_port(const struct device *dev, enum rk_gpio_port_reg reg,
				uint32_t mask, uint32_t value)
{
	uint32_t old = rk_gpio_read_port(dev, reg);

	rk_gpio_write_port(dev, reg, (old & ~mask) | (value & mask));
}

static void rk_gpio_write_irq_field(const struct device *dev, enum rk_gpio_irq_reg reg,
				    gpio_pin_t pin, bool enable)
{
	uintptr_t base = RK_GPIO_BASE(dev);
	uint32_t offset = rk_gpio_irq_reg(dev, reg);

	if (RK_GPIO_DATA(dev)->layout == RK_GPIO_LAYOUT_SPLIT) {
		uint32_t bit = pin % 16U;

		if (pin >= 16U) {
			offset += 4U;
		}

		sys_write32(BIT(bit + 16U) | (enable ? BIT(bit) : 0U), base + offset);
	} else {
		uint32_t state = sys_read32(base + offset);

		if (enable) {
			state |= BIT(pin);
		} else {
			state &= ~BIT(pin);
		}
		sys_write32(state, base + offset);
	}
}

static void rk_gpio_ack_irq(const struct device *dev, uint32_t mask)
{
	uintptr_t base = RK_GPIO_BASE(dev);
	uint32_t offset = rk_gpio_irq_reg(dev, RK_GPIO_IRQ_ACK);

	if (RK_GPIO_DATA(dev)->layout == RK_GPIO_LAYOUT_SPLIT) {
		uint32_t lower = mask & 0xffffU;
		uint32_t upper = (mask >> 16) & 0xffffU;

		sys_write32((lower << 16) | lower, base + offset);
		sys_write32((upper << 16) | upper, base + offset + 4U);
	} else {
		sys_write32(mask, base + offset);
	}
}

static int rk_gpio_pin_configure(const struct device *dev, gpio_pin_t pin,
				 gpio_flags_t flags)
{
	k_spinlock_key_t key;
	uint32_t pin_mask;

	if (pin >= RK_GPIO_BANK_PINS) {
		return -EINVAL;
	}

	if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN | GPIO_OPEN_DRAIN)) != 0U) {
		return -ENOTSUP;
	}

	pin_mask = BIT(pin);
	key = k_spin_lock(&RK_GPIO_DATA(dev)->lock);

	if ((flags & GPIO_OUTPUT) != 0U) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
			rk_gpio_update_port(dev, RK_GPIO_PORT_OUTPUT, pin_mask, pin_mask);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
			rk_gpio_update_port(dev, RK_GPIO_PORT_OUTPUT, pin_mask, 0U);
		}

		rk_gpio_update_port(dev, RK_GPIO_PORT_DIRECTION, pin_mask, pin_mask);
	} else {
		rk_gpio_update_port(dev, RK_GPIO_PORT_DIRECTION, pin_mask, 0U);
	}

	k_spin_unlock(&RK_GPIO_DATA(dev)->lock, key);
	return 0;
}

static int rk_gpio_port_get_raw(const struct device *dev, uint32_t *value)
{
	uint32_t offset = RK_GPIO_DATA(dev)->layout == RK_GPIO_LAYOUT_SPLIT ?
			  RK_GPIO_SPLIT_EXT_PORT_REG : RK_GPIO_LEGACY_EXT_PORT_REG;

	*value = sys_read32(RK_GPIO_BASE(dev) + offset);
	return 0;
}

static int rk_gpio_port_set_masked_raw(const struct device *dev, uint32_t mask,
				       uint32_t value)
{
	k_spinlock_key_t key = k_spin_lock(&RK_GPIO_DATA(dev)->lock);

	rk_gpio_update_port(dev, RK_GPIO_PORT_OUTPUT, mask, value);
	k_spin_unlock(&RK_GPIO_DATA(dev)->lock, key);
	return 0;
}

static int rk_gpio_port_set_bits_raw(const struct device *dev, uint32_t mask)
{
	return rk_gpio_port_set_masked_raw(dev, mask, mask);
}

static int rk_gpio_port_clear_bits_raw(const struct device *dev, uint32_t mask)
{
	return rk_gpio_port_set_masked_raw(dev, mask, 0U);
}

static int rk_gpio_port_toggle_bits(const struct device *dev, uint32_t mask)
{
	k_spinlock_key_t key = k_spin_lock(&RK_GPIO_DATA(dev)->lock);
	uint32_t value = rk_gpio_read_port(dev, RK_GPIO_PORT_OUTPUT);

	rk_gpio_write_port(dev, RK_GPIO_PORT_OUTPUT, value ^ mask);
	k_spin_unlock(&RK_GPIO_DATA(dev)->lock, key);
	return 0;
}

static int rk_gpio_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
					   enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	k_spinlock_key_t key;
	bool edge;
	bool both;

	if (pin >= RK_GPIO_BANK_PINS) {
		return -EINVAL;
	}

	edge = mode == GPIO_INT_MODE_EDGE;
	both = edge && trig == GPIO_INT_TRIG_BOTH;
	if (both && RK_GPIO_DATA(dev)->layout != RK_GPIO_LAYOUT_SPLIT) {
		return -ENOTSUP;
	}

	key = k_spin_lock(&RK_GPIO_DATA(dev)->lock);

	rk_gpio_write_irq_field(dev, RK_GPIO_IRQ_ENABLE, pin, false);
	rk_gpio_write_irq_field(dev, RK_GPIO_IRQ_MASK, pin, true);
	rk_gpio_ack_irq(dev, BIT(pin));

	if (mode != GPIO_INT_MODE_DISABLED) {
		rk_gpio_write_irq_field(dev, RK_GPIO_IRQ_EDGE, pin, edge);
		rk_gpio_write_irq_field(dev, RK_GPIO_IRQ_POLARITY, pin,
					trig == GPIO_INT_TRIG_HIGH || trig == GPIO_INT_TRIG_BOTH);
		if (RK_GPIO_DATA(dev)->layout == RK_GPIO_LAYOUT_SPLIT) {
			rk_gpio_write_irq_field(dev, RK_GPIO_IRQ_BOTH, pin, both);
		}
		rk_gpio_write_irq_field(dev, RK_GPIO_IRQ_MASK, pin, false);
		rk_gpio_write_irq_field(dev, RK_GPIO_IRQ_ENABLE, pin, true);
	}

	k_spin_unlock(&RK_GPIO_DATA(dev)->lock, key);
	return 0;
}

static int rk_gpio_manage_callback(const struct device *dev,
				   struct gpio_callback *callback, bool set)
{
	return gpio_manage_callback(&RK_GPIO_DATA(dev)->callbacks, callback, set);
}

static uint32_t rk_gpio_get_pending_int(const struct device *dev)
{
	return sys_read32(RK_GPIO_BASE(dev) + rk_gpio_irq_reg(dev, RK_GPIO_IRQ_STATUS));
}

static void rk_gpio_isr(const void *arg)
{
	const struct device *dev = arg;
	uint32_t status = rk_gpio_get_pending_int(dev);

	if (status == 0U) {
		return;
	}

	rk_gpio_ack_irq(dev, status);
	gpio_fire_callbacks(&RK_GPIO_DATA(dev)->callbacks, dev, status);
}

static int rk_gpio_detect_layout(struct rk_gpio_data *data, uintptr_t base)
{
	uint32_t version = sys_read32(base + RK_GPIO_VERSION_REG);

	switch (version) {
	case RK_GPIO_VERSION_LEGACY:
		data->layout = RK_GPIO_LAYOUT_LEGACY;
		return 0;
	case RK_GPIO_VERSION_V2:
	case RK_GPIO_VERSION_V2_1:
	case RK_GPIO_VERSION_V2_2:
	case RK_GPIO_VERSION_V2_6:
		data->layout = RK_GPIO_LAYOUT_SPLIT;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int rk_gpio_init(const struct device *dev)
{
	struct rk_gpio_data *data = RK_GPIO_DATA(dev);
	const struct rk_gpio_config *config = RK_GPIO_CFG(dev);
	int ret;

	DEVICE_MMIO_NAMED_MAP(dev, mmio, K_MEM_CACHE_NONE);
	ret = rk_gpio_detect_layout(data, RK_GPIO_BASE(dev));
	if (ret != 0) {
		return ret;
	}

	sys_slist_init(&data->callbacks);

	if (config->irq_connect != NULL) {
		config->irq_connect();
	}

	return 0;
}

static DEVICE_API(gpio, rk_gpio_api) = {
	.pin_configure = rk_gpio_pin_configure,
	.port_get_raw = rk_gpio_port_get_raw,
	.port_set_masked_raw = rk_gpio_port_set_masked_raw,
	.port_set_bits_raw = rk_gpio_port_set_bits_raw,
	.port_clear_bits_raw = rk_gpio_port_clear_bits_raw,
	.port_toggle_bits = rk_gpio_port_toggle_bits,
	.pin_interrupt_configure = rk_gpio_pin_interrupt_configure,
	.manage_callback = rk_gpio_manage_callback,
	.get_pending_int = rk_gpio_get_pending_int,
};

#define RK_GPIO_DT_INIT(n)								\
	static void rk_gpio_irq_connect_##n(void)					\
	{										\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), rk_gpio_isr,	\
			    DEVICE_DT_INST_GET(n), 0);					\
		irq_enable(DT_INST_IRQN(n));						\
	}										\
											\
	static struct rk_gpio_data rk_gpio_data_##n;					\
											\
	static const struct rk_gpio_config rk_gpio_cfg_##n = {				\
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(n),				\
		DEVICE_MMIO_NAMED_ROM_INIT(mmio, DT_DRV_INST(n)),			\
		.swap_pins = DT_INST_PROP_OR(n, rockchip_bank4_pin_remap, false),	\
		.irq_connect = rk_gpio_irq_connect_##n,					\
	};										\
											\
	DEVICE_DT_INST_DEFINE(n, rk_gpio_init, NULL, &rk_gpio_data_##n,			\
			      &rk_gpio_cfg_##n, POST_KERNEL,				\
			      CONFIG_GPIO_INIT_PRIORITY, &rk_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(RK_GPIO_DT_INIT)
