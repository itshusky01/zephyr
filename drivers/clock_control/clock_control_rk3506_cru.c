/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk3506_cru_clk

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/rk3506-cru.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_CLOCK_CONTROL_ROCKCHIP_RK3506_RUNTIME_PLL)
#include "clock_control_rk3506_cru_pll.h"
#endif

#define RK3506_CRU_CLKSEL_CON(n)	(0x300U + ((n) * 4U))
#define RK3506_CRU_CLKGATE_CON(n)	(0x800U + ((n) * 4U))
#define RK3506_CRU_PMU_CLKSEL_CON(n)	(0x10300U + ((n) * 4U))
#define RK3506_CRU_PMU_CLKGATE_CON(n)	(0x10800U + ((n) * 4U))
#define RK3506_CRU_PLL_CON(n)		(0x10000U + ((n) * 4U))
#define RK3506_CRU_CLK_GPLL_DIV		9U
#define RK3506_CRU_CLK_GPLL_DIV_100M	10U
#define RK3506_CRU_CLK_V0PLL_DIV	11U
#define RK3506_CRU_CLK_V1PLL_DIV	12U
#define RK3506_CRU_XIN24M       0xfff0U
#define RK3506_CRU_SARADC_400K  0xfff1U
#define RK3506_CRU_SARADC_32K   0xfff2U

/*
 * Sources which are intentionally not assigned a numeric rate yet.  A clock
 * whose current mux selects one of these sources reports -ENOTSUP instead of
 * inventing a frequency.
 */
#define RK3506_CRU_EXT_32K		0xffe0U
#define RK3506_CRU_EXT_CORE_PVTPLL	0xffe1U
#define RK3506_CRU_EXT_SAI0_MCLK_IN	0xffe2U
#define RK3506_CRU_EXT_SAI0_SCLK_IN	0xffe3U
#define RK3506_CRU_EXT_SAI1_MCLK_IN	0xffe4U
#define RK3506_CRU_EXT_SAI1_SCLK_IN	0xffe5U
#define RK3506_CRU_EXT_SAI2_MCLK_IN	0xffe6U
#define RK3506_CRU_EXT_SAI2_SCLK_IN	0xffe7U
#define RK3506_CRU_EXT_SAI3_MCLK_IN	0xffe8U
#define RK3506_CRU_EXT_SAI3_SCLK_IN	0xffe9U
#define RK3506_CRU_EXT_CLK_RC		0xffeaU
#define RK3506_CRU_EXT_XIN32K		0xffebU
#define RK3506_CRU_EXT_PLL_REF_IO	0xffecU
#define RK3506_CRU_EXT_DEEPSLOW_PMU	0xffedU

struct rk3506_cru_config {
	DEVICE_MMIO_ROM;
};

struct rk3506_cru_data {
	DEVICE_MMIO_RAM;
	struct k_mutex set_rate_lock;
	struct k_spinlock reg_lock;
};

#define RK3506_CRU_DATA(dev) ((struct rk3506_cru_data *)(dev)->data)

struct rk3506_cru_gate {
	uint16_t id;
	uint16_t reg;
	uint8_t bit;
	bool pmu;
};

struct rk3506_cru_clk {
	uint16_t id;
	uint16_t parents[16];
	uint8_t parent_count;
	uint8_t reg;
	uint8_t sel_shift;
	uint8_t sel_width;
	uint8_t div_shift;
	uint8_t div_width;
	bool pmu;
	uint8_t frac_high_reg;
	bool fractional;
};

static const struct rk3506_cru_gate rk3506_cru_gates[] = {
	{ XIN24M_GATE, 0, 1, false },
	{ CLK_GPLL_GATE, 0, 2, false },
	{ CLK_V0PLL_GATE, 0, 3, false },
	{ CLK_V1PLL_GATE, 0, 4, false },
	{ CLK_GPLL_DIV, 0, 5, false },
	{ CLK_GPLL_DIV_100M, 0, 6, false },
	{ CLK_V0PLL_DIV, 0, 7, false },
	{ CLK_V1PLL_DIV, 0, 8, false },
	{ CLK_INT_VOICE_MATRIX0, 0, 9, false },
	{ CLK_INT_VOICE_MATRIX1, 0, 10, false },
	{ CLK_INT_VOICE_MATRIX2, 0, 11, false },
	{ CLK_FRAC_UART_MATRIX0, 0, 13, false },
	{ CLK_FRAC_UART_MATRIX1, 0, 14, false },
	{ CLK_FRAC_VOICE_MATRIX0, 0, 15, false },
	{ CLK_FRAC_VOICE_MATRIX1, 1, 0, false },
	{ CLK_FRAC_COMMON_MATRIX0, 1, 1, false },
	{ CLK_FRAC_COMMON_MATRIX1, 1, 2, false },
	{ CLK_FRAC_COMMON_MATRIX2, 1, 3, false },
	{ ACLK_CORE_ROOT, 2, 11, false },
	{ PCLK_CORE_ROOT, 2, 12, false },
	{ HCLK_CORE_PERI_ROOT, 4, 1, false },
	{ PCLK_CORE_PERI_ROOT, 4, 2, false },
	{ CLK_DSMC, 4, 4, false },
	{ ACLK_DSMC, 4, 5, false },
	{ PCLK_DSMC, 4, 6, false },
	{ CLK_FLEXBUS_TX, 4, 7, false },
	{ CLK_FLEXBUS_RX, 4, 8, false },
	{ ACLK_FLEXBUS, 4, 9, false },
	{ HCLK_FLEXBUS, 4, 10, false },
	{ ACLK_DSMC_SLV, 4, 11, false },
	{ HCLK_DSMC_SLV, 4, 12, false },
	{ ACLK_BUS_ROOT, 5, 0, false },
	{ HCLK_BUS_ROOT, 5, 1, false },
	{ PCLK_BUS_ROOT, 5, 2, false },
	{ PCLK_TIMER, 6, 2, false },
	{ CLK_TIMER0_CH0, 6, 3, false },
	{ CLK_TIMER0_CH1, 6, 4, false },
	{ CLK_TIMER0_CH2, 6, 5, false },
	{ CLK_TIMER0_CH3, 6, 6, false },
	{ CLK_TIMER0_CH4, 6, 7, false },
	{ CLK_TIMER0_CH5, 6, 8, false },
	{ PCLK_WDT0, 6, 9, false },
	{ TCLK_WDT0, 6, 10, false },
	{ PCLK_WDT1, 6, 11, false },
	{ TCLK_WDT1, 6, 12, false },
	{ PCLK_MAILBOX, 6, 13, false },
	{ HCLK_M0, 5, 10, false },
	{ STCLK_M0, 8, 2, false },
	{ PCLK_LSPERI_ROOT, 11, 1, false },
	{ PCLK_UART0, 11, 4, false },
	{ PCLK_UART1, 11, 5, false },
	{ PCLK_UART2, 11, 6, false },
	{ PCLK_UART3, 11, 7, false },
	{ PCLK_UART4, 11, 8, false },
	{ SCLK_UART0, 11, 9, false },
	{ SCLK_UART1, 11, 10, false },
	{ SCLK_UART2, 11, 11, false },
	{ SCLK_UART3, 11, 12, false },
	{ SCLK_UART4, 11, 13, false },
	{ PCLK_UART5, 19, 6, false },
	{ SCLK_UART5, 19, 7, false },
	{ PCLK_I2C0, 11, 14, false },
	{ CLK_I2C0, 11, 15, false },
	{ PCLK_I2C1, 12, 0, false },
	{ CLK_I2C1, 12, 1, false },
	{ PCLK_I2C2, 12, 2, false },
	{ CLK_I2C2, 12, 3, false },
	{ PCLK_PWM1, 12, 4, false },
	{ CLK_PWM1, 12, 5, false },
	{ CLK_OSC_PWM1, 12, 6, false },
	{ CLK_RC_PWM1, 12, 7, false },
	{ CLK_FREQ_PWM1, 12, 8, false },
	{ CLK_COUNTER_PWM1, 12, 9, false },
	{ PCLK_SPI0, 12, 10, false },
	{ CLK_SPI0, 12, 11, false },
	{ PCLK_SPI1, 12, 12, false },
	{ CLK_SPI1, 12, 13, false },
	{ PCLK_SPI2, 17, 10, false },
	{ CLK_SPI2, 20, 0, false },
	{ PCLK_GPIO0, 0, 8, true },
	{ DBCLK_GPIO0, 0, 9, true },
	{ PCLK_GPIO0_IOC, 0, 7, true },
	{ PCLK_GPIO1, 3, 8, false },
	{ DBCLK_GPIO1, 3, 9, false },
	{ PCLK_GPIO1_IOC, 22, 1, false },
	{ PCLK_GPIO2, 12, 14, false },
	{ DBCLK_GPIO2, 12, 15, false },
	{ PCLK_GPIO3, 13, 0, false },
	{ DBCLK_GPIO3, 13, 1, false },
	{ PCLK_GPIO4, 13, 2, false },
	{ DBCLK_GPIO4, 13, 3, false },
	{ HCLK_CAN0, 13, 4, false },
	{ CLK_CAN0, 13, 5, false },
	{ HCLK_CAN1, 13, 6, false },
	{ CLK_CAN1, 13, 7, false },
	{ HCLK_PDM, 13, 8, false },
	{ MCLK_PDM, 13, 9, false },
	{ CLKOUT_PDM, 13, 10, false },
	{ MCLK_SPDIFTX, 13, 11, false },
	{ HCLK_SPDIFTX, 13, 12, false },
	{ HCLK_SPDIFRX, 13, 13, false },
	{ MCLK_SPDIFRX, 13, 14, false },
	{ MCLK_SAI0, 13, 15, false },
	{ HCLK_SAI0, 14, 0, false },
	{ MCLK_OUT_SAI0, 14, 1, false },
	{ MCLK_SAI1, 14, 2, false },
	{ HCLK_SAI1, 14, 3, false },
	{ MCLK_OUT_SAI1, 14, 4, false },
	{ HCLK_ASRC0, 14, 5, false },
	{ CLK_ASRC0, 14, 6, false },
	{ HCLK_ASRC1, 14, 7, false },
	{ CLK_ASRC1, 14, 8, false },
	{ PCLK_CRU, 14, 9, false },
	{ PCLK_PMU_ROOT, 14, 10, false },
	{ MCLK_ASRC0, 16, 0, false },
	{ MCLK_ASRC1, 16, 1, false },
	{ MCLK_ASRC2, 16, 2, false },
	{ MCLK_ASRC3, 16, 3, false },
	{ ACLK_HSPERI_ROOT, 17, 0, false },
	{ HCLK_HSPERI_ROOT, 17, 1, false },
	{ PCLK_HSPERI_ROOT, 17, 2, false },
	{ MCLK_SAI2, 18, 2, false },
	{ HCLK_SAI2, 18, 3, false },
	{ MCLK_OUT_SAI2, 18, 4, false },
	{ MCLK_SAI3_SRC, 18, 5, false },
	{ HCLK_SAI3, 18, 6, false },
	{ MCLK_SAI3, 18, 7, false },
	{ MCLK_OUT_SAI3, 18, 8, false },
	{ MCLK_SAI4_SRC, 18, 9, false },
	{ HCLK_SAI4, 18, 10, false },
	{ MCLK_SAI4, 18, 11, false },
	{ PCLK_OTPC_NS, 19, 3, false },
	{ CLK_SBPI_OTPC_NS, 19, 4, false },
	{ PCLK_TSADC, 21, 14, false },
	{ CLK_TSADC, 21, 15, false },
	{ CLK_TSADC_TSEN, 22, 0, false },
	{ CLK_REF_USBPHY_TOP, 1, 4, false },
	{ HCLK_USBOTG0, 7, 5, false },
	{ HCLK_USBOTG0_PMU, 7, 6, false },
	{ CLK_USBOTG0_ADP, 7, 7, false },
	{ HCLK_USBOTG1, 7, 8, false },
	{ HCLK_USBOTG1_PMU, 7, 9, false },
	{ CLK_USBOTG1_ADP, 7, 10, false },
	{ PCLK_USBPHY, 7, 11, false },
	{ PCLK_GPIO234_IOC, 19, 8, false },
	{ CCLK_SRC_SDMMC, 17, 6, false },
	{ HCLK_SDMMC, 17, 7, false },
	{ HCLK_FSPI, 17, 8, false },
	{ SCLK_FSPI, 17, 9, false },
	{ ACLK_MAC0, 17, 11, false },
	{ ACLK_MAC1, 17, 12, false },
	{ PCLK_MAC0, 17, 13, false },
	{ PCLK_MAC1, 17, 14, false },
	{ CLK_MAC_ROOT, 17, 15, false },
	{ CLK_MAC0, 18, 0, false },
	{ CLK_MAC1, 18, 1, false },
	{ CLK_MAC_PTP_ROOT, 19, 9, false },
	{ CLK_MAC0_PTP, 19, 10, false },
	{ CLK_MAC1_PTP, 19, 11, false },
	{ PCLK_SARADC, 19, 0, false },
	{ CLK_SARADC, 19, 1, false },
	{ ACLK_VIO_ROOT, 21, 0, false },
	{ HCLK_VIO_ROOT, 21, 1, false },
	{ PCLK_VIO_ROOT, 21, 2, false },
	{ HCLK_RGA, 21, 6, false },
	{ ACLK_RGA, 21, 7, false },
	{ CLK_CORE_RGA, 21, 8, false },
	{ ACLK_VOP, 21, 9, false },
	{ HCLK_VOP, 21, 10, false },
	{ DCLK_VOP, 21, 11, false },
	{ PCLK_DPHY, 21, 12, false },
	{ PCLK_DSI_HOST, 21, 13, false },
	{ CLK_REF_PHY_PLL, 1, 14, true },
	{ CLK_REF_OUT0, 1, 4, true },
	{ CLK_REF_OUT1, 1, 5, true },
	{ CLK_32K_FRAC, 1, 6, true },
	{ CLK_32K_RC, 1, 7, true },
	{ CLK_32K, 1, 8, true },
	{ CLK_32K_PMU, 1, 9, true },
	{ CLK_MAC_OUT, 1, 3, true },
	{ CLK_OSC_PWM0, 1, 1, true },
	{ CLK_RC_PWM0, 1, 2, true },
	{ CLK_PMU_HP_TIMER, 0, 13, true },
	{ CLK_PMU_HP_TIMER_32K, 0, 14, true },
	{ CLK_PMU_32K, 0, 3, true },
	{ PCLK_PMU_HP_TIMER, 0, 12, true },
	{ DBCLK_GPIO1_SHADOW, 0, 11, true },
	{ PCLK_GPIO1_SHADOW, 0, 10, true },
	{ PCLK_PMU_GRF, 0, 5, true },
	{ PCLK_PMU_CRU, 0, 4, true },
	{ PCLK_PMU, 0, 2, true },
	{ CLK_PMU, 0, 1, true },
	{ PCLK_PWM0, 0, 15, true },
	{ CLK_TOUCH_KEY, 1, 13, true },
	{ PCLK_TOUCH_KEY, 1, 12, true },
	{ CLK_PWM0, 1, 0, true },
	{ CLK_WIFI_OUT, 2, 0, true },
};

static const struct rk3506_cru_clk rk3506_cru_clks[] = {
	{ CLK_INT_VOICE_MATRIX0, { PLL_V0PLL }, 1, 1, 0, 0, 8, 5, false },
	{ CLK_INT_VOICE_MATRIX1, { PLL_V1PLL }, 1, 2, 0, 0, 0, 5, false },
	{ CLK_INT_VOICE_MATRIX2, { PLL_V0PLL }, 1, 2, 0, 0, 5, 5, false },
	{ CLK_FRAC_UART_MATRIX0_MUX,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  3, 9, 2, 0, 0, false },
	{ CLK_FRAC_UART_MATRIX1_MUX,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  3, 11, 2, 0, 0, false },
	{ CLK_FRAC_VOICE_MATRIX0_MUX,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  3, 13, 2, 0, 0, false },
	{ CLK_FRAC_VOICE_MATRIX1_MUX,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  4, 0, 2, 0, 0, false },
	{ CLK_FRAC_COMMON_MATRIX0_MUX,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  4, 2, 2, 0, 0, false },
	{ CLK_FRAC_COMMON_MATRIX1_MUX,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  4, 4, 2, 0, 0, false },
	{ CLK_FRAC_COMMON_MATRIX2_MUX,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  4, 6, 2, 0, 0, false },
	{ CLK_FRAC_UART_MATRIX0, { CLK_FRAC_UART_MATRIX0_MUX }, 1,
	  5, 0, 0, 0, 0, false, 0, true },
	{ CLK_FRAC_UART_MATRIX1, { CLK_FRAC_UART_MATRIX1_MUX }, 1,
	  6, 0, 0, 0, 0, false, 0, true },
	{ CLK_FRAC_VOICE_MATRIX0, { CLK_FRAC_VOICE_MATRIX0_MUX }, 1,
	  7, 0, 0, 0, 0, false, 8, true },
	{ CLK_FRAC_VOICE_MATRIX1, { CLK_FRAC_VOICE_MATRIX1_MUX }, 1,
	  9, 0, 0, 0, 0, false, 10, true },
	{ CLK_FRAC_COMMON_MATRIX0, { CLK_FRAC_COMMON_MATRIX0_MUX }, 1,
	  11, 0, 0, 0, 0, false, 0, true },
	{ CLK_FRAC_COMMON_MATRIX1, { CLK_FRAC_COMMON_MATRIX1_MUX }, 1,
	  12, 0, 0, 0, 0, false, 0, true },
	{ CLK_FRAC_COMMON_MATRIX2, { CLK_FRAC_COMMON_MATRIX2_MUX }, 1,
	  13, 0, 0, 0, 0, false, 0, true },
	{ ARMCLK, { PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 3, 15, 5, 2, 0, 5, false },
	{ ACLK_CORE_ROOT, { ARMCLK }, 1, 15, 0, 0, 9, 4, false },
	{ PCLK_CORE_ROOT, { ARMCLK }, 1, 16, 0, 0, 0, 4, false },
	{ ACLK_CORE_PERI_ROOT, { PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 3,
	  18, 5, 2, 0, 5, false },
	{ HCLK_CORE_PERI_ROOT, { ACLK_CORE_PERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ PCLK_CORE_PERI_ROOT, { ACLK_CORE_PERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_DSMC, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  18, 12, 2, 7, 5, false },
	{ ACLK_DSMC, { ACLK_CORE_PERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ PCLK_DSMC, { PCLK_CORE_PERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_FLEXBUS_TX, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  19, 5, 2, 0, 5, false },
	{ CLK_FLEXBUS_RX, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  19, 12, 2, 7, 5, false },
	{ ACLK_FLEXBUS, { ACLK_CORE_PERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_FLEXBUS, { HCLK_CORE_PERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ ACLK_DSMC_SLV, { ACLK_CORE_PERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_DSMC_SLV, { HCLK_CORE_PERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ ACLK_BUS_ROOT, { CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 3,
	  21, 5, 2, 0, 5, false },
	{ HCLK_BUS_ROOT, { CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 3,
	  21, 12, 2, 7, 5, false },
	{ PCLK_BUS_ROOT, { CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 3,
	  22, 5, 2, 0, 5, false },
	{ HCLK_M0, { ACLK_BUS_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ STCLK_M0, { RK3506_CRU_XIN24M }, 1, 23, 0, 0, 9, 6, false },
	{ HCLK_LSPERI_ROOT, { CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 3,
	  29, 5, 2, 0, 5, false },
	{ PCLK_LSPERI_ROOT, { HCLK_LSPERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_TIMER0_CH0,
	  { RK3506_CRU_XIN24M, CLK_GPLL_DIV_100M, RK3506_CRU_EXT_32K,
	    RK3506_CRU_EXT_CORE_PVTPLL, RK3506_CRU_EXT_SAI0_MCLK_IN,
	    RK3506_CRU_EXT_SAI0_SCLK_IN }, 6, 22, 7, 3, 0, 0, false },
	{ CLK_TIMER0_CH1,
	  { RK3506_CRU_XIN24M, CLK_GPLL_DIV_100M, RK3506_CRU_EXT_32K,
	    RK3506_CRU_EXT_CORE_PVTPLL, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI1_SCLK_IN }, 6, 22, 10, 3, 0, 0, false },
	{ CLK_TIMER0_CH2,
	  { RK3506_CRU_XIN24M, CLK_GPLL_DIV_100M, RK3506_CRU_EXT_32K,
	    RK3506_CRU_EXT_CORE_PVTPLL, RK3506_CRU_EXT_SAI2_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_SCLK_IN }, 6, 22, 13, 3, 0, 0, false },
	{ CLK_TIMER0_CH3,
	  { RK3506_CRU_XIN24M, CLK_GPLL_DIV_100M, RK3506_CRU_EXT_32K,
	    RK3506_CRU_EXT_CORE_PVTPLL, RK3506_CRU_EXT_SAI3_MCLK_IN,
	    RK3506_CRU_EXT_SAI3_SCLK_IN }, 6, 23, 0, 3, 0, 0, false },
	{ CLK_TIMER0_CH4,
	  { RK3506_CRU_XIN24M, CLK_GPLL_DIV_100M, RK3506_CRU_EXT_32K,
	    RK3506_CRU_EXT_CORE_PVTPLL, MCLK_ASRC0 }, 5,
	  23, 3, 3, 0, 0, false },
	{ CLK_TIMER0_CH5,
	  { RK3506_CRU_XIN24M, CLK_GPLL_DIV_100M, RK3506_CRU_EXT_32K,
	    RK3506_CRU_EXT_CORE_PVTPLL, MCLK_ASRC1 }, 5,
	  23, 6, 3, 0, 0, false },
	{ TCLK_WDT0, { RK3506_CRU_XIN24M }, 1, 0, 0, 0, 0, 0, false, 0, false },
	{ TCLK_WDT1, { RK3506_CRU_XIN24M }, 1, 0, 0, 0, 0, 0, false, 0, false },
	{ ACLK_HSPERI_ROOT, { CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 3,
	  49, 5, 2, 0, 5, false },
	{ HCLK_HSPERI_ROOT, { ACLK_HSPERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ PCLK_HSPERI_ROOT, { HCLK_HSPERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_SDMMC, { HCLK_HSPERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_FSPI, { HCLK_HSPERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ PCLK_OTPC_NS, { PCLK_HSPERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_SBPI_OTPC_NS, { RK3506_CRU_XIN24M }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_REF_USBPHY_TOP, { RK3506_CRU_XIN24M }, 1, 0, 0, 0, 0, 0, false },
	{ PCLK_USBPHY, { PCLK_BUS_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_USBOTG0, { HCLK_BUS_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_USBOTG0_PMU, { HCLK_BUS_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_USBOTG0_ADP, { CLK_32K }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_USBOTG1, { HCLK_BUS_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_USBOTG1_PMU, { HCLK_BUS_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_USBOTG1_ADP, { CLK_32K }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_TSADC, { RK3506_CRU_XIN24M }, 1, 61, 0, 0, 0, 8, false },
	{ CLK_TSADC_TSEN, { RK3506_CRU_XIN24M }, 1, 61, 0, 0, 8, 3, false },
	{ SCLK_UART0, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  29, 12, 3, 7, 5, false },
	{ SCLK_UART1, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  30, 5, 3, 0, 5, false },
	{ SCLK_UART2, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  30, 13, 3, 8, 5, false },
	{ SCLK_UART3, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  31, 5, 3, 0, 5, false },
	{ SCLK_UART4, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  31, 13, 3, 8, 5, false },
	{ SCLK_UART5, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  54, 11, 3, 6, 5, false },
	{ CLK_I2C0, { RK3506_CRU_XIN24M, CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 4,
	  32, 4, 2, 0, 4, false },
	{ CLK_I2C1, { RK3506_CRU_XIN24M, CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 4,
	  32, 10, 2, 6, 4, false },
	{ CLK_I2C2, { RK3506_CRU_XIN24M, CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 4,
	  33, 4, 2, 0, 4, false },
	{ CLK_SPI0, { RK3506_CRU_XIN24M, CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 4,
	  34, 8, 2, 4, 4, false },
	{ CLK_SPI1, { RK3506_CRU_XIN24M, CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 4,
	  34, 14, 2, 10, 4, false },
	{ CLK_PWM1, { CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 3,
	  33, 10, 2, 6, 4, false },
	{ CLK_FREQ_PWM1,
	  { RK3506_CRU_EXT_CLK_RC, RK3506_CRU_EXT_SAI0_MCLK_IN,
	    RK3506_CRU_EXT_SAI1_MCLK_IN, RK3506_CRU_EXT_SAI2_MCLK_IN,
	    RK3506_CRU_EXT_SAI3_MCLK_IN, RK3506_CRU_EXT_SAI0_SCLK_IN,
	    RK3506_CRU_EXT_SAI1_SCLK_IN, RK3506_CRU_EXT_SAI2_SCLK_IN,
	    RK3506_CRU_EXT_SAI3_SCLK_IN, MCLK_ASRC0, MCLK_ASRC1 }, 11,
	  33, 12, 4, 0, 0, false },
	{ CLK_COUNTER_PWM1,
	  { RK3506_CRU_EXT_CLK_RC, RK3506_CRU_EXT_SAI0_MCLK_IN,
	    RK3506_CRU_EXT_SAI1_MCLK_IN, RK3506_CRU_EXT_SAI2_MCLK_IN,
	    RK3506_CRU_EXT_SAI3_MCLK_IN, RK3506_CRU_EXT_SAI0_SCLK_IN,
	    RK3506_CRU_EXT_SAI1_SCLK_IN, RK3506_CRU_EXT_SAI2_SCLK_IN,
	    RK3506_CRU_EXT_SAI3_SCLK_IN, MCLK_ASRC0, MCLK_ASRC1 }, 11,
	  34, 0, 4, 0, 0, false },
	{ CLK_CAN0,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2 }, 8,
	  35, 11, 3, 6, 5, false },
	{ CLK_CAN1,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2 }, 8,
	  36, 5, 3, 0, 5, false },
	{ MCLK_PDM,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN,
	    CLK_GPLL_DIV }, 14, 37, 5, 4, 0, 5, false },
	{ CLKOUT_PDM,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN,
	    CLK_GPLL_DIV }, 14, 38, 10, 4, 0, 10, false },
	{ MCLK_SPDIFRX, { PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 3,
	  39, 14, 2, 9, 5, false },
	{ MCLK_SPDIFTX,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 39, 5, 4, 0, 5, false },
	{ MCLK_SAI0,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 40, 8, 4, 0, 8, false },
	{ MCLK_SAI1,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 41, 8, 4, 0, 8, false },
	{ MCLK_SAI2,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 51, 8, 4, 0, 8, false },
	{ MCLK_SAI3_SRC,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 52, 8, 4, 0, 8, false },
	{ MCLK_SAI3, { MCLK_SAI3_SRC }, 1, 0, 0, 0, 0, 0, false },
	{ MCLK_SAI4_SRC,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 53, 8, 4, 0, 8, false },
	{ MCLK_SAI4, { MCLK_SAI4_SRC }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_ASRC0, { PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 3,
	  42, 5, 2, 0, 5, false },
	{ CLK_ASRC1, { PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 3,
	  42, 12, 2, 7, 5, false },
	{ MCLK_ASRC0,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 46, 0, 4, 0, 0, false },
	{ MCLK_ASRC1,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 46, 4, 4, 0, 0, false },
	{ MCLK_ASRC2,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 46, 8, 4, 0, 0, false },
	{ MCLK_ASRC3,
	  { RK3506_CRU_XIN24M, CLK_INT_VOICE_MATRIX0, CLK_INT_VOICE_MATRIX1,
	    CLK_INT_VOICE_MATRIX2, CLK_FRAC_VOICE_MATRIX0,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2,
	    RK3506_CRU_EXT_SAI0_MCLK_IN, RK3506_CRU_EXT_SAI1_MCLK_IN,
	    RK3506_CRU_EXT_SAI2_MCLK_IN, RK3506_CRU_EXT_SAI3_MCLK_IN },
	  13, 46, 12, 4, 0, 0, false },
	{ CCLK_SRC_SDMMC, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  49, 13, 2, 7, 6, false },
	{ SCLK_FSPI, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  50, 5, 2, 0, 5, false },
	{ CLK_MAC_PTP_ROOT, { PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 3,
	  55, 5, 2, 0, 5, false },
	{ CLK_MAC0_PTP, { CLK_MAC_PTP_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_MAC1_PTP, { CLK_MAC_PTP_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_MAC_ROOT, { PLL_GPLL }, 1, 50, 0, 0, 7, 5, false },
	{ CLK_MAC0, { CLK_MAC_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_MAC1, { CLK_MAC_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ ACLK_VIO_ROOT, { PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 3,
	  58, 5, 2, 0, 5, false },
	{ HCLK_VIO_ROOT, { CLK_GPLL_DIV, CLK_V0PLL_DIV, CLK_V1PLL_DIV }, 3,
	  58, 12, 2, 7, 5, false },
	{ PCLK_VIO_ROOT, { HCLK_VIO_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ ACLK_RGA, { ACLK_VIO_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_RGA, { HCLK_VIO_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_CORE_RGA, { PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 3,
	  59, 5, 2, 0, 5, false },
	{ ACLK_VOP, { ACLK_VIO_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ HCLK_VOP, { HCLK_VIO_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ DCLK_VOP,
	  { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL,
	    CLK_FRAC_VOICE_MATRIX1, CLK_FRAC_COMMON_MATRIX0,
	    CLK_FRAC_COMMON_MATRIX1, CLK_FRAC_COMMON_MATRIX2 }, 8,
	  60, 8, 3, 0, 8, false },
	{ PCLK_DPHY, { PCLK_VIO_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ PCLK_DSI_HOST, { PCLK_VIO_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ PCLK_TSADC, { PCLK_VIO_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ PCLK_GPIO1_IOC, { PCLK_VIO_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_MAC_OUT, { PLL_GPLL }, 1, 0, 0, 0, 10, 6, true },
	{ PCLK_PMU_ROOT, { PCLK_LSPERI_ROOT }, 1, 0, 0, 0, 0, 0, false },
	{ CLK_PMU, { RK3506_CRU_XIN24M }, 1, 0, 0, 0, 0, 0, true },
	{ PCLK_PMU, { PCLK_PMU_ROOT }, 1, 0, 0, 0, 0, 0, true },
	{ PCLK_PMU_CRU, { PCLK_PMU_ROOT }, 1, 0, 0, 0, 0, 0, true },
	{ PCLK_PMU_GRF, { PCLK_PMU_ROOT }, 1, 0, 0, 0, 0, 0, true },
	{ PCLK_GPIO0, { PCLK_PMU_ROOT }, 1, 0, 0, 0, 0, 0, true },
	{ PCLK_GPIO0_IOC, { PCLK_PMU_ROOT }, 1, 0, 0, 0, 0, 0, true },
	{ PCLK_GPIO1_SHADOW, { PCLK_PMU_ROOT }, 1, 0, 0, 0, 0, 0, true },
	{ DBCLK_GPIO0,
	  { RK3506_CRU_XIN24M, RK3506_CRU_EXT_CLK_RC,
	    RK3506_CRU_EXT_DEEPSLOW_PMU }, 3,
	  0, 0, 2, 0, 0, true },
	{ DBCLK_GPIO1_SHADOW,
	  { RK3506_CRU_XIN24M, RK3506_CRU_EXT_CLK_RC,
	    RK3506_CRU_EXT_DEEPSLOW_PMU }, 3,
	  0, 2, 2, 0, 0, true },
	{ PCLK_PMU_HP_TIMER, { PCLK_PMU_ROOT }, 1, 0, 0, 0, 0, 0, true },
	{ CLK_PMU_HP_TIMER,
	  { RK3506_CRU_XIN24M, CLK_GPLL_DIV_100M,
	    RK3506_CRU_EXT_CORE_PVTPLL }, 3, 0, 4, 2, 0, 0, true },
	{ PCLK_PWM0, { PCLK_PMU_ROOT }, 1, 0, 0, 0, 0, 0, true },
	{ CLK_PWM0, { CLK_GPLL_DIV_100M }, 1, 0, 0, 0, 6, 4, true },
	{ CLK_OSC_PWM0, { RK3506_CRU_XIN24M }, 1, 0, 0, 0, 0, 0, true },
	{ CLK_RC_PWM0, { RK3506_CRU_EXT_CLK_RC }, 1, 0, 0, 0, 0, 0, true },
	{ CLK_REF_OUT0, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  1, 6, 2, 0, 6, true },
	{ CLK_REF_OUT1, { RK3506_CRU_XIN24M, PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 4,
	  1, 14, 2, 8, 6, true },
	{ CLK_32K_FRAC_MUX,
	  { RK3506_CRU_XIN24M, PLL_V0PLL, PLL_V1PLL, RK3506_CRU_EXT_CLK_RC }, 4,
	  3, 0, 2, 0, 0, true },
	{ CLK_32K_FRAC, { CLK_32K_FRAC_MUX }, 1,
	  2, 0, 0, 0, 0, true, 0, true },
	{ CLK_32K_RC, { RK3506_CRU_EXT_CLK_RC }, 1,
	  3, 0, 0, 2, 5, true },
	{ CLK_32K,
	  { RK3506_CRU_EXT_XIN32K, CLK_32K_RC, CLK_32K_FRAC }, 3,
	  3, 7, 2, 0, 0, true },
	{ CLK_32K_PMU,
	  { RK3506_CRU_EXT_XIN32K, CLK_32K_RC, CLK_32K_FRAC }, 3,
	  3, 9, 2, 0, 0, true },
	{ CLK_PMU_32K, { CLK_32K_PMU }, 1, 0, 0, 0, 0, 0, true },
	{ CLK_PMU_HP_TIMER_32K, { CLK_32K_PMU }, 1, 0, 0, 0, 0, 0, true },
	{ PCLK_TOUCH_KEY, { PCLK_PMU_ROOT }, 1, 0, 0, 0, 0, 0, true },
	{ CLK_TOUCH_KEY, { RK3506_CRU_XIN24M }, 1, 0, 0, 0, 0, 0, true },
	{ CLK_WIFI_OUT, { RK3506_CRU_XIN24M }, 1, 0, 0, 0, 0, 0, true },
	{ CLK_REF_PHY_PLL, { PLL_GPLL, PLL_V0PLL, PLL_V1PLL }, 3,
	  4, 13, 2, 6, 7, true },
	{ CLK_REF_PHY_PMU_MUX,
	  { RK3506_CRU_XIN24M, CLK_REF_PHY_PLL }, 2,
	  4, 15, 1, 0, 0, true },
	{ CLK_V0PLL_REF,
	  { RK3506_CRU_XIN24M, RK3506_CRU_EXT_PLL_REF_IO }, 2,
	  6, 0, 1, 0, 0, true },
	{ CLK_V1PLL_REF,
	  { RK3506_CRU_XIN24M, RK3506_CRU_EXT_PLL_REF_IO }, 2,
	  6, 1, 1, 0, 0, true },
	{ CLK_SARADC, { RK3506_CRU_XIN24M, RK3506_CRU_SARADC_400K,
			RK3506_CRU_SARADC_32K }, 3, 54, 4, 2, 0, 4, false },
	{ CLK_GPLL_DIV, { PLL_GPLL }, 1, 0, 0, 0, 6, 4, false },
	{ CLK_GPLL_DIV_100M, { CLK_GPLL_DIV }, 1, 0, 0, 0, 10, 4, false },
	{ CLK_V0PLL_DIV, { PLL_V0PLL }, 1, 1, 0, 0, 0, 4, false },
	{ CLK_V1PLL_DIV, { PLL_V1PLL }, 1, 1, 0, 0, 4, 4, false },
};

static const struct rk3506_cru_gate *rk3506_cru_find_gate(uint16_t id)
{
	for (size_t i = 0U; i < ARRAY_SIZE(rk3506_cru_gates); i++) {
		if (rk3506_cru_gates[i].id == id) {
			return &rk3506_cru_gates[i];
		}
	}

	return NULL;
}

static const struct rk3506_cru_clk *rk3506_cru_find_clk(uint16_t id)
{
	for (size_t i = 0U; i < ARRAY_SIZE(rk3506_cru_clks); i++) {
		if (rk3506_cru_clks[i].id == id) {
			return &rk3506_cru_clks[i];
		}
	}

	return NULL;
}

static bool rk3506_cru_can_set_rate(uint16_t id)
{
	switch (id) {
	case SCLK_UART0:
	case SCLK_UART1:
	case SCLK_UART2:
	case SCLK_UART3:
	case SCLK_UART4:
	case SCLK_UART5:
	case CLK_I2C0:
	case CLK_I2C1:
	case CLK_I2C2:
	case CLK_SPI0:
	case CLK_SPI1:
	case CCLK_SRC_SDMMC:
	case SCLK_FSPI:
	case CLK_MAC_ROOT:
	case CLK_PWM0:
	case CLK_PWM1:
	case CLK_CAN0:
	case CLK_CAN1:
	case MCLK_PDM:
	case CLKOUT_PDM:
	case MCLK_SPDIFTX:
	case MCLK_SPDIFRX:
	case MCLK_SAI0:
	case MCLK_SAI1:
	case MCLK_SAI2:
	case MCLK_SAI3_SRC:
	case MCLK_SAI4_SRC:
	case CLK_ASRC0:
	case CLK_ASRC1:
	case CLK_TSADC:
	case CLK_TSADC_TSEN:
	case STCLK_M0:
		return true;
	default:
		return false;
	}
}

static uint32_t rk3506_cru_pll_rate(mm_reg_t base, uint32_t offset)
{
	uint32_t con0 = sys_read32(base + offset);
	uint32_t con1 = sys_read32(base + offset + 4U);
	uint32_t con2 = sys_read32(base + offset + 8U);
	uint32_t fbdiv = con0 & 0xfffU;
	uint32_t postdiv1 = (con0 >> 12) & 0x7U;
	uint32_t refdiv = con1 & 0x3fU;
	uint32_t postdiv2 = (con1 >> 6) & 0x7U;
	bool dsmpd = (con1 & BIT(12)) != 0U;
	uint64_t numerator = 24000000ULL * fbdiv;
	uint64_t denominator = refdiv;
	uint64_t postdiv;

	if (refdiv == 0U || postdiv1 == 0U || postdiv2 == 0U) {
		return 0U;
	}

	if (!dsmpd) {
		numerator = numerator * (1ULL << 24U) +
			    24000000ULL * (con2 & 0xffffffU);
		denominator *= 1ULL << 24U;
	}
	postdiv = (uint64_t)postdiv1 * postdiv2;
	denominator *= postdiv;

	return (uint32_t)((numerator + denominator / 2U) / denominator);
}

static int rk3506_cru_get_rate_id(mm_reg_t base, uint16_t id, uint32_t *rate)
{
	const struct rk3506_cru_clk *clk;
	uint32_t parent_rate;
	uint32_t value = 0U;
	uint32_t sel = 0U;
	uint32_t div = 1U;
	bool value_read = false;
	int ret;

	switch (id) {
	case RK3506_CRU_XIN24M:
		*rate = 24000000U;
		return 0;
	case RK3506_CRU_SARADC_400K:
		*rate = 400000U;
		return 0;
	case RK3506_CRU_SARADC_32K:
		*rate = 32000U;
		return 0;
	case PLL_GPLL:
		*rate = rk3506_cru_pll_rate(base, RK3506_CRU_PLL_CON(0));
		return 0;
	case PLL_V0PLL:
		*rate = rk3506_cru_pll_rate(base, RK3506_CRU_PLL_CON(8));
		return 0;
	case PLL_V1PLL:
		*rate = rk3506_cru_pll_rate(base, RK3506_CRU_PLL_CON(16));
		return 0;
	default:
		break;
	}

	clk = rk3506_cru_find_clk(id);
	if (clk == NULL || clk->parent_count == 0U) {
		return -ENOTSUP;
	}

	if (clk->sel_width != 0U || clk->div_width != 0U) {
		uint32_t offset = clk->pmu ? RK3506_CRU_PMU_CLKSEL_CON(clk->reg) :
					    RK3506_CRU_CLKSEL_CON(clk->reg);

		value = sys_read32(base + offset);
		value_read = true;
	}

	if (clk->sel_width != 0U) {
		sel = (value >> clk->sel_shift) & BIT_MASK(clk->sel_width);
		if (sel >= clk->parent_count) {
			return -ENOTSUP;
		}
	}

	ret = rk3506_cru_get_rate_id(base, clk->parents[sel], &parent_rate);
	if (ret != 0) {
		return ret;
	}

	if (clk->fractional) {
		uint32_t div_offset =
			clk->pmu ? RK3506_CRU_PMU_CLKSEL_CON(clk->reg) :
				   RK3506_CRU_CLKSEL_CON(clk->reg);
		uint32_t div_value = sys_read32(base + div_offset);
		uint64_t numerator = (div_value >> 16U) & 0xffffU;
		uint64_t denominator = div_value & 0xffffU;

		if (clk->frac_high_reg != 0U) {
			uint32_t high_offset =
				clk->pmu ?
				RK3506_CRU_PMU_CLKSEL_CON(clk->frac_high_reg) :
				RK3506_CRU_CLKSEL_CON(clk->frac_high_reg);
			uint32_t high = sys_read32(base + high_offset);

			numerator |= ((high >> 16U) & 0xffU) << 16U;
			denominator |= (high & 0xffU) << 16U;
		}

		if (numerator == 0U || denominator == 0U) {
			return -ENOTSUP;
		}

		*rate = (uint32_t)(((uint64_t)parent_rate * numerator +
				   denominator / 2U) / denominator);
		return 0;
	}

	if (clk->div_width != 0U && value_read) {
		div = ((value >> clk->div_shift) & BIT_MASK(clk->div_width)) + 1U;
	}

	*rate = parent_rate / div;
	return 0;
}

static void rk3506_cru_gate_write(const struct device *dev,
				  const struct rk3506_cru_gate *gate, bool enable)
{
	mm_reg_t base = DEVICE_MMIO_GET(dev);
	uint32_t offset = gate->pmu ? RK3506_CRU_PMU_CLKGATE_CON(gate->reg) :
				      RK3506_CRU_CLKGATE_CON(gate->reg);
	k_spinlock_key_t key = k_spin_lock(&RK3506_CRU_DATA(dev)->reg_lock);
	uint32_t reg = sys_read32(base + offset);
	uint32_t val;

	if (enable) {
		val = (BIT(gate->bit) << 16) | (reg & ~BIT(gate->bit));
	} else {
		val = (BIT(gate->bit) << 16) | (reg | BIT(gate->bit));
	}

	sys_write32(val, base + offset);
	k_spin_unlock(&RK3506_CRU_DATA(dev)->reg_lock, key);
}

static int rk3506_cru_clock_on(const struct device *dev, clock_control_subsys_t sys)
{
	const struct rk3506_cru_gate *gate = rk3506_cru_find_gate((uint16_t)(uintptr_t)sys);

	if (gate == NULL) {
		return -ENOTSUP;
	}

	rk3506_cru_gate_write(dev, gate, true);
	return 0;
}

static int rk3506_cru_clock_off(const struct device *dev, clock_control_subsys_t sys)
{
	const struct rk3506_cru_gate *gate = rk3506_cru_find_gate((uint16_t)(uintptr_t)sys);

	if (gate == NULL) {
		return -ENOTSUP;
	}

	rk3506_cru_gate_write(dev, gate, false);
	return 0;
}

static enum clock_control_status rk3506_cru_clock_get_status(const struct device *dev,
							 clock_control_subsys_t sys)
{
	mm_reg_t base = DEVICE_MMIO_GET(dev);
	const struct rk3506_cru_gate *gate = rk3506_cru_find_gate((uint16_t)(uintptr_t)sys);
	uint32_t offset;

	if (gate == NULL) {
		return CLOCK_CONTROL_STATUS_UNKNOWN;
	}

	offset = gate->pmu ? RK3506_CRU_PMU_CLKGATE_CON(gate->reg) :
			     RK3506_CRU_CLKGATE_CON(gate->reg);

	return (sys_read32(base + offset) & BIT(gate->bit)) != 0U ?
	       CLOCK_CONTROL_STATUS_OFF : CLOCK_CONTROL_STATUS_ON;
}

static int rk3506_cru_clock_get_rate(const struct device *dev, clock_control_subsys_t sys,
				 uint32_t *rate)
{
	mm_reg_t base = DEVICE_MMIO_GET(dev);
	uint16_t id = (uint16_t)(uintptr_t)sys;

	return rk3506_cru_get_rate_id(base, id, rate);
}

static int rk3506_cru_clock_set_rate_locked(const struct device *dev,
					    clock_control_subsys_t sys,
					    clock_control_subsys_rate_t rate_ptr)
{
	mm_reg_t base = DEVICE_MMIO_GET(dev);
	uint16_t id = (uint16_t)(uintptr_t)sys;
	uint32_t rate = (uint32_t)(uintptr_t)rate_ptr;
	const struct rk3506_cru_clk *clk;
	uint32_t offset;
	uint32_t parent_rate;
	uint32_t field_mask;
	uint32_t value;
	uint32_t div;
	uint32_t best_sel;
	uint32_t best_div;
	uint32_t best_error;
	uint32_t max_sel;
	uint32_t max_div;
	int ret;

	if (rate == 0U) {
		return -EINVAL;
	}

#if defined(CONFIG_CLOCK_CONTROL_ROCKCHIP_RK3506_RUNTIME_PLL)
	if (id == PLL_V0PLL) {
		return rk3506_cru_set_v0pll_rate(base, rate);
	}
#endif

	if (!rk3506_cru_can_set_rate(id)) {
		return -ENOTSUP;
	}

	clk = rk3506_cru_find_clk(id);
	if (clk == NULL || clk->div_width == 0U || clk->parent_count == 0U) {
		return -ENOTSUP;
	}

	offset = clk->pmu ? RK3506_CRU_PMU_CLKSEL_CON(clk->reg) :
			    RK3506_CRU_CLKSEL_CON(clk->reg);
	value = sys_read32(base + offset);
	max_sel = clk->parent_count;
	max_div = BIT(clk->div_width);
	best_sel = 0U;
	best_div = 1U;
	best_error = UINT32_MAX;

	for (uint32_t candidate_sel = 0U; candidate_sel < max_sel; candidate_sel++) {
		ret = rk3506_cru_get_rate_id(base, clk->parents[candidate_sel], &parent_rate);
		if (ret != 0 || parent_rate == 0U || rate > parent_rate) {
			continue;
		}

		div = parent_rate / rate;
		for (uint32_t candidate_div = div; candidate_div <= div + 1U; candidate_div++) {
			uint32_t actual;
			uint32_t error;

			if (candidate_div == 0U || candidate_div > max_div) {
				continue;
			}

			actual = parent_rate / candidate_div;
			error = actual > rate ? actual - rate : rate - actual;

			if (error < best_error) {
				best_error = error;
				best_sel = candidate_sel;
				best_div = candidate_div;
			}
		}
	}

	if (best_error == UINT32_MAX) {
		return -EINVAL;
	}

	field_mask = (BIT_MASK(clk->div_width) << clk->div_shift) |
		     (clk->sel_width == 0U ? 0U :
		      (BIT_MASK(clk->sel_width) << clk->sel_shift));
	value &= ~field_mask;
	value |= (best_div - 1U) << clk->div_shift;
	value |= best_sel << clk->sel_shift;
	sys_write32(value | (field_mask << 16), base + offset);

	return rk3506_cru_clock_get_rate(dev, sys, &rate);
}

static int rk3506_cru_clock_set_rate(const struct device *dev, clock_control_subsys_t sys,
				     clock_control_subsys_rate_t rate_ptr)
{
	struct rk3506_cru_data *data = RK3506_CRU_DATA(dev);
	int ret;

	k_mutex_lock(&data->set_rate_lock, K_FOREVER);
	ret = rk3506_cru_clock_set_rate_locked(dev, sys, rate_ptr);
	k_mutex_unlock(&data->set_rate_lock);

	return ret;
}

static DEVICE_API(clock_control, rk3506_cru_clock_api) = {
	.on = rk3506_cru_clock_on,
	.off = rk3506_cru_clock_off,
	.get_rate = rk3506_cru_clock_get_rate,
	.set_rate = rk3506_cru_clock_set_rate,
	.get_status = rk3506_cru_clock_get_status,
};

static int rk3506_cru_clock_init(const struct device *dev)
{
	struct rk3506_cru_data *data = RK3506_CRU_DATA(dev);

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	k_mutex_init(&data->set_rate_lock);

	return 0;
}

#define RK3506_CRU_CLOCK_DEFINE(n)						\
	static struct rk3506_cru_data rk3506_cru_clock_data_##n;		\
	static const struct rk3506_cru_config					\
		rk3506_cru_clock_config_##n = {					\
		DEVICE_MMIO_ROM_INIT(DT_INST_PARENT(n)),			\
	};									\
	DEVICE_DT_INST_DEFINE(n, rk3506_cru_clock_init, NULL,			\
			      &rk3506_cru_clock_data_##n,			\
			      &rk3506_cru_clock_config_##n, PRE_KERNEL_1,	\
			      CONFIG_CLOCK_CONTROL_INIT_PRIORITY,		\
			      &rk3506_cru_clock_api);

DT_INST_FOREACH_STATUS_OKAY(RK3506_CRU_CLOCK_DEFINE)
