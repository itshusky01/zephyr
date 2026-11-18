/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_RK3506_PINCTRL_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_RK3506_PINCTRL_H_

#define RK3506_PINCTRL_MUX(bank, pin, func) \
	(((bank) << 16) | ((pin) << 8) | (func))

#define RK3506_PINCTRL_BANK(value) (((value) >> 16) & 0xffU)
#define RK3506_PINCTRL_PIN(value)  (((value) >> 8) & 0xffU)
#define RK3506_PINCTRL_MUX_FUNC(value) ((value) & 0xffU)

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_RK3506_PINCTRL_H_ */
