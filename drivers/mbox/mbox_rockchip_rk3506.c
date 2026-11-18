/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk3506_mailbox

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define RK_MBOX_A2B_INTEN  0x00U
#define RK_MBOX_A2B_STATUS 0x04U
#define RK_MBOX_A2B_CMD    0x08U
#define RK_MBOX_A2B_DATA   0x0cU
#define RK_MBOX_B2A_INTEN  0x10U
#define RK_MBOX_B2A_STATUS 0x14U
#define RK_MBOX_B2A_CMD    0x18U
#define RK_MBOX_B2A_DATA   0x1cU

#define RK_MBOX_TX_DONE   BIT(0)
#define RK_MBOX_RX_DONE   BIT(1)
#define RK_MBOX_TRIG_MODE BIT(8)
#define RK_MBOX_MAX_DATA  8U

struct rk_mbox_msg {
	uint32_t cmd;
	uint32_t data;
};

struct rk_mbox_config {
	DEVICE_MMIO_ROM;
	const struct device *clock_dev;
	clock_control_subsys_t clock;
	void (*irq_config)(const struct device *dev);
};

struct rk_mbox_data {
	DEVICE_MMIO_RAM;
	mbox_callback_t callback;
	void *user_data;
	struct k_spinlock lock;
	bool enabled;
};

static void rk_mbox_write_enable(uintptr_t base, uint32_t offset, uint32_t mask,
				 uint32_t value)
{
	uint32_t old = sys_read32(base + offset);

	sys_write32((old & ~mask) | (value & mask) | (mask << 16), base + offset);
}

static int rk_mbox_send(const struct device *dev, mbox_channel_id_t channel_id,
			const struct mbox_msg *msg)
{
	struct rk_mbox_data *data = dev->data;
	uintptr_t base = DEVICE_MMIO_GET(dev);
	struct rk_mbox_msg frame;
	k_spinlock_key_t key;

	if (channel_id != 0U) {
		return -EINVAL;
	}
	if (msg == NULL || msg->size != sizeof(frame) || msg->data == NULL) {
		return -EMSGSIZE;
	}

	memcpy(&frame, msg->data, sizeof(frame));

	key = k_spin_lock(&data->lock);
	if ((sys_read32(base + RK_MBOX_A2B_STATUS) & RK_MBOX_TX_DONE) != 0U) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}

	rk_mbox_write_enable(base, RK_MBOX_A2B_INTEN,
			     RK_MBOX_TX_DONE | RK_MBOX_TRIG_MODE,
			     RK_MBOX_TX_DONE | RK_MBOX_TRIG_MODE);
	sys_write32(frame.cmd, base + RK_MBOX_A2B_CMD);
	sys_write32(frame.data, base + RK_MBOX_A2B_DATA);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static void rk_mbox_irq_handler(const struct device *dev)
{
	struct rk_mbox_data *data = dev->data;
	uintptr_t base = DEVICE_MMIO_GET(dev);
	uint32_t status = sys_read32(base + RK_MBOX_B2A_STATUS);
	struct rk_mbox_msg frame;
	mbox_callback_t callback;
	void *user_data;
	k_spinlock_key_t key;

	if ((status & RK_MBOX_TX_DONE) == 0U) {
		if ((status & RK_MBOX_RX_DONE) != 0U) {
			sys_write32(RK_MBOX_RX_DONE, base + RK_MBOX_B2A_STATUS);
		}
		return;
	}

	frame.cmd = sys_read32(base + RK_MBOX_B2A_CMD);
	frame.data = sys_read32(base + RK_MBOX_B2A_DATA);

	/* Clearing TX-done also acknowledges the transfer to the remote side. */
	sys_write32(RK_MBOX_TX_DONE, base + RK_MBOX_B2A_STATUS);

	key = k_spin_lock(&data->lock);
	callback = data->callback;
	user_data = data->user_data;
	k_spin_unlock(&data->lock, key);

	if (callback != NULL) {
		struct mbox_msg msg = {
			.data = &frame,
			.size = sizeof(frame),
		};

		callback(dev, 0U, user_data, &msg);
	}

	if ((status & RK_MBOX_RX_DONE) != 0U) {
		sys_write32(RK_MBOX_RX_DONE, base + RK_MBOX_B2A_STATUS);
	}
}

static int rk_mbox_register_callback(const struct device *dev, mbox_channel_id_t channel_id,
				     mbox_callback_t callback, void *user_data)
{
	struct rk_mbox_data *data = dev->data;
	k_spinlock_key_t key;

	if (channel_id != 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	data->callback = callback;
	data->user_data = user_data;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int rk_mbox_mtu_get(const struct device *dev)
{
	ARG_UNUSED(dev);
	return RK_MBOX_MAX_DATA;
}

static uint32_t rk_mbox_max_channels_get(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 1U;
}

static int rk_mbox_set_enabled(const struct device *dev, mbox_channel_id_t channel_id,
			       bool enabled)
{
	struct rk_mbox_data *data = dev->data;
	uintptr_t base = DEVICE_MMIO_GET(dev);
	k_spinlock_key_t key;

	if (channel_id != 0U) {
		return -EINVAL;
	}
	if (enabled == data->enabled) {
		return -EALREADY;
	}

	key = k_spin_lock(&data->lock);
	rk_mbox_write_enable(base, RK_MBOX_B2A_INTEN, RK_MBOX_TX_DONE,
			     enabled ? RK_MBOX_TX_DONE : 0U);
	data->enabled = enabled;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static DEVICE_API(mbox, rk_mbox_api) = {
	.send = rk_mbox_send,
	.register_callback = rk_mbox_register_callback,
	.mtu_get = rk_mbox_mtu_get,
	.max_channels_get = rk_mbox_max_channels_get,
	.set_enabled = rk_mbox_set_enabled,
};

#define RK_MBOX_IRQ_CONFIG(n)                                                                  \
	static void rk_mbox_irq_config_##n(const struct device *dev)                            \
	{                                                                                      \
		ARG_UNUSED(dev);                                                                \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), rk_mbox_irq_handler,     \
			    DEVICE_DT_INST_GET(n), 0);                                          \
		irq_enable(DT_INST_IRQN(n));                                                    \
	}

static int rk_mbox_init(const struct device *dev)
{
	const struct rk_mbox_config *cfg = dev->config;
	uintptr_t base;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	base = DEVICE_MMIO_GET(dev);
	if (!device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->clock);
	if (ret != 0) {
		return ret;
	}

	rk_mbox_write_enable(base, RK_MBOX_A2B_INTEN, RK_MBOX_TX_DONE, 0U);
	rk_mbox_write_enable(base, RK_MBOX_B2A_INTEN, RK_MBOX_TX_DONE, 0U);
	rk_mbox_write_enable(base, RK_MBOX_B2A_INTEN, RK_MBOX_RX_DONE, 0U);

	cfg->irq_config(dev);
	return 0;
}

#define RK_MBOX_DEFINE(n)                                                                      \
	RK_MBOX_IRQ_CONFIG(n)                                                                  \
	static struct rk_mbox_data rk_mbox_data_##n;                                            \
	static const struct rk_mbox_config rk_mbox_config_##n = {                              \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                             \
		.clock = (clock_control_subsys_t)(uintptr_t)DT_INST_CLOCKS_CELL(n, clkid),      \
		.irq_config = rk_mbox_irq_config_##n,                                           \
	};                                                                                     \
	DEVICE_DT_INST_DEFINE(n, rk_mbox_init, NULL, &rk_mbox_data_##n, &rk_mbox_config_##n,   \
			      POST_KERNEL, CONFIG_MBOX_INIT_PRIORITY, &rk_mbox_api);

DT_INST_FOREACH_STATUS_OKAY(RK_MBOX_DEFINE)
