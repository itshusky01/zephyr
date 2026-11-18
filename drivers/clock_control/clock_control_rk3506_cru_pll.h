/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_CLOCK_CONTROL_CLOCK_CONTROL_RK3506_CRU_PLL_H_
#define ZEPHYR_DRIVERS_CLOCK_CONTROL_CLOCK_CONTROL_RK3506_CRU_PLL_H_

#include <stdint.h>

int rk3506_cru_set_v0pll_rate(uintptr_t base, uint32_t rate);

#endif /* ZEPHYR_DRIVERS_CLOCK_CONTROL_CLOCK_CONTROL_RK3506_CRU_PLL_H_ */
