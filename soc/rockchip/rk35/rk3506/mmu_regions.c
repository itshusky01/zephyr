/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/arch/arm/mmu/arm_mmu.h>
#include <zephyr/sys/util.h>

extern char _image_ram_end[];

#define ZEPHYR_RAM_BASE DT_REG_ADDR(DT_CHOSEN(zephyr_sram))
#define ZEPHYR_RAM_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_sram))
#define ZEPHYR_RAM_END  (ZEPHYR_RAM_BASE + ZEPHYR_RAM_SIZE)

#if defined(CONFIG_RK3506_M0)
#define M0_SHARED_NODE DT_PHANDLE(DT_NODELABEL(m0), memory_region)

BUILD_ASSERT(ZEPHYR_RAM_END <= DT_REG_ADDR(M0_SHARED_NODE),
	     "m0_shared overlaps Zephyr RAM");
BUILD_ASSERT((DT_REG_ADDR(M0_SHARED_NODE) &
	      (CONFIG_MMU_PAGE_SIZE - 1U)) == 0U,
	     "m0_shared base is not page aligned");
BUILD_ASSERT((DT_REG_SIZE(M0_SHARED_NODE) &
	      (CONFIG_MMU_PAGE_SIZE - 1U)) == 0U,
	     "m0_shared size is not page aligned");

#define M0_SHARED_MMU_ENTRY							\
	/* MT_NORMAL without cache attributes is normal non-cacheable memory. */ \
	MMU_REGION_FLAT_ENTRY("m0_shared",					\
			      DT_REG_ADDR(M0_SHARED_NODE),			\
			      DT_REG_SIZE(M0_SHARED_NODE),			\
			      MT_NORMAL | MATTR_SHARED | MPERM_R | MPERM_W),
#else
#define M0_SHARED_MMU_ENTRY
#endif

/* Keep this region mutable so the image end can be resolved after linking. */
static struct arm_mmu_region mmu_regions[] = {
	MMU_REGION_FLAT_ENTRY("zephyr_free_ram",
			      ZEPHYR_RAM_BASE,
			      ZEPHYR_RAM_SIZE,
			      MT_NORMAL | MATTR_SHARED |
			      MPERM_R | MPERM_W |
			      MATTR_CACHE_OUTER_WB_WA | MATTR_CACHE_INNER_WB_WA),
	MMU_REGION_DT_COMPAT_FOREACH_FLAT_ENTRY(ns16550,
						MT_DEVICE | MPERM_R | MPERM_W)
	MMU_REGION_FLAT_ENTRY("gic", 0xff581000, 0x8000,
			      MT_STRONGLY_ORDERED | MPERM_R | MPERM_W),
	MMU_REGION_FLAT_ENTRY("grf", 0xff288000, 0x4000,
			      MT_DEVICE | MPERM_R | MPERM_W),
	MMU_REGION_FLAT_ENTRY("otp", 0xff4f0000, 0x4000,
			      MT_DEVICE | MPERM_R | MPERM_W),
	MMU_REGION_DT_COMPAT_FOREACH_FLAT_ENTRY(rockchip_rk3506_tsadc,
						MT_DEVICE | MPERM_R | MPERM_W)
	MMU_REGION_FLAT_ENTRY("ioc_grf", 0xff4d8000, 0x8000,
			      MT_DEVICE | MPERM_R | MPERM_W),
	MMU_REGION_FLAT_ENTRY("ioc1", 0xff660000, 0x10000,
			      MT_DEVICE | MPERM_R | MPERM_W),
	MMU_REGION_FLAT_ENTRY("ioc_pmu", 0xff950000, 0x10000,
			      MT_DEVICE | MPERM_R | MPERM_W),
	MMU_REGION_FLAT_ENTRY("pmu", 0xff900000, 0x1000,
			      MT_DEVICE | MPERM_R | MPERM_W),
	MMU_REGION_FLAT_ENTRY("pmugrf", 0xff910000, 0x4000,
			      MT_DEVICE | MPERM_R | MPERM_W),
	MMU_REGION_DT_COMPAT_FOREACH_FLAT_ENTRY(rockchip_gpio_bank,
						MT_DEVICE | MPERM_R | MPERM_W)
	MMU_REGION_DT_COMPAT_FOREACH_FLAT_ENTRY(rockchip_rk3506_mailbox,
						MT_DEVICE | MPERM_R | MPERM_W)
	MMU_REGION_DT_COMPAT_FOREACH_FLAT_ENTRY(rockchip_rk3506_cru,
						MT_DEVICE | MPERM_R | MPERM_W)
	MMU_REGION_FLAT_ENTRY("m0_sram", 0xfff80000, 0xc000,
			      MT_DEVICE | MPERM_R | MPERM_W),
	M0_SHARED_MMU_ENTRY
	MMU_REGION_DT_COMPAT_FOREACH_FLAT_ENTRY(rockchip_rk3506_dw_mshc,
						MT_DEVICE | MPERM_R | MPERM_W)
	MMU_REGION_DT_COMPAT_FOREACH_FLAT_ENTRY(rockchip_rk3506_usb2phy,
						MT_DEVICE | MPERM_R | MPERM_W)
	MMU_REGION_DT_COMPAT_FOREACH_FLAT_ENTRY(rockchip_rk3506_usb,
						MT_DEVICE | MPERM_R | MPERM_W)
};

void arm_mmu_config_setup(void)
{
	uint32_t start = POINTER_TO_UINT(_image_ram_end);

	__ASSERT(start >= ZEPHYR_RAM_BASE && start < ZEPHYR_RAM_END,
		 "image end 0x%08x is outside Zephyr RAM", start);
	__ASSERT((start & (CONFIG_MMU_PAGE_SIZE - 1U)) == 0U,
		 "image end 0x%08x is not page aligned", start);

	mmu_regions[0].base_pa = start;
	mmu_regions[0].base_va = start;
	mmu_regions[0].size = ZEPHYR_RAM_END - start;
}

const struct arm_mmu_config mmu_config = {
	.num_regions = ARRAY_SIZE(mmu_regions),
	.mmu_regions = mmu_regions,
};
