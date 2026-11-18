/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk3506_tsadc

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(rk3506_tsadc, CONFIG_SENSOR_LOG_LEVEL);

#define TSADCV2_AUTO_CON			0x04U
#define TSADCV2_INT_EN				0x08U
#define TSADCV3_AUTO_SRC_CON			0x0cU
#define TSADCV3_HSHUT_GPIO_INT_EN		0x18U
#define TSADCV3_HSHUT_CRU_INT_EN		0x1cU
#define TSADCV3_INT_PD				0x24U
#define TSADCV3_DATA(chn)			(0x2cU + ((chn) * 4U))
#define TSADCV3_COMP_SHUT(chn)			(0x10cU + ((chn) * 4U))
#define TSADCV3_AUTO_PERIOD			0x154U
#define TSADCV3_AUTO_PERIOD_HT			0x158U
#define TSADCV3_HIGHT_INT_DEBOUNCE		0x14cU
#define TSADCV3_HIGHT_TSHUT_DEBOUNCE		0x150U
#define TSADCV9_Q_MAX				0x210U

#define TSADCV2_AUTO_EN				BIT(0)
#define TSADCV2_AUTO_EN_MASK			BIT(16)
#define TSADCV3_AUTO_Q_SEL_EN			BIT(1)
#define TSADCV3_AUTO_Q_SEL_EN_MASK		BIT(17)
#define TSADCV3_AUTO_SRC_EN(chn)		BIT(chn)
#define TSADCV3_AUTO_SRC_EN_MASK(chn)		BIT(16 + (chn))
#define TSADCV2_AUTO_TSHUT_POLARITY_HIGH	BIT(8)
#define TSADCV2_AUTO_TSHUT_POLARITY_MASK	BIT(24)
#define TSADCV2_SHUT_2GPIO_SRC_EN(chn)		BIT(4 + (chn))
#define TSADCV2_SHUT_2CRU_SRC_EN(chn)		BIT(8 + (chn))

#define RK3506_GRF_TSADC_CON			0x10U
#define RV1106_VOGRF_TSADC_TSEN			(0x10001U << 8)
#define RV1106_VOGRF_TSADC_ANA			(0xff0007U << 0)

#define OTPC_SBPI_CTRL				0x0020U
#define OTPC_SBPI_CMD_VALID_PRE			0x0024U
#define OTPC_SBPI_STATUS			0x002cU
#define OTPC_USER_CTRL				0x0100U
#define OTPC_USER_ADDR				0x0104U
#define OTPC_USER_ENABLE			0x0108U
#define OTPC_USER_Q				0x0124U
#define OTPC_INT_STATUS				0x0304U
#define OTPC_SBPI_CMD0_OFFSET			0x1000U
#define OTPC_SBPI_CMD1_OFFSET			0x1004U

#define OTPC_USER_ADDR_MASK			GENMASK(31, 16)
#define OTPC_USE_USER				BIT(0)
#define OTPC_USE_USER_MASK			GENMASK(16, 16)
#define OTPC_USER_FSM_ENABLE			BIT(0)
#define OTPC_USER_FSM_ENABLE_MASK		GENMASK(16, 16)
#define OTPC_SBPI_DONE				BIT(1)
#define OTPC_USER_DONE				BIT(2)
#define OTPC_SBPI_DAP_ADDR			0x02U
#define OTPC_SBPI_DAP_ADDR_SHIFT		8U
#define OTPC_SBPI_DAP_ADDR_MASK			GENMASK(31, 24)
#define OTPC_SBPI_CMD_VALID_MASK			GENMASK(31, 16)
#define OTPC_SBPI_DAP_CMD_WRF			0xc0U
#define OTPC_SBPI_DAP_REG_ECC			0x3aU
#define OTPC_SBPI_ECC_DISABLE			0x09U
#define OTPC_SBPI_ENABLE			BIT(0)
#define OTPC_SBPI_ENABLE_MASK			GENMASK(16, 16)

#define RK3506_TSADC_DATA_MASK			0x3ffU
#define RK3506_TSADC_TRIM_BLOCK			0x10U
#define RK3506_TSADC_TRIM_BASE_MDEG		30000
#define RK3506_TSADC_TRIM_SLOPE			594
#define RK3506_TSADC_MIN_MDEG			(-60000)
#define RK3506_TSADC_MAX_MDEG			180000

struct rk3506_tsadc_table_entry {
	uint16_t code;
	int32_t temp_mdeg;
};

static const struct rk3506_tsadc_table_entry rk3506_tsadc_table[] = {
	{0, RK3506_TSADC_MIN_MDEG},
	{362, RK3506_TSADC_MIN_MDEG},
	{395, -40000},
	{503, 25000},
	{528, 40000},
	{679, 125000},
	{779, RK3506_TSADC_MAX_MDEG},
	{0x3ff, RK3506_TSADC_MAX_MDEG},
};

struct rk3506_tsadc_config {
	DEVICE_MMIO_NAMED_ROM(tsadc);
	DEVICE_MMIO_NAMED_ROM(grf);
	DEVICE_MMIO_NAMED_ROM(otp);
	const struct device *clock_dev;
	clock_control_subsys_t tsadc_clk;
	clock_control_subsys_t tsadc_pclk;
	clock_control_subsys_t tsadc_tsen_clk;
	clock_control_subsys_t otp_pclk;
	clock_control_subsys_t otp_sbp_clk;
	int32_t tshut_temp_mdeg;
};

struct rk3506_tsadc_data {
	DEVICE_MMIO_NAMED_RAM(tsadc);
	DEVICE_MMIO_NAMED_RAM(grf);
	DEVICE_MMIO_NAMED_RAM(otp);
	struct k_mutex lock;
	int32_t temp_mdeg;
	int32_t trim_mdeg;
};

#define DEV_CFG(dev) ((const struct rk3506_tsadc_config *)((dev)->config))
#define DEV_DATA(dev) ((struct rk3506_tsadc_data *)((dev)->data))

#define TSADC_MMIO(dev) DEVICE_MMIO_NAMED_GET(dev, tsadc)
#define TSADC_GRF_MMIO(dev) DEVICE_MMIO_NAMED_GET(dev, grf)
#define TSADC_OTP_MMIO(dev) DEVICE_MMIO_NAMED_GET(dev, otp)

static int rk3506_tsadc_code_to_temp(uint32_t code, int32_t *temp_mdeg)
{
	code &= RK3506_TSADC_DATA_MASK;

	for (size_t i = 1U; i < ARRAY_SIZE(rk3506_tsadc_table); i++) {
		const struct rk3506_tsadc_table_entry *low = &rk3506_tsadc_table[i - 1U];
		const struct rk3506_tsadc_table_entry *high = &rk3506_tsadc_table[i];
		int64_t delta;

		if (code > high->code) {
			continue;
		}

		delta = (int64_t)(code - low->code) * (high->temp_mdeg - low->temp_mdeg);
		delta /= high->code - low->code;
		*temp_mdeg = low->temp_mdeg + delta;
		return 0;
	}

	return -ERANGE;
}

static int rk3506_tsadc_temp_to_code(int32_t temp_mdeg, uint32_t *code)
{
	if (temp_mdeg <= rk3506_tsadc_table[0].temp_mdeg) {
		*code = rk3506_tsadc_table[0].code;
		return 0;
	}

	for (size_t i = 1U; i < ARRAY_SIZE(rk3506_tsadc_table); i++) {
		const struct rk3506_tsadc_table_entry *low = &rk3506_tsadc_table[i - 1U];
		const struct rk3506_tsadc_table_entry *high = &rk3506_tsadc_table[i];
		int64_t num;

		if (temp_mdeg > high->temp_mdeg) {
			continue;
		}

		num = (int64_t)(temp_mdeg - low->temp_mdeg) * (high->code - low->code);
		*code = low->code + DIV_ROUND_UP(num, high->temp_mdeg - low->temp_mdeg);
		return 0;
	}

	return -ERANGE;
}

static int rk3506_tsadc_otp_poll(uintptr_t base, uint32_t mask)
{
	for (uint32_t i = 0U; i < 10000U; i++) {
		if ((sys_read32(base + OTPC_INT_STATUS) & mask) != 0U) {
			sys_write32(mask, base + OTPC_INT_STATUS);
			return 0;
		}
		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

static int rk3506_tsadc_otp_read_trim(uintptr_t base, uint16_t *trim)
{
	int ret;

	sys_write32(OTPC_SBPI_DAP_ADDR_MASK |
		    (OTPC_SBPI_DAP_ADDR << OTPC_SBPI_DAP_ADDR_SHIFT),
		    base + OTPC_SBPI_CTRL);
	sys_write32(OTPC_SBPI_CMD_VALID_MASK | 0x1U, base + OTPC_SBPI_CMD_VALID_PRE);
	sys_write32(OTPC_SBPI_DAP_CMD_WRF | OTPC_SBPI_DAP_REG_ECC,
		    base + OTPC_SBPI_CMD0_OFFSET);
	sys_write32(OTPC_SBPI_ECC_DISABLE, base + OTPC_SBPI_CMD1_OFFSET);
	sys_write32(OTPC_SBPI_ENABLE_MASK | OTPC_SBPI_ENABLE, base + OTPC_SBPI_CTRL);

	ret = rk3506_tsadc_otp_poll(base, OTPC_SBPI_DONE);
	if (ret != 0) {
		return ret;
	}

	sys_write32(OTPC_USE_USER | OTPC_USE_USER_MASK, base + OTPC_USER_CTRL);
	k_busy_wait(5);
	sys_write32(OTPC_USER_ADDR_MASK | RK3506_TSADC_TRIM_BLOCK, base + OTPC_USER_ADDR);
	sys_write32(OTPC_USER_FSM_ENABLE | OTPC_USER_FSM_ENABLE_MASK,
		    base + OTPC_USER_ENABLE);

	ret = rk3506_tsadc_otp_poll(base, OTPC_USER_DONE);
	if (ret == 0) {
		*trim = sys_read32(base + OTPC_USER_Q) & 0xffffU;
	}

	sys_write32(OTPC_USE_USER_MASK, base + OTPC_USER_CTRL);
	return ret;
}

static int rk3506_tsadc_load_trim(const struct device *dev, struct rk3506_tsadc_data *data)
{
	uint16_t trim = 0U;
	uint32_t base_code;
	int32_t trim_code;
	int ret;

	ret = rk3506_tsadc_otp_read_trim(TSADC_OTP_MMIO(dev), &trim);
	if (ret != 0) {
		LOG_WRN("TSADC trim read failed: %d", ret);
		return ret;
	}

	if (trim == 0U) {
		LOG_WRN("TSADC trim is zero; using untrimmed table");
		return 0;
	}

	ret = rk3506_tsadc_temp_to_code(RK3506_TSADC_TRIM_BASE_MDEG, &base_code);
	if (ret != 0) {
		return ret;
	}

	trim_code = (int32_t)trim - (int32_t)base_code;
	data->trim_mdeg = RK3506_TSADC_TRIM_SLOPE * trim_code;
	LOG_INF("TSADC trim: raw=0x%04x code_delta=%d correction=%d mdeg", trim,
		trim_code, data->trim_mdeg);
	return 0;
}

static int rk3506_tsadc_configure_hw_tshut(const struct device *dev,
					   const struct rk3506_tsadc_data *data)
{
	int32_t tshut_temp = DEV_CFG(dev)->tshut_temp_mdeg + data->trim_mdeg;
	uint32_t tshut_code;
	int ret;

	ret = rk3506_tsadc_temp_to_code(tshut_temp, &tshut_code);
	if (ret != 0) {
		LOG_ERR("Invalid TSADC tshut temperature %d", tshut_temp);
		return ret;
	}

	sys_write32(tshut_code, TSADC_MMIO(dev) + TSADCV3_COMP_SHUT(0));
	sys_write32(TSADCV3_AUTO_SRC_EN(0) | TSADCV3_AUTO_SRC_EN_MASK(0),
		    TSADC_MMIO(dev) + TSADCV3_AUTO_SRC_CON);
	sys_write32(TSADCV2_SHUT_2GPIO_SRC_EN(0) | TSADCV2_SHUT_2GPIO_SRC_EN(0) << 16,
		    TSADC_MMIO(dev) + TSADCV3_HSHUT_GPIO_INT_EN);
	sys_write32(TSADCV2_SHUT_2CRU_SRC_EN(0) | TSADCV2_SHUT_2CRU_SRC_EN(0) << 16,
		    TSADC_MMIO(dev) + TSADCV3_HSHUT_CRU_INT_EN);

	LOG_INF("TSADC hw tshut: %d mdeg, code=%u", tshut_temp, tshut_code);
	return 0;
}

static int rk3506_tsadc_init(const struct device *dev)
{
	const struct rk3506_tsadc_config *cfg = DEV_CFG(dev);
	struct rk3506_tsadc_data *data = DEV_DATA(dev);
	uint32_t auto_con;
	int ret;

	DEVICE_MMIO_NAMED_MAP(dev, tsadc, K_MEM_CACHE_NONE);
	DEVICE_MMIO_NAMED_MAP(dev, grf, K_MEM_CACHE_NONE);
	DEVICE_MMIO_NAMED_MAP(dev, otp, K_MEM_CACHE_NONE);

	k_mutex_init(&data->lock);

	if (!device_is_ready(cfg->clock_dev)) {
		LOG_ERR_DEVICE_NOT_READY(cfg->clock_dev);
		return -ENODEV;
	}

	ret = clock_control_set_rate(cfg->clock_dev, cfg->tsadc_clk,
				     (clock_control_subsys_rate_t)(uintptr_t)1000000U);
	if (ret != 0) {
		LOG_ERR("failed to set TSADC clock: %d", ret);
		return ret;
	}

	ret = clock_control_set_rate(cfg->clock_dev, cfg->tsadc_tsen_clk,
				     (clock_control_subsys_rate_t)(uintptr_t)12000000U);
	if (ret != 0) {
		LOG_ERR("failed to set TSEN clock: %d", ret);
		return ret;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->tsadc_pclk);
	if (ret != 0) {
		LOG_ERR("failed to enable TSADC PCLK: %d", ret);
		return ret;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->tsadc_clk);
	if (ret != 0) {
		LOG_ERR("failed to enable TSADC clock: %d", ret);
		return ret;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->tsadc_tsen_clk);
	if (ret != 0) {
		LOG_ERR("failed to enable TSEN clock: %d", ret);
		return ret;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->otp_pclk);
	if (ret != 0) {
		LOG_ERR("failed to enable OTP PCLK: %d", ret);
		return ret;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->otp_sbp_clk);
	if (ret != 0) {
		LOG_ERR("failed to enable OTP SBPI clock: %d", ret);
		return ret;
	}

	sys_write32(RV1106_VOGRF_TSADC_TSEN, TSADC_GRF_MMIO(dev) + RK3506_GRF_TSADC_CON);
	k_busy_wait(10);
	sys_write32(RV1106_VOGRF_TSADC_ANA, TSADC_GRF_MMIO(dev) + RK3506_GRF_TSADC_CON);
	k_busy_wait(100);

	sys_write32(2500U, TSADC_MMIO(dev) + TSADCV3_AUTO_PERIOD);
	sys_write32(2500U, TSADC_MMIO(dev) + TSADCV3_AUTO_PERIOD_HT);
	sys_write32(4U, TSADC_MMIO(dev) + TSADCV3_HIGHT_INT_DEBOUNCE);
	sys_write32(4U, TSADC_MMIO(dev) + TSADCV3_HIGHT_TSHUT_DEBOUNCE);
	sys_write32(0x3ffU, TSADC_MMIO(dev) + TSADCV9_Q_MAX);

	ret = rk3506_tsadc_load_trim(dev, data);
	if (ret != 0) {
		data->trim_mdeg = 0;
	}

	ret = rk3506_tsadc_configure_hw_tshut(dev, data);
	if (ret != 0) {
		return ret;
	}

	auto_con = TSADCV2_AUTO_TSHUT_POLARITY_MASK |
		   TSADCV3_AUTO_Q_SEL_EN_MASK | TSADCV3_AUTO_Q_SEL_EN |
		   TSADCV2_AUTO_EN_MASK | TSADCV2_AUTO_EN;
	sys_write32(auto_con, TSADC_MMIO(dev) + TSADCV2_AUTO_CON);
	k_busy_wait(5000);

	return 0;
}

static int rk3506_tsadc_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct rk3506_tsadc_data *data = DEV_DATA(dev);
	uint32_t code;
	int32_t temp_mdeg;
	int ret;

	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_DIE_TEMP) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	code = sys_read32(TSADC_MMIO(dev) + TSADCV3_DATA(0)) & RK3506_TSADC_DATA_MASK;
	ret = rk3506_tsadc_code_to_temp(code, &temp_mdeg);
	if (ret == 0) {
		data->temp_mdeg = temp_mdeg - data->trim_mdeg;
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int rk3506_tsadc_channel_get(const struct device *dev, enum sensor_channel chan,
				    struct sensor_value *val)
{
	struct rk3506_tsadc_data *data = DEV_DATA(dev);
	int32_t temp_mdeg;

	if (chan != SENSOR_CHAN_DIE_TEMP) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	temp_mdeg = data->temp_mdeg;
	k_mutex_unlock(&data->lock);

	val->val1 = temp_mdeg / 1000;
	val->val2 = (temp_mdeg % 1000) * 1000;
	return 0;
}

static DEVICE_API(sensor, rk3506_tsadc_api) = {
	.sample_fetch = rk3506_tsadc_sample_fetch,
	.channel_get = rk3506_tsadc_channel_get,
};

#define RK3506_TSADC_DEFINE(inst)                                                       \
	static struct rk3506_tsadc_data rk3506_tsadc_data_##inst;                        \
	static const struct rk3506_tsadc_config rk3506_tsadc_config_##inst = {            \
		DEVICE_MMIO_NAMED_ROM_INIT(tsadc, DT_DRV_INST(inst)),                        \
		DEVICE_MMIO_NAMED_ROM_INIT(grf, DT_INST_PHANDLE(inst, rockchip_grf)),        \
		DEVICE_MMIO_NAMED_ROM_INIT(otp, DT_INST_PHANDLE(inst, rockchip_otp)),        \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(inst, tsadc)),        \
		.tsadc_clk = (clock_control_subsys_t)(uintptr_t)                             \
			DT_INST_CLOCKS_CELL_BY_NAME(inst, tsadc, clkid),                     \
		.tsadc_pclk = (clock_control_subsys_t)(uintptr_t)                            \
			DT_INST_CLOCKS_CELL_BY_NAME(inst, apb_pclk, clkid),                  \
		.tsadc_tsen_clk = (clock_control_subsys_t)(uintptr_t)                         \
			DT_INST_CLOCKS_CELL_BY_NAME(inst, tsen, clkid),                      \
		.otp_pclk = (clock_control_subsys_t)(uintptr_t)                              \
			DT_INST_CLOCKS_CELL_BY_NAME(inst, otp_pclk, clkid),                  \
		.otp_sbp_clk = (clock_control_subsys_t)(uintptr_t)                           \
			DT_INST_CLOCKS_CELL_BY_NAME(inst, otp_sbp, clkid),                   \
		.tshut_temp_mdeg = DT_INST_PROP(inst, rockchip_hw_tshut_temp),                \
	};                                                                                  \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, rk3506_tsadc_init, NULL,                       \
				     &rk3506_tsadc_data_##inst,                           \
				     &rk3506_tsadc_config_##inst, POST_KERNEL,             \
				     CONFIG_SENSOR_INIT_PRIORITY, &rk3506_tsadc_api);

DT_INST_FOREACH_STATUS_OKAY(RK3506_TSADC_DEFINE)
