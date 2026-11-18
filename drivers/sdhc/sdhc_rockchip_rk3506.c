/*
 * Copyright (c) 2026 Jiaming Zhao
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk3506_dw_mshc

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/internal/mm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "sdhc_common.h"

LOG_MODULE_REGISTER(sdhc_rockchip_rk3506, CONFIG_SDHC_LOG_LEVEL);

#define RK3506_SDHC_CTRL    0x000U
#define RK3506_SDHC_PWREN   0x004U
#define RK3506_SDHC_CLKDIV  0x008U
#define RK3506_SDHC_CLKSRC  0x00cU
#define RK3506_SDHC_CLKENA  0x010U
#define RK3506_SDHC_TMOUT   0x014U
#define RK3506_SDHC_CTYPE   0x018U
#define RK3506_SDHC_BLKSIZ  0x01cU
#define RK3506_SDHC_BYTCNT  0x020U
#define RK3506_SDHC_INTMASK 0x024U
#define RK3506_SDHC_CMDARG  0x028U
#define RK3506_SDHC_CMD     0x02cU
#define RK3506_SDHC_RESP0   0x030U
#define RK3506_SDHC_RESP1   0x034U
#define RK3506_SDHC_RESP2   0x038U
#define RK3506_SDHC_RESP3   0x03cU
#define RK3506_SDHC_RINTSTS 0x044U
#define RK3506_SDHC_STATUS  0x048U
#define RK3506_SDHC_FIFOTH  0x04cU
#define RK3506_SDHC_CDETECT 0x050U
#define RK3506_SDHC_BMOD    0x080U
#define RK3506_SDHC_PLDMND  0x084U
#define RK3506_SDHC_DBADDR  0x088U
#define RK3506_SDHC_IDSTS   0x08cU
#define RK3506_SDHC_IDINTEN 0x090U
#define RK3506_SDHC_DSCADDR 0x094U
#define RK3506_SDHC_BUFADDR 0x098U
#define RK3506_SDHC_DATA    0x200U

#define RK3506_SDHC_CTRL_RESET      BIT(0)
#define RK3506_SDHC_CTRL_FIFO_RESET BIT(1)
#define RK3506_SDHC_CTRL_DMA_RESET  BIT(2)
#define RK3506_SDHC_CTRL_IDMAC      BIT(25)

#define RK3506_SDHC_CMD_RESP_EXP     BIT(6)
#define RK3506_SDHC_CMD_RESP_LENGTH  BIT(7)
#define RK3506_SDHC_CMD_CHECK_CRC    BIT(8)
#define RK3506_SDHC_CMD_DATA_EXP     BIT(9)
#define RK3506_SDHC_CMD_RW           BIT(10)
#define RK3506_SDHC_CMD_SEND_STOP    BIT(12)
#define RK3506_SDHC_CMD_ABORT_STOP   BIT(14)
#define RK3506_SDHC_CMD_PRV_DAT_WAIT BIT(13)
#define RK3506_SDHC_CMD_UPD_CLK      BIT(21)
#define RK3506_SDHC_CMD_USE_HOLD_REG BIT(29)
#define RK3506_SDHC_CMD_START        BIT(31)

#define RK3506_SDHC_CLKEN_ENABLE  BIT(0)
#define RK3506_SDHC_CLKEN_LOW_PWR BIT(16)
#define RK3506_SDHC_CLKEN_ACTIVE                                                               \
	(RK3506_SDHC_CLKEN_ENABLE |                                                            \
	 (IS_ENABLED(CONFIG_SDHC_ROCKCHIP_RK3506_CLOCK_LOW_POWER) ?                            \
		  RK3506_SDHC_CLKEN_LOW_PWR : 0U))

#define RK3506_SDHC_CTYPE_4BIT BIT(0)
#define RK3506_SDHC_CTYPE_8BIT BIT(16)

#define RK3506_SDHC_INT_RE    BIT(1)
#define RK3506_SDHC_INT_CDONE BIT(2)
#define RK3506_SDHC_INT_RCRC  BIT(6)
#define RK3506_SDHC_INT_DCRC  BIT(7)
#define RK3506_SDHC_INT_RTO   BIT(8)
#define RK3506_SDHC_INT_DRTO  BIT(9)
#define RK3506_SDHC_INT_HTO   BIT(10)
#define RK3506_SDHC_INT_FRUN  BIT(11)
#define RK3506_SDHC_INT_HLE   BIT(12)
#define RK3506_SDHC_INT_SBE   BIT(13)
#define RK3506_SDHC_INT_EBE   BIT(15)
#define RK3506_SDHC_INT_RXDR  BIT(5)
#define RK3506_SDHC_INT_TXDR  BIT(4)
#define RK3506_SDHC_INT_DTO   BIT(3)

#define RK3506_SDHC_DATA_ERR                                                                       \
	(RK3506_SDHC_INT_DCRC | RK3506_SDHC_INT_FRUN | RK3506_SDHC_INT_HLE | RK3506_SDHC_INT_SBE | \
	 RK3506_SDHC_INT_EBE)
#define RK3506_SDHC_DATA_TOUT (RK3506_SDHC_INT_DRTO | RK3506_SDHC_INT_HTO)
#define RK3506_SDHC_ALL_INTS  GENMASK(15, 0)

#define RK3506_SDHC_STATUS_BUSY      BIT(9)
#define RK3506_SDHC_FIFO_COUNT_MASK  0x1fffU
#define RK3506_SDHC_FIFO_COUNT_SHIFT 17U

#define RK3506_SDHC_FIFOTH_MSIZE(x)    ((x) << 28U)
#define RK3506_SDHC_FIFOTH_RX_WMARK(x) ((x) << 16U)
#define RK3506_SDHC_FIFOTH_TX_WMARK(x) (x)

#define RK3506_SDHC_BMOD_IDMAC_RESET        BIT(0)
#define RK3506_SDHC_BMOD_FIXED_BURST        BIT(1)
#define RK3506_SDHC_BMOD_DMA_ENABLE         BIT(7)

#define RK3506_SDHC_DESC_OWN BIT(31)
#define RK3506_SDHC_DESC_DIC BIT(1)
#define RK3506_SDHC_DESC_CH  BIT(4)
#define RK3506_SDHC_DESC_FS  BIT(3)
#define RK3506_SDHC_DESC_LD  BIT(2)

#define RK3506_SDHC_IDSTS_TI   BIT(0)
#define RK3506_SDHC_IDSTS_RI   BIT(1)
#define RK3506_SDHC_IDSTS_FBE  BIT(2)
#define RK3506_SDHC_IDSTS_DU   BIT(4)
#define RK3506_SDHC_IDSTS_CES  BIT(5)
#define RK3506_SDHC_IDSTS_NIS  BIT(8)
#define RK3506_SDHC_IDSTS_AIS  BIT(9)
#define RK3506_SDHC_IDSTS_ALL  GENMASK(9, 0)
#define RK3506_SDHC_IDINTEN_COMPLETE (RK3506_SDHC_IDSTS_TI | RK3506_SDHC_IDSTS_RI | \
				      RK3506_SDHC_IDSTS_NIS)

#define RK3506_SDHC_DMA_MAX_DESC  CONFIG_SDHC_ROCKCHIP_RK3506_DMA_MAX_DESC
#define RK3506_SDHC_DMA_CHUNK_SIZE 4096U
#define RK3506_SDHC_DMA_MAX_BYTES                                                              \
	((size_t)RK3506_SDHC_DMA_MAX_DESC * RK3506_SDHC_DMA_CHUNK_SIZE)
#define RK3506_SDHC_RESET_TIMEOUT_US        100000U
#define RK3506_SDHC_BUSY_TIMEOUT_MS         10000U
#define RK3506_SDHC_POLL_DELAY_US           10U
#define RK3506_SDHC_DEFAULT_CMD_TIMEOUT_MS  200U
#define RK3506_SDHC_DEFAULT_DATA_TIMEOUT_MS 10000U

struct rk3506_sdhc_dma_desc {
	uint32_t des0;
	uint32_t des1;
	uint32_t des2;
	uint32_t des3;
} __aligned(CONFIG_SDHC_BUFFER_ALIGNMENT);

struct rk3506_sdhc_config {
	DEVICE_MMIO_ROM;
	const struct device *clock_dev;
	clock_control_subsys_t biu_clock;
	clock_control_subsys_t ciu_clock;
	struct reset_dt_spec reset;
	const struct pinctrl_dev_config *pcfg;
	struct sdhc_common_config common;
	uint32_t fifo_depth;
	bool non_removable;
};

struct rk3506_sdhc_data {
	DEVICE_MMIO_RAM;
	struct k_mutex lock;
	struct sdhc_host_props props;
	struct sdhc_io io;
	uint32_t source_clock;
	uint32_t fifo_depth;
#ifdef CONFIG_SDHC_ROCKCHIP_RK3506_DMA
	struct rk3506_sdhc_dma_desc dma_desc[RK3506_SDHC_DMA_MAX_DESC];
#endif
};

static inline uint32_t rk3506_sdhc_read(const struct device *dev, uint32_t reg)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + reg);
}

static inline void rk3506_sdhc_write(const struct device *dev, uint32_t reg, uint32_t value)
{
	sys_write32(value, DEVICE_MMIO_GET(dev) + reg);
}

static int rk3506_sdhc_wait_bits(const struct device *dev, uint32_t reg, uint32_t mask,
				 uint32_t expected, uint32_t timeout_us)
{
	uint32_t start = k_cycle_get_32();
	uint32_t timeout_cycles = k_us_to_cyc_ceil32(timeout_us);

	do {
		if ((rk3506_sdhc_read(dev, reg) & mask) == expected) {
			return 0;
		}
		k_busy_wait(1);
	} while (k_cycle_get_32() - start < timeout_cycles);

	return -ETIMEDOUT;
}

static int rk3506_sdhc_reset_fifo(const struct device *dev)
{
	int ret;

	rk3506_sdhc_write(dev, RK3506_SDHC_CTRL, RK3506_SDHC_CTRL_FIFO_RESET);
	ret = rk3506_sdhc_wait_bits(dev, RK3506_SDHC_CTRL, RK3506_SDHC_CTRL_FIFO_RESET, 0U,
				    RK3506_SDHC_RESET_TIMEOUT_US);
	if (ret != 0) {
		LOG_ERR("FIFO reset timed out");
	}

	return ret;
}

#ifdef CONFIG_SDHC_ROCKCHIP_RK3506_DMA
static void rk3506_sdhc_dma_disable(const struct device *dev);
#endif

static void rk3506_sdhc_recover_request(const struct device *dev)
{
#ifdef CONFIG_SDHC_ROCKCHIP_RK3506_DMA
	rk3506_sdhc_dma_disable(dev);
#endif
	(void)rk3506_sdhc_reset_fifo(dev);
	rk3506_sdhc_write(dev, RK3506_SDHC_RINTSTS, RK3506_SDHC_ALL_INTS);
}

static int rk3506_sdhc_reset_controller(const struct device *dev)
{
	int ret;

	rk3506_sdhc_write(dev, RK3506_SDHC_CTRL,
			  RK3506_SDHC_CTRL_RESET | RK3506_SDHC_CTRL_FIFO_RESET |
				  RK3506_SDHC_CTRL_DMA_RESET);
	ret = rk3506_sdhc_wait_bits(dev, RK3506_SDHC_CTRL,
				    RK3506_SDHC_CTRL_RESET | RK3506_SDHC_CTRL_FIFO_RESET |
					    RK3506_SDHC_CTRL_DMA_RESET,
				    0U, RK3506_SDHC_RESET_TIMEOUT_US);
	if (ret != 0) {
		LOG_ERR("controller reset timed out");
		return ret;
	}

	rk3506_sdhc_write(dev, RK3506_SDHC_RINTSTS, RK3506_SDHC_ALL_INTS);
	rk3506_sdhc_write(dev, RK3506_SDHC_INTMASK, 0U);
	rk3506_sdhc_write(dev, RK3506_SDHC_TMOUT, UINT32_MAX);
	rk3506_sdhc_write(dev, RK3506_SDHC_BMOD, RK3506_SDHC_BMOD_IDMAC_RESET);
	ret = rk3506_sdhc_wait_bits(dev, RK3506_SDHC_BMOD, RK3506_SDHC_BMOD_IDMAC_RESET, 0U,
				    RK3506_SDHC_RESET_TIMEOUT_US);

	return ret;
}

static void rk3506_sdhc_init_fifo(const struct device *dev)
{
	struct rk3506_sdhc_data *data = dev->data;
	uint32_t fifo_depth = data->fifo_depth;
	uint32_t threshold;

	if (fifo_depth == 0U) {
		uint32_t fifoth = rk3506_sdhc_read(dev, RK3506_SDHC_FIFOTH);
		uint32_t rx_watermark = (fifoth >> 16U) & 0xfffU;

		fifo_depth = rx_watermark + 1U;
		data->fifo_depth = fifo_depth;
	}

	threshold = fifo_depth / 2U;
	rk3506_sdhc_write(dev, RK3506_SDHC_FIFOTH,
			  RK3506_SDHC_FIFOTH_MSIZE(2U) |
				  RK3506_SDHC_FIFOTH_RX_WMARK(threshold - 1U) |
				  RK3506_SDHC_FIFOTH_TX_WMARK(threshold));
}

static int rk3506_sdhc_ciu_update(const struct device *dev)
{
	uint32_t start = k_cycle_get_32();
	uint32_t timeout_cycles = k_us_to_cyc_ceil32(RK3506_SDHC_RESET_TIMEOUT_US);

	rk3506_sdhc_write(dev, RK3506_SDHC_CMDARG, 0U);
	rk3506_sdhc_write(dev, RK3506_SDHC_CMD,
			  RK3506_SDHC_CMD_START | RK3506_SDHC_CMD_PRV_DAT_WAIT |
				  RK3506_SDHC_CMD_UPD_CLK);

	do {
		if ((rk3506_sdhc_read(dev, RK3506_SDHC_CMD) & RK3506_SDHC_CMD_START) == 0U) {
			return 0;
		}
		k_busy_wait(1);
	} while (k_cycle_get_32() - start < timeout_cycles);

	return -ETIMEDOUT;
}

static int rk3506_sdhc_set_clock(const struct device *dev, uint32_t clock)
{
	struct rk3506_sdhc_data *data = dev->data;
	uint32_t divider;
	int ret;

	if (clock == 0U) {
		rk3506_sdhc_write(dev, RK3506_SDHC_CLKENA, 0U);
		rk3506_sdhc_write(dev, RK3506_SDHC_CLKSRC, 0U);
		return rk3506_sdhc_ciu_update(dev);
	}

	if (clock >= data->source_clock) {
		divider = 0U;
	} else {
		divider = DIV_ROUND_UP(data->source_clock, 2U * clock);
		divider = CLAMP(divider, 1U, 255U);
	}

	rk3506_sdhc_write(dev, RK3506_SDHC_CLKENA, 0U);
	rk3506_sdhc_write(dev, RK3506_SDHC_CLKSRC, 0U);
	ret = rk3506_sdhc_ciu_update(dev);
	if (ret != 0) {
		return ret;
	}

	rk3506_sdhc_write(dev, RK3506_SDHC_CLKDIV, divider);
	ret = rk3506_sdhc_ciu_update(dev);
	if (ret != 0) {
		return ret;
	}

	rk3506_sdhc_write(dev, RK3506_SDHC_CLKENA,
			  RK3506_SDHC_CLKEN_ACTIVE);
	ret = rk3506_sdhc_ciu_update(dev);
	if (ret != 0) {
		return ret;
	}

	LOG_DBG("clock: requested %u Hz, source %u Hz, divider %u", clock, data->source_clock,
		divider);
	return 0;
}

static int rk3506_sdhc_set_bus_width(const struct device *dev, enum sdhc_bus_width width)
{
	uint32_t ctype;

	switch (width) {
	case SDHC_BUS_WIDTH1BIT:
		ctype = 0U;
		break;
	case SDHC_BUS_WIDTH4BIT:
		ctype = RK3506_SDHC_CTYPE_4BIT;
		break;
	case SDHC_BUS_WIDTH8BIT:
		ctype = RK3506_SDHC_CTYPE_8BIT;
		break;
	default:
		return -EINVAL;
	}

	rk3506_sdhc_write(dev, RK3506_SDHC_CTYPE, ctype);
	return 0;
}

static int rk3506_sdhc_set_io(const struct device *dev, struct sdhc_io *ios)
{
	const struct rk3506_sdhc_config *cfg = dev->config;
	struct rk3506_sdhc_data *data = dev->data;
	bool power_was_off;
	int ret;

	if (ios == NULL) {
		return -EINVAL;
	}

	if (ios->signal_voltage != SD_VOL_3_3_V && ios->signal_voltage != SD_VOL_3_0_V) {
		return -ENOTSUP;
	}

	if (ios->timing != SDHC_TIMING_LEGACY && ios->timing != SDHC_TIMING_HS) {
		return -ENOTSUP;
	}

	if (ios->enhanced_strobe) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	power_was_off = data->io.power_mode != SDHC_POWER_ON;

	if (ios->power_mode == SDHC_POWER_OFF) {
		rk3506_sdhc_write(dev, RK3506_SDHC_CLKENA, 0U);
		rk3506_sdhc_write(dev, RK3506_SDHC_PWREN, 0U);
		data->io = *ios;
		ret = 0;
		goto out;
	}

	if (power_was_off) {
		rk3506_sdhc_write(dev, RK3506_SDHC_PWREN, 1U);
		ret = rk3506_sdhc_reset_controller(dev);
		if (ret != 0) {
			goto out;
		}
		rk3506_sdhc_write(dev, RK3506_SDHC_PWREN, 1U);
		rk3506_sdhc_init_fifo(dev);
	}

	if (ios->clock == 0U) {
		rk3506_sdhc_write(dev, RK3506_SDHC_CLKENA, 0U);
		ret = 0;
	} else {
		ret = clock_control_get_rate(cfg->clock_dev, cfg->ciu_clock,
					     &data->source_clock);
		if (ret != 0 || data->source_clock == 0U) {
			goto out;
		}
		ret = rk3506_sdhc_set_clock(dev, ios->clock);
	}
	if (ret != 0) {
		goto out;
	}

	if (ios->clock != 0U) {
		ret = rk3506_sdhc_set_bus_width(dev, ios->bus_width);
		if (ret != 0) {
			goto out;
		}
	}

	data->io = *ios;
out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int rk3506_sdhc_get_host_props(const struct device *dev, struct sdhc_host_props *props)
{
	struct rk3506_sdhc_data *data = dev->data;

	if (props == NULL) {
		return -EINVAL;
	}

	*props = data->props;
	return 0;
}

static int rk3506_sdhc_get_card_present(const struct device *dev)
{
	const struct rk3506_sdhc_config *cfg = dev->config;

	if (cfg->non_removable) {
		return 1;
	}

	return (rk3506_sdhc_read(dev, RK3506_SDHC_CDETECT) & BIT(0)) == 0U;
}

static int rk3506_sdhc_card_busy(const struct device *dev)
{
	return (rk3506_sdhc_read(dev, RK3506_SDHC_STATUS) & RK3506_SDHC_STATUS_BUSY) != 0U;
}

static int rk3506_sdhc_check_status(const struct device *dev, uint32_t status, bool data)
{
	if ((status & RK3506_SDHC_INT_RTO) != 0U) {
		return -ETIMEDOUT;
	}

	if ((status & RK3506_SDHC_INT_RCRC) != 0U) {
		return -EILSEQ;
	}

	if ((status & RK3506_SDHC_INT_RE) != 0U) {
		return -EIO;
	}

	if (data && (status & (RK3506_SDHC_DATA_ERR | RK3506_SDHC_DATA_TOUT)) != 0U) {
		return (status & RK3506_SDHC_DATA_TOUT) != 0U ? -ETIMEDOUT : -EIO;
	}

	return 0;
}

static void rk3506_sdhc_read_data_fifo(const struct device *dev, uint8_t **buffer, size_t words)
{
	for (size_t i = 0U; i < words; i++) {
		uint32_t value = rk3506_sdhc_read(dev, RK3506_SDHC_DATA);

		memcpy(*buffer, &value, sizeof(value));
		*buffer += sizeof(value);
	}
}

static void rk3506_sdhc_write_data_fifo(const struct device *dev, const uint8_t **buffer,
					size_t words)
{
	for (size_t i = 0U; i < words; i++) {
		uint32_t value;

		memcpy(&value, *buffer, sizeof(value));
		rk3506_sdhc_write(dev, RK3506_SDHC_DATA, value);
		*buffer += sizeof(value);
	}
}

static int rk3506_sdhc_transfer_data(const struct device *dev, struct sdhc_data *data, bool write)
{
	struct rk3506_sdhc_data *data_priv = dev->data;
	size_t total_bytes;
	size_t total_words;
	size_t remaining_words;
	uint8_t *buffer = data->data;
	const uint8_t *write_buffer = data->data;
	uint32_t timeout_ms;

	if (data->blocks != 0U && data->block_size > SIZE_MAX / data->blocks) {
		return -EOVERFLOW;
	}
	total_bytes = data->block_size * data->blocks;
	if ((total_bytes % sizeof(uint32_t)) != 0U) {
		return -EINVAL;
	}
	total_words = total_bytes / sizeof(uint32_t);
	remaining_words = total_words;
	timeout_ms =
		data->timeout_ms > 0 ? data->timeout_ms : RK3506_SDHC_DEFAULT_DATA_TIMEOUT_MS;
	uint32_t start = k_uptime_get_32();

	while (remaining_words > 0U) {
		uint32_t status = rk3506_sdhc_read(dev, RK3506_SDHC_STATUS);
		uint32_t rintsts = rk3506_sdhc_read(dev, RK3506_SDHC_RINTSTS);
		uint32_t fifo_words =
			(status >> RK3506_SDHC_FIFO_COUNT_SHIFT) & RK3506_SDHC_FIFO_COUNT_MASK;
		size_t words;
		int ret = rk3506_sdhc_check_status(dev, rintsts, true);

		if (ret != 0) {
			data->bytes_xfered = (total_words - remaining_words) * sizeof(uint32_t);
			return ret;
		}

		if (write) {
			words = data_priv->fifo_depth - fifo_words;
			words = MIN(words, remaining_words);
			if (words > 0U) {
				rk3506_sdhc_write_data_fifo(dev, &write_buffer, words);
				remaining_words -= words;
				rk3506_sdhc_write(dev, RK3506_SDHC_RINTSTS, RK3506_SDHC_INT_TXDR);
			}
		} else {
			words = MIN(fifo_words, remaining_words);
			if (words > 0U) {
				rk3506_sdhc_read_data_fifo(dev, &buffer, words);
				remaining_words -= words;
				rk3506_sdhc_write(dev, RK3506_SDHC_RINTSTS, RK3506_SDHC_INT_RXDR);
			}
		}

		if ((rintsts & RK3506_SDHC_INT_DTO) != 0U && remaining_words == 0U) {
			break;
		}

		if ((k_uptime_get_32() - start) >= timeout_ms) {
			data->bytes_xfered = (total_words - remaining_words) * sizeof(uint32_t);
			return -ETIMEDOUT;
		}

		k_busy_wait(RK3506_SDHC_POLL_DELAY_US);
	}

	data->bytes_xfered = data->block_size * data->blocks;
	return 0;
}

#ifdef CONFIG_SDHC_ROCKCHIP_RK3506_DMA
static bool rk3506_sdhc_dma_supported(const struct sdhc_data *data)
{
	size_t bytes;

	if (data->blocks <= 1U ||
	    data->block_size > RK3506_SDHC_DMA_MAX_BYTES / data->blocks) {
		return false;
	}

	bytes = data->block_size * data->blocks;
	return bytes <= RK3506_SDHC_DMA_MAX_BYTES &&
	       (bytes % sizeof(uint32_t)) == 0U &&
	       ((uintptr_t)data->data % CONFIG_SDHC_BUFFER_ALIGNMENT) == 0U;
}

static void rk3506_sdhc_dma_disable(const struct device *dev)
{
	uint32_t ctrl = rk3506_sdhc_read(dev, RK3506_SDHC_CTRL);

	rk3506_sdhc_write(dev, RK3506_SDHC_CTRL, ctrl & ~RK3506_SDHC_CTRL_IDMAC);
	rk3506_sdhc_write(dev, RK3506_SDHC_IDINTEN, 0U);
	rk3506_sdhc_write(dev, RK3506_SDHC_BMOD, 0U);
	rk3506_sdhc_write(dev, RK3506_SDHC_IDSTS, RK3506_SDHC_IDSTS_ALL);
}

static int rk3506_sdhc_reset_dma_engine(const struct device *dev)
{
	uint32_t ctrl = rk3506_sdhc_read(dev, RK3506_SDHC_CTRL);
	int ret;

	rk3506_sdhc_write(dev, RK3506_SDHC_CTRL,
			  (ctrl & ~RK3506_SDHC_CTRL_IDMAC) | RK3506_SDHC_CTRL_DMA_RESET);
	ret = rk3506_sdhc_wait_bits(dev, RK3506_SDHC_CTRL,
				    RK3506_SDHC_CTRL_DMA_RESET, 0U,
				    RK3506_SDHC_RESET_TIMEOUT_US);
	if (ret != 0) {
		return ret;
	}

	rk3506_sdhc_write(dev, RK3506_SDHC_BMOD, RK3506_SDHC_BMOD_IDMAC_RESET);
	return rk3506_sdhc_wait_bits(dev, RK3506_SDHC_BMOD,
				     RK3506_SDHC_BMOD_IDMAC_RESET, 0U,
				     RK3506_SDHC_RESET_TIMEOUT_US);
}

static int rk3506_sdhc_prepare_dma(const struct device *dev, struct sdhc_data *data,
				   bool write)
{
	struct rk3506_sdhc_data *data_priv = dev->data;
	struct rk3506_sdhc_dma_desc *desc = data_priv->dma_desc;
	size_t bytes = data->block_size * data->blocks;
	size_t desc_count = DIV_ROUND_UP(bytes, RK3506_SDHC_DMA_CHUNK_SIZE);
	uintptr_t buffer_phys;
	uintptr_t desc_phys;
	int ret;

	ret = rk3506_sdhc_reset_dma_engine(dev);
	if (ret != 0) {
		return ret;
	}

	if (write) {
		ret = sys_cache_data_flush_range(data->data, bytes);
	} else {
		/*
		 * The DMA overwrites the entire receive buffer. Invalidating
		 * before the transfer prevents dirty lines from being evicted
		 * over DMA data without paying for an unnecessary writeback.
		 */
		ret = sys_cache_data_invd_range(data->data, bytes);
	}
	if (ret != 0) {
		return ret;
	}

	buffer_phys = k_mem_phys_addr(data->data);
	desc_phys = k_mem_phys_addr(desc);

	if (desc_count > RK3506_SDHC_DMA_MAX_DESC) {
		return -E2BIG;
	}

	for (size_t i = 0U; i < desc_count; i++) {
		size_t offset = i * RK3506_SDHC_DMA_CHUNK_SIZE;
		size_t chunk = MIN(bytes - offset, RK3506_SDHC_DMA_CHUNK_SIZE);

		desc[i].des0 = RK3506_SDHC_DESC_OWN;
		desc[i].des3 = 0U;
		if (i == 0U) {
			desc[i].des0 |= RK3506_SDHC_DESC_FS;
		}
		if (i + 1U == desc_count) {
			desc[i].des0 |= RK3506_SDHC_DESC_LD;
		} else {
			desc[i].des0 |= RK3506_SDHC_DESC_CH | RK3506_SDHC_DESC_DIC;
			desc[i].des3 =
				(uint32_t)(desc_phys + ((i + 1U) * sizeof(desc[0])));
		}
		desc[i].des1 = chunk;
		desc[i].des2 = (uint32_t)(buffer_phys + offset);
	}

	ret = sys_cache_data_flush_range(desc, desc_count * sizeof(desc[0]));
	if (ret != 0) {
		return ret;
	}
	__DSB();

	rk3506_sdhc_write(dev, RK3506_SDHC_IDINTEN, RK3506_SDHC_IDINTEN_COMPLETE);
	rk3506_sdhc_write(dev, RK3506_SDHC_IDSTS, RK3506_SDHC_IDSTS_ALL);
	rk3506_sdhc_write(dev, RK3506_SDHC_BMOD,
			  RK3506_SDHC_BMOD_DMA_ENABLE | RK3506_SDHC_BMOD_FIXED_BURST);
	rk3506_sdhc_write(dev, RK3506_SDHC_DBADDR, (uint32_t)desc_phys);
	rk3506_sdhc_write(dev, RK3506_SDHC_CTRL,
			  rk3506_sdhc_read(dev, RK3506_SDHC_CTRL) |
				  RK3506_SDHC_CTRL_IDMAC);
	__DSB();
	rk3506_sdhc_write(dev, RK3506_SDHC_PLDMND, 1U);

	return 0;
}

static int rk3506_sdhc_wait_dma(const struct device *dev, struct sdhc_data *data, bool write)
{
	struct rk3506_sdhc_data *data_priv = dev->data;
	size_t bytes = data->block_size * data->blocks;
	uint32_t timeout_ms =
		data->timeout_ms > 0 ? data->timeout_ms : RK3506_SDHC_DEFAULT_DATA_TIMEOUT_MS;
	uint32_t start = k_uptime_get_32();
	int ret;

	while (true) {
		uint32_t rintsts = rk3506_sdhc_read(dev, RK3506_SDHC_RINTSTS);
		uint32_t idsts = rk3506_sdhc_read(dev, RK3506_SDHC_IDSTS);

		ret = rk3506_sdhc_check_status(dev, rintsts, true);
		if (ret != 0) {
			LOG_ERR("DMA card error: rintsts=0x%08x idsts=0x%08x",
				rintsts, idsts);
			return ret;
		}
		if ((idsts & (RK3506_SDHC_IDSTS_FBE | RK3506_SDHC_IDSTS_DU |
			      RK3506_SDHC_IDSTS_CES)) != 0U) {
			LOG_ERR("DMA engine error: rintsts=0x%08x idsts=0x%08x",
				rintsts, idsts);
			return -EIO;
		}
		if ((idsts & (RK3506_SDHC_IDSTS_RI | RK3506_SDHC_IDSTS_TI)) != 0U &&
		    (rintsts & RK3506_SDHC_INT_DTO) != 0U) {
			break;
		}
		if ((k_uptime_get_32() - start) >= timeout_ms) {
			LOG_ERR("DMA timeout: rintsts=0x%08x idsts=0x%08x "
				"ctrl=0x%08x bmod=0x%08x dbaddr=0x%08x "
				"dscaddr=0x%08x bufaddr=0x%08x idinten=0x%08x",
				rintsts, idsts, rk3506_sdhc_read(dev, RK3506_SDHC_CTRL),
				rk3506_sdhc_read(dev, RK3506_SDHC_BMOD),
				rk3506_sdhc_read(dev, RK3506_SDHC_DBADDR),
				rk3506_sdhc_read(dev, RK3506_SDHC_DSCADDR),
				rk3506_sdhc_read(dev, RK3506_SDHC_BUFADDR),
				rk3506_sdhc_read(dev, RK3506_SDHC_IDINTEN));
			return -ETIMEDOUT;
		}
		k_busy_wait(RK3506_SDHC_POLL_DELAY_US);
	}

	size_t desc_count = DIV_ROUND_UP(bytes, RK3506_SDHC_DMA_CHUNK_SIZE);

	(void)sys_cache_data_invd_range(data_priv->dma_desc,
					desc_count * sizeof(data_priv->dma_desc[0]));
	if (!write) {
		(void)sys_cache_data_invd_range(data->data, bytes);
	}

	data->bytes_xfered = bytes;
	return 0;
}
#endif /* CONFIG_SDHC_ROCKCHIP_RK3506_DMA */

static uint32_t rk3506_sdhc_command_flags(const struct sdhc_command *cmd, bool has_data, bool write)
{
	uint32_t response_type = cmd->response_type & SDHC_NATIVE_RESPONSE_MASK;
	uint32_t flags = RK3506_SDHC_CMD_START | RK3506_SDHC_CMD_USE_HOLD_REG;

	if (response_type != SD_RSP_TYPE_NONE) {
		flags |= RK3506_SDHC_CMD_RESP_EXP;
	}

	if (response_type == SD_RSP_TYPE_R2) {
		flags |= RK3506_SDHC_CMD_RESP_LENGTH;
	}

	if (response_type == SD_RSP_TYPE_R1 || response_type == SD_RSP_TYPE_R1b ||
	    response_type == SD_RSP_TYPE_R2 || response_type == SD_RSP_TYPE_R5 ||
	    response_type == SD_RSP_TYPE_R5b || response_type == SD_RSP_TYPE_R6 ||
	    response_type == SD_RSP_TYPE_R7) {
		flags |= RK3506_SDHC_CMD_CHECK_CRC;
	}

	if (has_data) {
		flags |= RK3506_SDHC_CMD_DATA_EXP;
		if (write) {
			flags |= RK3506_SDHC_CMD_RW;
		}

		if (cmd->opcode == SD_READ_MULTIPLE_BLOCK ||
		    cmd->opcode == SD_WRITE_MULTIPLE_BLOCK) {
			flags |= RK3506_SDHC_CMD_SEND_STOP;
		}
	}

	if (cmd->opcode == SD_STOP_TRANSMISSION) {
		flags |= RK3506_SDHC_CMD_ABORT_STOP;
	} else {
		flags |= RK3506_SDHC_CMD_PRV_DAT_WAIT;
	}

	return flags;
}

static bool rk3506_sdhc_is_write(const struct sdhc_command *cmd)
{
	return cmd->opcode == SD_WRITE_SINGLE_BLOCK || cmd->opcode == SD_WRITE_MULTIPLE_BLOCK;
}

static int rk3506_sdhc_wait_command(const struct device *dev, const struct sdhc_command *cmd,
				    bool has_data)
{
	uint32_t timeout_ms =
		cmd->timeout_ms > 0 ? cmd->timeout_ms : RK3506_SDHC_DEFAULT_CMD_TIMEOUT_MS;
	uint32_t start = k_uptime_get_32();

	while (true) {
		uint32_t status = rk3506_sdhc_read(dev, RK3506_SDHC_RINTSTS);
		int ret = rk3506_sdhc_check_status(dev, status, has_data);

		if (ret != 0) {
			return ret;
		}

		if ((status & RK3506_SDHC_INT_CDONE) != 0U) {
			return 0;
		}

		if ((k_uptime_get_32() - start) >= timeout_ms) {
			return -ETIMEDOUT;
		}

		k_busy_wait(RK3506_SDHC_POLL_DELAY_US);
	}
}

static void rk3506_sdhc_read_response(const struct device *dev, struct sdhc_command *cmd)
{
	uint32_t response_type = cmd->response_type & SDHC_NATIVE_RESPONSE_MASK;
	uint32_t resp0;
	uint32_t resp1;
	uint32_t resp2;
	uint32_t resp3;

	if (response_type == SD_RSP_TYPE_NONE) {
		return;
	}

	resp0 = rk3506_sdhc_read(dev, RK3506_SDHC_RESP0);
	if (response_type != SD_RSP_TYPE_R2) {
		cmd->response[0] = resp0;
		cmd->response[1] = 0U;
		cmd->response[2] = 0U;
		cmd->response[3] = 0U;
		return;
	}

	resp1 = rk3506_sdhc_read(dev, RK3506_SDHC_RESP1);
	resp2 = rk3506_sdhc_read(dev, RK3506_SDHC_RESP2);
	resp3 = rk3506_sdhc_read(dev, RK3506_SDHC_RESP3);

	/* Zephyr stores R2 least-significant word first. */
	cmd->response[0] = resp0;
	cmd->response[1] = resp1;
	cmd->response[2] = resp2;
	cmd->response[3] = resp3;
}

static int rk3506_sdhc_wait_card_ready(const struct device *dev)
{
	uint32_t start = k_uptime_get_32();

	while ((rk3506_sdhc_read(dev, RK3506_SDHC_STATUS) & RK3506_SDHC_STATUS_BUSY) != 0U) {
		if ((k_uptime_get_32() - start) >= RK3506_SDHC_BUSY_TIMEOUT_MS) {
			return -ETIMEDOUT;
		}
		k_busy_wait(RK3506_SDHC_POLL_DELAY_US);
	}

	return 0;
}

static int rk3506_sdhc_request(const struct device *dev, struct sdhc_command *cmd,
			       struct sdhc_data *data)
{
	struct rk3506_sdhc_data *data_priv = dev->data;
	uint32_t flags;
	bool write = false;
#ifdef CONFIG_SDHC_ROCKCHIP_RK3506_DMA
	bool use_dma = false;
#endif
	int ret;

	if (cmd == NULL) {
		return -EINVAL;
	}

	if (data != NULL) {
		data->bytes_xfered = 0U;
		if (data->data == NULL || data->block_size == 0U || data->blocks == 0U ||
		    (data->block_size % sizeof(uint32_t)) != 0U) {
			return -EINVAL;
		}
		if (data->blocks > UINT32_MAX / data->block_size) {
			return -EOVERFLOW;
		}
		write = rk3506_sdhc_is_write(cmd);
	}

	k_mutex_lock(&data_priv->lock, K_FOREVER);
	ret = rk3506_sdhc_wait_card_ready(dev);
	if (ret != 0) {
		goto out;
	}

	rk3506_sdhc_write(dev, RK3506_SDHC_RINTSTS, RK3506_SDHC_ALL_INTS);
	if (data != NULL) {
		ret = rk3506_sdhc_reset_fifo(dev);
		if (ret != 0) {
			goto out;
		}
		rk3506_sdhc_write(dev, RK3506_SDHC_BLKSIZ, data->block_size);
		rk3506_sdhc_write(dev, RK3506_SDHC_BYTCNT, data->block_size * data->blocks);
#ifdef CONFIG_SDHC_ROCKCHIP_RK3506_DMA
		if (rk3506_sdhc_dma_supported(data)) {
			ret = rk3506_sdhc_prepare_dma(dev, data, write);
			if (ret != 0) {
				goto out;
			}
			use_dma = true;
		} else {
			rk3506_sdhc_dma_disable(dev);
		}
#endif
	}

	flags = rk3506_sdhc_command_flags(cmd, data != NULL, write) | (cmd->opcode & 0x3fU);
	rk3506_sdhc_write(dev, RK3506_SDHC_CMDARG, cmd->arg);
	rk3506_sdhc_write(dev, RK3506_SDHC_CMD, flags);

	ret = rk3506_sdhc_wait_command(dev, cmd, data != NULL);
	if (ret != 0) {
		goto out;
	}

	rk3506_sdhc_read_response(dev, cmd);

	if (data != NULL) {
#ifdef CONFIG_SDHC_ROCKCHIP_RK3506_DMA
		if (use_dma) {
			ret = rk3506_sdhc_wait_dma(dev, data, write);
			rk3506_sdhc_dma_disable(dev);
		} else {
			ret = rk3506_sdhc_transfer_data(dev, data, write);
		}
#else
		ret = rk3506_sdhc_transfer_data(dev, data, write);
#endif
		if (ret != 0) {
			goto out;
		}
	}

	rk3506_sdhc_write(dev, RK3506_SDHC_RINTSTS, RK3506_SDHC_ALL_INTS);
out:
	if (ret != 0) {
		LOG_DBG("CMD%u failed: %d", cmd->opcode, ret);
		rk3506_sdhc_recover_request(dev);
	}
	k_mutex_unlock(&data_priv->lock);
	return ret;
}

static int rk3506_sdhc_reset(const struct device *dev)
{
	struct rk3506_sdhc_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	rk3506_sdhc_write(dev, RK3506_SDHC_PWREN, 1U);
	ret = rk3506_sdhc_reset_controller(dev);
	if (ret == 0) {
		rk3506_sdhc_init_fifo(dev);
		if (data->io.power_mode == SDHC_POWER_ON) {
			rk3506_sdhc_write(dev, RK3506_SDHC_PWREN, 1U);
			ret = rk3506_sdhc_set_clock(dev, data->io.clock);
			if (ret == 0) {
				ret = rk3506_sdhc_set_bus_width(dev, data->io.bus_width);
			}
		}
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static DEVICE_API(sdhc, rk3506_sdhc_api) = {
	.reset = rk3506_sdhc_reset,
	.request = rk3506_sdhc_request,
	.set_io = rk3506_sdhc_set_io,
	.get_card_present = rk3506_sdhc_get_card_present,
	.card_busy = rk3506_sdhc_card_busy,
	.get_host_props = rk3506_sdhc_get_host_props,
};

static int rk3506_sdhc_init(const struct device *dev)
{
	const struct rk3506_sdhc_config *cfg = dev->config;
	struct rk3506_sdhc_data *data = dev->data;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	if (!device_is_ready(cfg->clock_dev)) {
		LOG_ERR("clock controller is not ready");
		return -ENODEV;
	}

	if (!device_is_ready(cfg->reset.dev)) {
		LOG_ERR("reset controller is not ready");
		return -ENODEV;
	}

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0 && ret != -ENOENT) {
		LOG_ERR("failed to apply pinctrl: %d", ret);
		return ret;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->biu_clock);
	if (ret != 0) {
		LOG_ERR("failed to enable BIU clock: %d", ret);
		return ret;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->ciu_clock);
	if (ret != 0) {
		LOG_ERR("failed to enable CIU clock: %d", ret);
		return ret;
	}

	ret = reset_line_deassert_dt(&cfg->reset);
	if (ret != 0) {
		LOG_ERR("failed to deassert controller reset: %d", ret);
		return ret;
	}

	ret = clock_control_get_rate(cfg->clock_dev, cfg->ciu_clock, &data->source_clock);
	if (ret != 0 || data->source_clock == 0U) {
		LOG_ERR("failed to get CIU clock rate: %d", ret);
		return ret != 0 ? ret : -EIO;
	}

	k_mutex_init(&data->lock);
	data->fifo_depth = cfg->fifo_depth;
	data->io.bus_mode = SDHC_BUSMODE_PUSHPULL;
	data->io.power_mode = SDHC_POWER_OFF;
	data->io.bus_width = cfg->common.bus_width;
	data->io.timing = SDHC_TIMING_LEGACY;
	data->io.signal_voltage = SD_VOL_3_3_V;

	rk3506_sdhc_write(dev, RK3506_SDHC_PWREN, 1U);
	ret = rk3506_sdhc_reset_controller(dev);
	if (ret != 0) {
		return ret;
	}
	rk3506_sdhc_write(dev, RK3506_SDHC_PWREN, 1U);
	rk3506_sdhc_init_fifo(dev);

	sdhc_common_dt_props_init(&data->props, &cfg->common);
	data->props.f_max = MIN(data->props.f_max, data->source_clock);
	data->props.host_caps.vol_330_support = 1U;
	data->props.host_caps.high_spd_support = 1U;
	data->props.host_caps.max_blk_len = 0U;
	data->props.is_spi = false;

	LOG_INF("DW MSHC initialized: source %u Hz, max %u Hz, FIFO %u words", data->source_clock,
		data->props.f_max, data->fifo_depth);

	return 0;
}

#define RK3506_SDHC_INIT(n)                                                                        \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static const struct rk3506_sdhc_config rk3506_sdhc_config_##n = {                          \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(n, biu)),                   \
		.biu_clock = (clock_control_subsys_t)(uintptr_t)DT_INST_CLOCKS_CELL_BY_NAME(       \
			n, biu, clkid),                                                            \
		.ciu_clock = (clock_control_subsys_t)(uintptr_t)DT_INST_CLOCKS_CELL_BY_NAME(       \
			n, ciu, clkid),                                                            \
		.reset = RESET_DT_SPEC_INST_GET(n),                                                \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.common = SDHC_COMMON_CONFIG_DT_INST_INIT(n),                                      \
		.fifo_depth = DT_INST_PROP_OR(n, fifo_depth, 0),                                   \
		.non_removable = DT_INST_PROP_OR(n, non_removable, 0),                             \
	};                                                                                         \
                                                                                                   \
	static struct rk3506_sdhc_data rk3506_sdhc_data_##n;                                       \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, rk3506_sdhc_init, NULL, &rk3506_sdhc_data_##n,                    \
			      &rk3506_sdhc_config_##n, POST_KERNEL, CONFIG_SDHC_INIT_PRIORITY,     \
			      &rk3506_sdhc_api);

DT_INST_FOREACH_STATUS_OKAY(RK3506_SDHC_INIT)
