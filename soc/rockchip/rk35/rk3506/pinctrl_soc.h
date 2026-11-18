/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_ROCKCHIP_RK3506_PINCTRL_SOC_H_
#define ZEPHYR_SOC_ROCKCHIP_RK3506_PINCTRL_SOC_H_

#include <zephyr/device.h>
#include <zephyr/dt-bindings/pinctrl/rk3506-pinctrl.h>

struct rk3506_pinctrl_soc_pin {
	const struct device *dev;
	uint8_t bank;
	uint8_t pin;
	uint8_t mux;
	uint8_t pull;
	uint8_t drive;
	uint8_t schmitt;
	uint8_t input;
	uint8_t open_drain;
	uint8_t slew;
};

typedef struct rk3506_pinctrl_soc_pin pinctrl_soc_pin_t;

#define RK3506_PINCTRL_PULL_NO_CHANGE 0xffU
#define RK3506_PINCTRL_PULL_NONE      0U
#define RK3506_PINCTRL_PULL_UP        1U
#define RK3506_PINCTRL_PULL_DOWN      2U
#define RK3506_PINCTRL_CONF_NO_CHANGE 0xffU

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)				\
	{									\
		.dev = DEVICE_DT_GET(DT_PARENT(DT_PARENT(node_id))),		\
		.bank = RK3506_PINCTRL_BANK(DT_PROP_BY_IDX(node_id, prop, idx)),	\
		.pin = RK3506_PINCTRL_PIN(DT_PROP_BY_IDX(node_id, prop, idx)),	\
		.mux = RK3506_PINCTRL_MUX_FUNC(DT_PROP_BY_IDX(node_id, prop, idx)), \
		.pull = DT_PROP_OR(node_id, bias_pull_up, 0) ?			\
			RK3506_PINCTRL_PULL_UP :				\
			(DT_PROP_OR(node_id, bias_pull_down, 0) ?		\
			 RK3506_PINCTRL_PULL_DOWN :				\
			 (DT_PROP_OR(node_id, bias_disable, 0) ?		\
			  RK3506_PINCTRL_PULL_NONE :				\
			  RK3506_PINCTRL_PULL_NO_CHANGE)),			\
		.drive = DT_PROP_OR(node_id, drive_strength,			\
				    RK3506_PINCTRL_CONF_NO_CHANGE),		\
		.schmitt = DT_PROP_OR(node_id, input_schmitt_enable, 0) ?		\
			1U : (DT_PROP_OR(node_id, input_schmitt_disable, 0) ?		\
			      0U : RK3506_PINCTRL_CONF_NO_CHANGE),			\
		.input = DT_PROP_OR(node_id, input_enable, 0) ?			\
			1U : (DT_PROP_OR(node_id, input_disable, 0) ?		\
			      0U : RK3506_PINCTRL_CONF_NO_CHANGE),			\
		.open_drain = DT_PROP_OR(node_id, drive_open_drain, 0) ?		\
			1U : (DT_PROP_OR(node_id, drive_push_pull, 0) ?		\
			      0U : RK3506_PINCTRL_CONF_NO_CHANGE),			\
		.slew = DT_PROP_OR(node_id, slew_rate,			\
				   RK3506_PINCTRL_CONF_NO_CHANGE),			\
	},

#define RK3506_PINCTRL_CHILD_NODE(node_id, prop) DT_PHANDLE(node_id, prop)
#define RK3506_PINCTRL_FOREACH_CHILD(node_id, fn, ...) \
	DT_FOREACH_CHILD_VARGS(node_id, fn, __VA_ARGS__)

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop) \
	{ RK3506_PINCTRL_FOREACH_CHILD(RK3506_PINCTRL_CHILD_NODE(node_id, prop), \
				       DT_FOREACH_PROP_ELEM, pinmux, \
				       Z_PINCTRL_STATE_PIN_INIT) }

#endif /* ZEPHYR_SOC_ROCKCHIP_RK3506_PINCTRL_SOC_H_ */
