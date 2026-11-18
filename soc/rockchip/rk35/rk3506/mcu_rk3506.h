/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SOC_ROCKCHIP_RK3506_MCU_H_
#define SOC_ROCKCHIP_RK3506_MCU_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * RK3506 BUSMCU0 is managed by the Rockchip secure firmware. This API owns
 * the non-secure side of the M0 lifecycle: image validation, SRAM loading,
 * clock/reset release, hot restart, and status reporting.
 */

#define RK3506_M0_SRAM_BASE 0xfff80000U
#define RK3506_M0_SRAM_SIZE 0xc000U
#define RK3506_M0_IMAGE_BASE 0xfff84000U
#define RK3506_M0_IMAGE_MAX_SIZE 0x8000U
#define RK3506_M0_MCU_ID 0x0000U
#define RK3506_M0_IMAGE_MAGIC 0x4d30494dU
#define RK3506_M0_IMAGE_VERSION 1U
#define RK3506_M0_IMAGE_HEADER_SIZE 0x20U

struct rk3506_m0_image_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t image_size;
	uint32_t load_addr;
	uint32_t entry_addr;
	uint32_t image_crc32;
	uint32_t flags;
	uint32_t reserved;
};

struct rk3506_m0_status {
	uint32_t raw_status;
	bool reset_released;
	bool hclk_enabled;
	bool swclktck_enabled;
	bool stclk_enabled;
	bool running;
	bool sleeping;
	bool lockup;
	bool halted;
	bool wakeup;
	bool txev;
	bool hclk_gate_ok;
	bool a7_direct_load_blocked;
};

int rk3506_m0_start(uintptr_t entry);
int rk3506_m0_boot_image(uintptr_t load_addr, const void *image, size_t size,
			 uintptr_t entry);
int rk3506_m0_boot_container(const void *container, size_t size);
int rk3506_m0_stop(void);
int rk3506_m0_restart(uintptr_t entry);
int rk3506_m0_probe(void);
int rk3506_m0_get_status(struct rk3506_m0_status *status);

#endif /* SOC_ROCKCHIP_RK3506_MCU_H_ */
