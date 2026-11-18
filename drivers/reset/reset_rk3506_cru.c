/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk3506_cru_rst

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define RK3506_CRU_SOFTRST_CON(n)	(0xa00U + ((n) * 4U))

struct rk3506_cru_reset_config {
	DEVICE_MMIO_ROM;
};

struct rk3506_cru_reset_data {
	DEVICE_MMIO_RAM;
	struct k_spinlock lock;
};

struct rk3506_cru_reset_range {
	uint16_t first;
	uint16_t last;
	uint8_t reg;
};

static const struct rk3506_cru_reset_range rk3506_cru_reset_ranges[] = {
	{ 0, 8, 0 },     { 9, 11, 2 },    { 12, 18, 3 },   { 19, 27, 4 },
	{ 28, 37, 5 },   { 38, 53, 6 },   { 54, 66, 7 },   { 67, 68, 8 },
	{ 69, 70, 9 },   { 71, 75, 10 },  { 76, 88, 11 },  { 89, 100, 12 },
	{ 101, 115, 13 }, { 116, 123, 14 }, { 124, 130, 17 }, { 131, 140, 18 },
	{ 141, 149, 19 }, { 150, 161, 21 }, { 162, 162, 22 },
};

static bool rk3506_cru_reset_lookup(uint32_t id, uint8_t *reg, uint8_t *bit)
{
	for (size_t i = 0U; i < ARRAY_SIZE(rk3506_cru_reset_ranges); i++) {
		const struct rk3506_cru_reset_range *range = &rk3506_cru_reset_ranges[i];

		if (id >= range->first && id <= range->last) {
			*reg = range->reg;
			*bit = id - range->first;
			return true;
		}
	}

	return false;
}

static int reset_rk3506_status(const struct device *dev, uint32_t id, uint8_t *status)
{
	uint8_t reg;
	uint8_t bit;

	if (!rk3506_cru_reset_lookup(id, &reg, &bit)) {
		return -EINVAL;
	}

	*status = !!(sys_read32(DEVICE_MMIO_GET(dev) + RK3506_CRU_SOFTRST_CON(reg)) & BIT(bit));
	return 0;
}

static void rk3506_cru_reset_write(const struct device *dev, uint8_t reg, uint8_t bit, bool assert)
{
	struct rk3506_cru_reset_data *data = dev->data;
	mm_reg_t base = DEVICE_MMIO_GET(dev);
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	uint32_t addr = base + RK3506_CRU_SOFTRST_CON(reg);
	uint32_t val = sys_read32(addr);

	if (assert) {
		val = (BIT(bit) << 16) | (val | BIT(bit));
	} else {
		val = (BIT(bit) << 16) | (val & ~BIT(bit));
	}

	sys_write32(val, addr);
	k_spin_unlock(&data->lock, key);
}

static int reset_rk3506_line_assert(const struct device *dev, uint32_t id)
{
	uint8_t reg;
	uint8_t bit;

	if (!rk3506_cru_reset_lookup(id, &reg, &bit)) {
		return -EINVAL;
	}

	rk3506_cru_reset_write(dev, reg, bit, true);
	return 0;
}

static int reset_rk3506_line_deassert(const struct device *dev, uint32_t id)
{
	uint8_t reg;
	uint8_t bit;

	if (!rk3506_cru_reset_lookup(id, &reg, &bit)) {
		return -EINVAL;
	}

	rk3506_cru_reset_write(dev, reg, bit, false);
	return 0;
}

static int reset_rk3506_line_toggle(const struct device *dev, uint32_t id)
{
	int ret = reset_rk3506_line_assert(dev, id);

	if (ret != 0) {
		return ret;
	}

	return reset_rk3506_line_deassert(dev, id);
}

static DEVICE_API(reset, reset_rk3506_driver_api) = {
	.status = reset_rk3506_status,
	.line_assert = reset_rk3506_line_assert,
	.line_deassert = reset_rk3506_line_deassert,
	.line_toggle = reset_rk3506_line_toggle,
};

static int reset_rk3506_init(const struct device *dev)
{
	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	return 0;
}

#define RESET_RK3506_DEFINE(n)						\
	static struct rk3506_cru_reset_data rk3506_cru_reset_data_##n;	\
	static const struct rk3506_cru_reset_config				\
		rk3506_cru_reset_config_##n = {					\
		DEVICE_MMIO_ROM_INIT(DT_INST_PARENT(n)),			\
	};									\
	DEVICE_DT_INST_DEFINE(n, reset_rk3506_init, NULL,			\
			      &rk3506_cru_reset_data_##n,			\
			      &rk3506_cru_reset_config_##n, PRE_KERNEL_1,	\
			      CONFIG_RESET_INIT_PRIORITY,			\
			      &reset_rk3506_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RESET_RK3506_DEFINE)
