/*
 *  linux/drivers/mmc/host/sdhci.c - Secure Digital Host Controller Interface driver
 *
 *  Copyright (C) 2005-2008 Pierre Ossman, All Rights Reserved.
 *  Copyright (C) 2012-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * Thanks to the following companies for their support:
 *
 *     - JMicron (hardware and technical support)
 */

#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/sched.h>

#include <linux/leds.h>

#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/slot-gpio.h>

#include <linux/sysedp.h>
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/ktime.h>
#endif

#ifdef CONFIG_EMMC_BLKTRACE
#include <linux/mmc/emmc-trace.h>
#include "../card/queue.h"
#endif
#include "sdhci.h"

#define DRIVER_NAME "sdhci"

#define DBG(f, x...) \
	pr_debug(DRIVER_NAME " [%s()]: " f, __func__, ## x)
#define MMC_CHECK_CMDQ_MODE(host)			\
	(host && host->mmc &&					\
	host->mmc->card &&						\
	host->mmc->card->ext_csd.cmdq_mode_en)

#if defined(CONFIG_LEDS_CLASS) || (defined(CONFIG_LEDS_CLASS_MODULE) && \
	defined(CONFIG_MMC_SDHCI_MODULE))
#define SDHCI_USE_LEDS_CLASS
#endif

#define MAX_TUNING_LOOP 40

#ifdef CONFIG_CMD_DUMP
static volatile unsigned int printk_cpu_test = UINT_MAX;
struct timeval cur_tv;
struct timeval prev_tv, curr_tv;
void mmc_cmd_dump(struct mmc_host *host);
void dbg_add_host_log(struct mmc_host *host, int type, int cmd, int arg)
{
	unsigned long long t;
	unsigned long long nanosec_rem;
	unsigned long flags;
	spin_lock_irqsave(&host->cmd_dump_lock, flags);

	if (host->dbg_run_host_log_dat[host->dbg_host_cnt - 1].type == type &&
		host->dbg_run_host_log_dat[host->dbg_host_cnt - 1].cmd == cmd &&
		host->dbg_run_host_log_dat[host->dbg_host_cnt - 1].arg == arg) {
		spin_unlock_irqrestore(&host->cmd_dump_lock, flags);
		return;
	}
	t = cpu_clock(printk_cpu_test);
	nanosec_rem = do_div(t, 1000000000)/1000;
	do_gettimeofday(&cur_tv);
	host->dbg_run_host_log_dat[host->dbg_host_cnt].time_sec = t;
	host->dbg_run_host_log_dat[host->dbg_host_cnt].time_usec = nanosec_rem;
	host->dbg_run_host_log_dat[host->dbg_host_cnt].type = type;
	host->dbg_run_host_log_dat[host->dbg_host_cnt].cmd = cmd;
	host->dbg_run_host_log_dat[host->dbg_host_cnt].arg = arg;
	host->dbg_host_cnt++;
	if (host->dbg_host_cnt >= dbg_max_cnt)
		host->dbg_host_cnt = 0;
	spin_unlock_irqrestore(&host->cmd_dump_lock, flags);
}
#endif

/* MMC_RTPM timeout */
#define MMC_RTPM_MSEC_TMOUT 10

/* SDIO 1msec timeout, but use 10msec timeout for HZ=100 */
#define SDIO_CLK_GATING_TICK_TMOUT ((HZ >= 1000) ? (HZ / 1000) : 1)
/* 20msec EMMC delayed clock gate timeout */
#define EMMC_CLK_GATING_TICK_TMOUT ((HZ >= 50) ? (HZ / 50) : 2)

#define IS_SDIO_CARD(host) \
		(host->mmc->card && \
		(host->mmc->card->type == MMC_TYPE_SDIO))

#define IS_EMMC_CARD(host) \
		(host->mmc->card && \
		(host->mmc->card->type == MMC_TYPE_MMC))

#define IS_SDIO_CARD_OR_EMMC(host) \
		(host->mmc->card && \
		((host->mmc->card->type == MMC_TYPE_SDIO) || \
		(host->mmc->card->type == MMC_TYPE_MMC)))

#define IS_DELAYED_CLK_GATE(host) \
		((host->quirks2 & SDHCI_QUIRK2_DELAYED_CLK_GATE) && \
		(IS_SDIO_CARD_OR_EMMC(host)) && \
		(host->mmc->caps2 & MMC_CAP2_CLOCK_GATING))

#ifdef CONFIG_DEBUG_FS

#define IS_32_BIT(x)	(x < (1ULL << 32))

#define IS_DATA_READ(flags)	((flags & MMC_DATA_READ) ? true : false)

#define PERF_STAT_COMPARE(stat, blk_size, blk_count, is_read) \
		( \
			(stat->is_read == is_read) && \
			(stat->stat_blk_size == blk_size) && \
			(stat->stat_blks_per_transfer == blk_count) \
		)

#endif

#define MIN_SDMMC_FREQ 400000

/* Response error index for SD Host controller spec
 * defined errors listed in next comment
 */
#define RESP_ERROR_INDEX(x) ((x & SDHCI_INT_CRC) << 1 | \
			(x & SDHCI_INT_TIMEOUT))

/* based on the SD Host controller spec these three errors are logged
 * CommandCRC Error     Command Timeout Error         Kinds of error
 * 0                    0                             No Error
 * 0                    1                             Response Timeout Error
 * 1                    0                             Response CRC Error
 * 1                    1                             CMD line conflict
 */
static char *resp_error[4] = {"No error", "Response TIMEOUT error",
				"Reaponse CRC error",
				"CMD LINE CONFLICT error"};
static unsigned int debug_quirks;
static unsigned int debug_quirks2;

static void sdhci_finish_data(struct sdhci_host *);

static void sdhci_send_command(struct sdhci_host *, struct mmc_command *);
static void sdhci_finish_command(struct sdhci_host *);
static int sdhci_execute_tuning(struct mmc_host *mmc, u32 opcode);
static int sdhci_validate_sd2_0(struct mmc_host *mmc);
static void sdhci_tuning_timer(unsigned long data);
static void sdhci_enable_preset_value(struct sdhci_host *host, bool enable);

#ifdef CONFIG_PM_RUNTIME
static int sdhci_runtime_pm_get(struct sdhci_host *host);
static int sdhci_runtime_pm_put(struct sdhci_host *host);
#else
static inline int sdhci_runtime_pm_get(struct sdhci_host *host)
{
	return 0;
}
static inline int sdhci_runtime_pm_put(struct sdhci_host *host)
{
	return 0;
}
static inline int sdhci_runtime_resume_host(struct sdhci_host *host)
{
	return 0;
}
static inline int sdhci_runtime_suspend_host(struct sdhci_host *host)
{
	return 0;
}
#endif

static void sdhci_dumpregs(struct sdhci_host *host)
{
	pr_err(DRIVER_NAME ": ================== REGISTER DUMP (%s)==================\n",
		mmc_hostname(host->mmc));

	pr_err(DRIVER_NAME ": Sys addr[0x%03x]: 0x%08x | Version[0x%03x]:  0x%08x\n",
		SDHCI_DMA_ADDRESS, sdhci_readl(host, SDHCI_DMA_ADDRESS),
		SDHCI_HOST_VERSION, sdhci_readw(host, SDHCI_HOST_VERSION));
	pr_err(DRIVER_NAME ": Blk size[0x%03x]: 0x%08x | Blk cnt[0x%03x]:  0x%08x\n",
		SDHCI_BLOCK_SIZE, sdhci_readw(host, SDHCI_BLOCK_SIZE),
		(host->version > SDHCI_SPEC_400) ? SDHCI_BLOCK_COUNT_32BIT :
		SDHCI_BLOCK_COUNT, (host->version > SDHCI_SPEC_400) ?
		sdhci_readw(host, SDHCI_BLOCK_COUNT_32BIT) :
		sdhci_readw(host, SDHCI_BLOCK_COUNT));
	pr_err(DRIVER_NAME ": Argument[0x%03x]: 0x%08x | Trn mode[0x%03x]: 0x%08x\n",
		SDHCI_ARGUMENT, sdhci_readl(host, SDHCI_ARGUMENT),
		SDHCI_TRANSFER_MODE, sdhci_readw(host, SDHCI_TRANSFER_MODE));
	pr_err(DRIVER_NAME ": Present[0x%03x]:  0x%08x | Host ctl[0x%03x]: 0x%08x\n",
		SDHCI_PRESENT_STATE, sdhci_readl(host, SDHCI_PRESENT_STATE),
		SDHCI_HOST_CONTROL, sdhci_readb(host, SDHCI_HOST_CONTROL));
	pr_err(DRIVER_NAME ": Power[0x%03x]:    0x%08x | Blk gap[0x%03x]:  0x%08x\n",
		SDHCI_POWER_CONTROL, sdhci_readb(host, SDHCI_POWER_CONTROL),
		SDHCI_BLOCK_GAP_CONTROL, sdhci_readb(host,
		SDHCI_BLOCK_GAP_CONTROL));
	pr_err(DRIVER_NAME ": Wake-up[0x%03x]:  0x%08x | Clock[0x%03x]:    0x%08x\n",
		SDHCI_WAKE_UP_CONTROL, sdhci_readb(host, SDHCI_WAKE_UP_CONTROL),
		SDHCI_CLOCK_CONTROL, sdhci_readw(host, SDHCI_CLOCK_CONTROL));
	pr_err(DRIVER_NAME ": Timeout[0x%03x]:  0x%08x | Int stat[0x%03x]: 0x%08x\n",
		SDHCI_TIMEOUT_CONTROL, sdhci_readb(host, SDHCI_TIMEOUT_CONTROL),
		SDHCI_INT_STATUS, sdhci_readl(host, SDHCI_INT_STATUS));
	pr_err(DRIVER_NAME ": Int enab[0x%03x]: 0x%08x | Sig enab[0x%03x]: 0x%08x\n",
		SDHCI_INT_ENABLE, sdhci_readl(host, SDHCI_INT_ENABLE),
		SDHCI_SIGNAL_ENABLE, sdhci_readl(host, SDHCI_SIGNAL_ENABLE));
	pr_err(DRIVER_NAME ": AC12 err[0x%03x]: 0x%08x | Slot int[0x%03x]: 0x%08x\n",
		SDHCI_ACMD12_ERR, sdhci_readw(host, SDHCI_ACMD12_ERR),
		SDHCI_SLOT_INT_STATUS, sdhci_readw(host,
		SDHCI_SLOT_INT_STATUS));
	pr_err(DRIVER_NAME ": Caps[0x%03x]:     0x%08x | Caps_1[0x%03x]:   0x%08x\n",
		SDHCI_CAPABILITIES, sdhci_readl(host, SDHCI_CAPABILITIES),
		SDHCI_CAPABILITIES_1, sdhci_readl(host, SDHCI_CAPABILITIES_1));
	pr_err(DRIVER_NAME ": Cmd[0x%03x]:      0x%08x | Max curr[0x%03x]: 0x%08x\n",
		SDHCI_COMMAND, sdhci_readw(host, SDHCI_COMMAND),
		SDHCI_MAX_CURRENT, sdhci_readl(host, SDHCI_MAX_CURRENT));
	pr_err(DRIVER_NAME ": Host ctl2[0x%03x]: 0x%08x\n",
		SDHCI_HOST_CONTROL2, sdhci_readw(host, SDHCI_HOST_CONTROL2));

#define SDHCI_VNDR_CLK_CTRL	0x100
#define SDHCI_VNDR_MISC_CTRL	0x120
#define SDMMC_SDMEMCOMPPADCTRL  0x1E0
#define SDMMC_AUTO_CAL_CONFIG   0x1E4
#define SDMMC_AUTO_CAL_STATUS   0x1EC
  	pr_err(DRIVER_NAME ": nvda clkc: 0x%08x\n",
  			sdhci_readl(host, SDHCI_VNDR_CLK_CTRL));
  
  	pr_err(DRIVER_NAME ": nvda misc: 0x%08x\n",
  			sdhci_readl(host, SDHCI_VNDR_MISC_CTRL));
  
  	pr_err(DRIVER_NAME ": nvda padc: 0x%08x\n",
  			sdhci_readl(host, SDMMC_SDMEMCOMPPADCTRL));
  
  	pr_err(DRIVER_NAME ": nvda calc: 0x%08x\n",
  			sdhci_readl(host, SDMMC_AUTO_CAL_CONFIG));
  
  	pr_err(DRIVER_NAME ": nvda cals: 0x%08x\n",
  			sdhci_readl(host, SDMMC_AUTO_CAL_STATUS));

	if (host->flags & SDHCI_USE_ADMA)
		pr_err(DRIVER_NAME ": ADMA Err[0x%03x]: 0x%08x | ADMA Ptr[0x%03x]: 0x%08x\n",
		       SDHCI_ADMA_ERROR, readl(host->ioaddr + SDHCI_ADMA_ERROR),
		       SDHCI_ADMA_ADDRESS, readl(host->ioaddr +
		       SDHCI_ADMA_ADDRESS));

	if (host->ops->dump_host_cust_regs)
		host->ops->dump_host_cust_regs(host);

	pr_err(DRIVER_NAME ": =========================================================\n");
}

/*****************************************************************************\
 *                                                                           *
 * Low level functions                                                       *
 *                                                                           *
\*****************************************************************************/

static void sdhci_clear_set_irqs(struct sdhci_host *host, u32 clear, u32 set)
{
	host->ier &= ~clear;
	host->ier |= set;
	sdhci_writel(host, host->ier, SDHCI_INT_ENABLE);
	sdhci_writel(host, host->ier, SDHCI_SIGNAL_ENABLE);
}

static void sdhci_unmask_irqs(struct sdhci_host *host, u32 irqs)
{
	sdhci_clear_set_irqs(host, 0, irqs);
}

static void sdhci_mask_irqs(struct sdhci_host *host, u32 irqs)
{
	sdhci_clear_set_irqs(host, irqs, 0);
}

static void sdhci_set_card_detection(struct sdhci_host *host, bool enable)
{
	u32 present, irqs;

	if ((host->quirks & SDHCI_QUIRK_BROKEN_CARD_DETECTION) ||
	    (host->mmc->caps & MMC_CAP_NONREMOVABLE))
		return;

	present = sdhci_readl(host, SDHCI_PRESENT_STATE) &
			      SDHCI_CARD_PRESENT;
	irqs = present ? SDHCI_INT_CARD_REMOVE : SDHCI_INT_CARD_INSERT;

	if (enable)
		sdhci_unmask_irqs(host, irqs);
	else
		sdhci_mask_irqs(host, irqs);
}

static void sdhci_enable_card_detection(struct sdhci_host *host)
{
	sdhci_set_card_detection(host, true);
}

static void sdhci_disable_card_detection(struct sdhci_host *host)
{
	sdhci_set_card_detection(host, false);
}

static void sdhci_reset(struct sdhci_host *host, u8 mask)
{
	u32 ctrl;
	unsigned long timeout;

	if (host->quirks & SDHCI_QUIRK_NO_CARD_NO_RESET) {
		if (!(sdhci_readl(host, SDHCI_PRESENT_STATE) &
			SDHCI_CARD_PRESENT))
			return;
	}

	if (host->ops->platform_reset_enter)
		host->ops->platform_reset_enter(host, mask);

	sdhci_writeb(host, mask, SDHCI_SOFTWARE_RESET);

	if (mask & SDHCI_RESET_ALL)
		host->clock = 0;

	/* Wait max 100 ms */
	timeout = 100;

	/* hw clears the bit when it's done */
	while (sdhci_readb(host, SDHCI_SOFTWARE_RESET) & mask) {
		if (timeout == 0) {
			pr_err("%s: Reset 0x%x never completed.\n",
				mmc_hostname(host->mmc), (int)mask);
			sdhci_dumpregs(host);
			return;
		}
		timeout--;
		mdelay(1);
	}

	if (host->ops->platform_reset_exit)
		host->ops->platform_reset_exit(host, mask);

	if (host->quirks & SDHCI_QUIRK_RESTORE_IRQS_AFTER_RESET)
		sdhci_clear_set_irqs(host, SDHCI_INT_ALL_MASK, host->ier);

	if (host->flags & (SDHCI_USE_SDMA | SDHCI_USE_ADMA)) {
		if ((host->ops->enable_dma) && (mask & SDHCI_RESET_ALL))
			host->ops->enable_dma(host);
	}

	/*
	 * VERSION_4_EN bit and 64BIT_EN bit are cleared after a full reset
	 * need to re-configure them after each full reset
	 */
	if ((mask & SDHCI_RESET_ALL) && host->version >= SDHCI_SPEC_400) {
		ctrl = sdhci_readl(host, SDHCI_ACMD12_ERR);
		ctrl |= SDHCI_HOST_VERSION_4_EN;
		if (host->quirks2 & SDHCI_QUIRK2_SUPPORT_64BIT_DMA)
			ctrl |= SDHCI_ADDRESSING_64BIT_EN;
		sdhci_writel(host, ctrl, SDHCI_ACMD12_ERR);
	}
}

static void sdhci_set_ios(struct mmc_host *mmc, struct mmc_ios *ios);

static void sdhci_init(struct sdhci_host *host, int soft)
{
	if (soft)
		sdhci_reset(host, SDHCI_RESET_CMD|SDHCI_RESET_DATA);
	else
		sdhci_reset(host, SDHCI_RESET_ALL);

	sdhci_clear_set_irqs(host, SDHCI_INT_ALL_MASK,
		SDHCI_INT_BUS_POWER | SDHCI_INT_DATA_END_BIT |
		SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_INDEX |
		SDHCI_INT_END_BIT | SDHCI_INT_CRC | SDHCI_INT_TIMEOUT |
		SDHCI_INT_DATA_END | SDHCI_INT_RESPONSE);

	if (soft) {
		/* force clock reconfiguration */
		host->clock = 0;
		sdhci_set_ios(host->mmc, &host->mmc->ios);
	}
}

static void sdhci_reinit(struct sdhci_host *host)
{
	sdhci_init(host, 0);
	/*
	 * When tuning mode 1 is selected, the max_block_count value is limited
	 * to 4MB as per the host specification. Default max_blk_count for a
	 * host is defined in the spec and this value should be set during
	 * re-init.
	 */
	if (host->flags & SDHCI_USING_RETUNING_TIMER) {
		host->flags &= ~SDHCI_USING_RETUNING_TIMER;

		del_timer_sync(&host->tuning_timer);
		host->flags &= ~SDHCI_NEEDS_RETUNING;
		if (host->quirks & SDHCI_QUIRK_NO_MULTIBLOCK)
			host->mmc->max_blk_count = 1;
		else
			host->mmc->max_blk_count =
				(host->version > SDHCI_SPEC_400) ?
				((1ULL << BLOCK_COUNT_32BIT) - 1) :
				((1 << BLOCK_COUNT_16BIT) - 1);
	}
	sdhci_enable_card_detection(host);
}

static void sdhci_activate_led(struct sdhci_host *host)
{
	u8 ctrl;

	ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);
	ctrl |= SDHCI_CTRL_LED;
	sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
}

static void sdhci_deactivate_led(struct sdhci_host *host)
{
	u8 ctrl;

	ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);
	ctrl &= ~SDHCI_CTRL_LED;
	sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
}

#ifdef SDHCI_USE_LEDS_CLASS
static void sdhci_led_control(struct led_classdev *led,
	enum led_brightness brightness)
{
	struct sdhci_host *host = container_of(led, struct sdhci_host, led);
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);

	if (host->runtime_suspended)
		goto out;

	if (brightness == LED_OFF)
		sdhci_deactivate_led(host);
	else
		sdhci_activate_led(host);
out:
	spin_unlock_irqrestore(&host->lock, flags);
}
#endif

/*****************************************************************************\
 *                                                                           *
 * Core functions                                                            *
 *                                                                           *
\*****************************************************************************/

static void sdhci_read_block_pio(struct sdhci_host *host)
{
	unsigned long flags;
	size_t blksize, len, chunk;
	u32 uninitialized_var(scratch);
	u8 *buf;

	DBG("PIO reading\n");

	blksize = host->data->blksz;
	chunk = 0;

	local_irq_save(flags);

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		while (len) {
			if (chunk == 0) {
				scratch = sdhci_readl(host, SDHCI_BUFFER);
				chunk = 4;
			}

			*buf = scratch & 0xFF;

			buf++;
			scratch >>= 8;
			chunk--;
			len--;
		}
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}

static void sdhci_write_block_pio(struct sdhci_host *host)
{
	unsigned long flags;
	size_t blksize, len, chunk;
	u32 scratch;
	u8 *buf;

	DBG("PIO writing\n");

	blksize = host->data->blksz;
	chunk = 0;
	scratch = 0;

	local_irq_save(flags);

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		while (len) {
			scratch |= (u32)*buf << (chunk * 8);

			buf++;
			chunk++;
			len--;

			if ((chunk == 4) || ((len == 0) && (blksize == 0))) {
				sdhci_writel(host, scratch, SDHCI_BUFFER);
				chunk = 0;
				scratch = 0;
			}
		}
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}

static void sdhci_transfer_pio(struct sdhci_host *host)
{
	u32 mask;

	BUG_ON(!host->data);

	if (host->data->flags & MMC_DATA_READ)
		mask = SDHCI_DATA_AVAILABLE;
	else
		mask = SDHCI_SPACE_AVAILABLE;

	/*
	 * Some controllers (JMicron JMB38x) mess up the buffer bits
	 * for transfers < 4 bytes. As long as it is just one block,
	 * we can ignore the bits.
	 */
	if ((host->quirks & SDHCI_QUIRK_BROKEN_SMALL_PIO) &&
		(host->data->blocks == 1))
		mask = ~0;

	/*
	 * Start the transfer if the present state register indicates
	 * SDHCI_DATA_AVAILABLE or SDHCI_SPACE_AVAILABLE. The driver should
	 * transfer one complete block of data and wait for the buffer ready
	 * interrupt to transfer the next block of data.
	 */
	if (sdhci_readl(host, SDHCI_PRESENT_STATE) & mask) {
		if (host->quirks & SDHCI_QUIRK_PIO_NEEDS_DELAY)
			udelay(100);

		if (host->data->flags & MMC_DATA_READ)
			sdhci_read_block_pio(host);
		else
			sdhci_write_block_pio(host);
	}

	DBG("PIO transfer complete.\n");
}

static char *sdhci_kmap_atomic(struct scatterlist *sg, unsigned long *flags)
{
	local_irq_save(*flags);
	return kmap_atomic(sg_page(sg)) + sg->offset;
}

static void sdhci_kunmap_atomic(void *buffer, unsigned long *flags)
{
	kunmap_atomic(buffer);
	local_irq_restore(*flags);
}
/* container to handle DMA bus widths of 32/64 bit */

union sdhci_dma_addr_t {
	u64 a;
	dma_addr_t b;
};

static void sdhci_set_adma_desc(struct sdhci_host *host, u8 *desc,
				dma_addr_t addr, int len, unsigned cmd)
{
	__le32 *dataddr = (__le32 __force *)(desc + 4);
	__le64 *dataddr64 = (__le64 __force *)(desc + 4);
	__le16 *cmdlen = (__le16 __force *)desc;
	u32 ctrl;
	union sdhci_dma_addr_t dma_addr;
	dma_addr.b = addr;

	/* SDHCI specification says ADMA descriptors should be 4 byte
	 * aligned, so using 16 or 32bit operations should be safe. */

	cmdlen[0] = cpu_to_le16(cmd);
	cmdlen[1] = cpu_to_le16(len);

	ctrl = sdhci_readl(host, SDHCI_ACMD12_ERR);
	if (ctrl & SDHCI_ADDRESSING_64BIT_EN)
		dataddr64[0] = cpu_to_le64(addr);
	else {
		BUG_ON(dma_addr.a >> 32);
		dataddr[0] = cpu_to_le32(addr);
	}
}

static int sdhci_adma_table_pre(struct sdhci_host *host,
	struct mmc_data *data)
{
	int direction;

	u8 *desc;
	u8 *align;
	dma_addr_t addr;
	dma_addr_t align_addr;
	int len, offset;

	struct scatterlist *sg;
	int i;
	char *buffer;
	unsigned long flags;
	int next_desc;
	u32 ctrl;

	/*
	 * The spec does not specify endianness of descriptor table.
	 * We currently guess that it is LE.
	 */

	if (data->flags & MMC_DATA_READ)
		direction = DMA_FROM_DEVICE;
	else
		direction = DMA_TO_DEVICE;

	/*
	 * The ADMA descriptor table is mapped further down as we
	 * need to fill it with data first.
	 */

	if (!host->use_dma_alloc) {
		host->align_addr = dma_map_single(mmc_dev(host->mmc),
			host->align_buffer, 128 * 8, direction);
		if (dma_mapping_error(mmc_dev(host->mmc), host->align_addr))
			goto fail;
		BUG_ON(host->align_addr & 0x3);
	}

	host->sg_count = dma_map_sg(mmc_dev(host->mmc),
		data->sg, data->sg_len, direction);
	if (host->sg_count == 0)
		goto unmap_align;

	desc = host->adma_desc;
	align = host->align_buffer;

	align_addr = host->align_addr;

	ctrl = sdhci_readl(host, SDHCI_ACMD12_ERR);
	if (ctrl & SDHCI_ADDRESSING_64BIT_EN) {
		if (ctrl & SDHCI_HOST_VERSION_4_EN)
			next_desc = 16;
		else
			next_desc = 12;
	} else {
		/* 32 bit DMA mode supported */
		next_desc = 8;
	}

	for_each_sg(data->sg, sg, host->sg_count, i) {
		addr = sg_dma_address(sg);
		len = sg_dma_len(sg);

		/*
		 * The SDHCI specification states that ADMA
		 * addresses must be 32-bit aligned. If they
		 * aren't, then we use a bounce buffer for
		 * the (up to three) bytes that screw up the
		 * alignment.
		 */
		offset = (4 - (addr & 0x3)) & 0x3;
		if (offset) {
			if (data->flags & MMC_DATA_WRITE) {
				buffer = sdhci_kmap_atomic(sg, &flags);
				WARN_ON(((long)buffer & PAGE_MASK) > (PAGE_SIZE - 3));
				memcpy(align, buffer, offset);
				sdhci_kunmap_atomic(buffer, &flags);
			}

			/* tran, valid */
			sdhci_set_adma_desc(host, desc, align_addr, offset,
						0x21);

			BUG_ON(offset > 65536);

			align += 4;
			align_addr += 4;

			desc += next_desc;

			addr += offset;
			len -= offset;
		}

		BUG_ON(len > 65536);

		/* tran, valid */
		if (len > 0) {
			sdhci_set_adma_desc(host, desc, addr, len, 0x21);
			desc += next_desc;
		}

		/*
		 * If this triggers then we have a calculation bug
		 * somewhere. :/
		 */
		WARN_ON((desc - host->adma_desc) > (128 * 2 + 1) * 8);
	}

	if (host->quirks & SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC) {
		/*
		* Mark the last descriptor as the terminating descriptor
		*/
		if (desc != host->adma_desc) {
			desc -= next_desc;
			desc[0] |= 0x3; /* end and valid*/
		}
	} else {
		/*
		* Add a terminating entry.
		*/

		/* nop, end, valid */
		sdhci_set_adma_desc(host, desc, 0, 0, 0x3);
	}

	/*
	 * Resync align buffer as we might have changed it.
	 */
	if (data->flags & MMC_DATA_WRITE) {
		dma_sync_single_for_device(mmc_dev(host->mmc),
			host->align_addr, 128 * 8, direction);
	}

	if (!host->use_dma_alloc) {
		host->adma_addr = dma_map_single(mmc_dev(host->mmc),
			host->adma_desc, (128 * 2 + 1) * 8, DMA_TO_DEVICE);
		if (dma_mapping_error(mmc_dev(host->mmc), host->adma_addr))
			goto unmap_entries;
		BUG_ON(host->adma_addr & 0x3);
	}

	return 0;

unmap_entries:
	dma_unmap_sg(mmc_dev(host->mmc), data->sg,
		data->sg_len, direction);
unmap_align:
	if (!host->use_dma_alloc)
		dma_unmap_single(mmc_dev(host->mmc), host->align_addr,
				128 * 8, direction);
fail:
	return -EINVAL;
}

static void sdhci_adma_table_post(struct sdhci_host *host,
	struct mmc_data *data)
{
	int direction;

	struct scatterlist *sg;
	int i, size;
	u8 *align;
	char *buffer;
	unsigned long flags;

	if (data->flags & MMC_DATA_READ)
		direction = DMA_FROM_DEVICE;
	else
		direction = DMA_TO_DEVICE;

	if (!host->use_dma_alloc) {
		dma_unmap_single(mmc_dev(host->mmc), host->adma_addr,
			(128 * 2 + 1) * 8, DMA_TO_DEVICE);

		dma_unmap_single(mmc_dev(host->mmc), host->align_addr,
			128 * 8, direction);
	}

	if (data->flags & MMC_DATA_READ) {
		dma_sync_sg_for_cpu(mmc_dev(host->mmc), data->sg,
			data->sg_len, direction);

		align = host->align_buffer;

		for_each_sg(data->sg, sg, host->sg_count, i) {
			if (sg_dma_address(sg) & 0x3) {
				size = 4 - (sg_dma_address(sg) & 0x3);

				buffer = sdhci_kmap_atomic(sg, &flags);
				WARN_ON(((long)buffer & PAGE_MASK) > (PAGE_SIZE - 3));
				memcpy(buffer, align, size);
				sdhci_kunmap_atomic(buffer, &flags);

				align += 4;
			}
		}
	}

	dma_unmap_sg(mmc_dev(host->mmc), data->sg,
		data->sg_len, direction);
}

static u8 sdhci_calc_timeout(struct sdhci_host *host, struct mmc_command *cmd)
{
	u8 count;
	struct mmc_data *data = cmd->data;
	unsigned target_timeout, current_timeout;

	/*
	 * If the host controller provides us with an incorrect timeout
	 * value, just skip the check and use 0xE.  The hardware may take
	 * longer to time out, but that's much better than having a too-short
	 * timeout value.
	 */
	if (host->quirks & SDHCI_QUIRK_BROKEN_TIMEOUT_VAL)
		return 0xE;

	/* Unspecified timeout, assume max */
	if (!data && !cmd->cmd_timeout_ms)
		return 0xE;

	/* timeout in us */
	if (!data)
		target_timeout = cmd->cmd_timeout_ms * 1000;
	else {
		target_timeout = data->timeout_ns / 1000;
		if (host->clock)
			target_timeout += data->timeout_clks / host->clock;
	}

	/*
	 * Figure out needed cycles.
	 * We do this in steps in order to fit inside a 32 bit int.
	 * The first step is the minimum timeout, which will have a
	 * minimum resolution of 6 bits:
	 * (1) 2^13*1000 > 2^22,
	 * (2) host->timeout_clk < 2^16
	 *     =>
	 *     (1) / (2) > 2^6
	 */
	count = 0;
	current_timeout = (1 << 13) * 1000 / host->timeout_clk;
	while (current_timeout < target_timeout) {
		count++;
		current_timeout <<= 1;
		if (count >= 0xF)
			break;
	}

	if (count >= 0xF) {
		DBG("%s: Too large timeout 0x%x requested for CMD%d!\n",
		    mmc_hostname(host->mmc), count, cmd->opcode);
		count = 0xE;
	}

	return count;
}

static void sdhci_set_transfer_irqs(struct sdhci_host *host)
{
	u32 pio_irqs = SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL;
	u32 dma_irqs = SDHCI_INT_DMA_END | SDHCI_INT_ADMA_ERROR;

	if (host->flags & SDHCI_REQ_USE_DMA)
		sdhci_clear_set_irqs(host, pio_irqs, dma_irqs);
	else
		sdhci_clear_set_irqs(host, dma_irqs, pio_irqs);
}

static void sdhci_determine_transfer_mode(struct sdhci_host *host,
	unsigned int req_size, unsigned int req_blocks)
{
	/* Nothing to do if DMA modes are not supported. */
	if (!(host->flags & (SDHCI_USE_SDMA | SDHCI_USE_ADMA))) {
		host->flags &= ~SDHCI_REQ_USE_DMA;
	} else if (!host->max_pio_size || (req_size > host->max_pio_size)) {
		host->flags |= SDHCI_REQ_USE_DMA;
	} else if (req_size < host->max_pio_size) {
		host->flags &= ~SDHCI_REQ_USE_DMA;
		if (host->max_pio_blocks &&
			(req_blocks > host->max_pio_blocks))
			host->flags |= SDHCI_REQ_USE_DMA;
	}
}

static void sdhci_prepare_data(struct sdhci_host *host, struct mmc_command *cmd)
{
	u8 count;
	u8 ctrl;
	struct mmc_data *data = cmd->data;
	int ret;
	union sdhci_dma_addr_t dma_addr;

	if (!MMC_CHECK_CMDQ_MODE(host))
		WARN_ON(host->data);

	if (data || (cmd->flags & MMC_RSP_BUSY)) {
		count = sdhci_calc_timeout(host, cmd);
		sdhci_writeb(host, count, SDHCI_TIMEOUT_CONTROL);
	}

	if (!data)
		return;

	/* Sanity checks */
	BUG_ON(data->blksz * data->blocks > 524288);
	BUG_ON(data->blksz > host->mmc->max_blk_size);
	BUG_ON(data->blocks > 65535);

	host->data = data;
	host->data_early = 0;
	host->data->bytes_xfered = 0;

	/* Select dma or PIO mode for transfer */
	sdhci_determine_transfer_mode(host, data->blksz * data->blocks,
		data->blocks);

	/*
	 * FIXME: This doesn't account for merging when mapping the
	 * scatterlist.
	 */
	if (host->flags & SDHCI_REQ_USE_DMA) {
		int broken, i;
		struct scatterlist *sg;

		broken = 0;
		if (host->flags & SDHCI_USE_ADMA) {
			if (host->quirks & SDHCI_QUIRK_32BIT_ADMA_SIZE)
				broken = 1;
		} else {
			if (host->quirks & SDHCI_QUIRK_32BIT_DMA_SIZE)
				broken = 1;
		}

		if (unlikely(broken)) {
			for_each_sg(data->sg, sg, data->sg_len, i) {
				if (sg->length & 0x3) {
					DBG("Reverting to PIO because of "
						"transfer size (%d)\n",
						sg->length);
					host->flags &= ~SDHCI_REQ_USE_DMA;
					break;
				}
			}
		}
	}

	/*
	 * The assumption here being that alignment is the same after
	 * translation to device address space.
	 */
	if (host->flags & SDHCI_REQ_USE_DMA) {
		int broken, i;
		struct scatterlist *sg;

		broken = 0;
		if (host->flags & SDHCI_USE_ADMA) {
			/*
			 * As we use 3 byte chunks to work around
			 * alignment problems, we need to check this
			 * quirk.
			 */
			if (host->quirks & SDHCI_QUIRK_32BIT_ADMA_SIZE)
				broken = 1;
		} else {
			if (host->quirks & SDHCI_QUIRK_32BIT_DMA_ADDR)
				broken = 1;
		}

		if (unlikely(broken)) {
			for_each_sg(data->sg, sg, data->sg_len, i) {
				if (sg->offset & 0x3) {
					DBG("Reverting to PIO because of "
						"bad alignment\n");
					host->flags &= ~SDHCI_REQ_USE_DMA;
					break;
				}
			}
		}
	}

	if (host->flags & SDHCI_REQ_USE_DMA) {
		if (host->flags & SDHCI_USE_ADMA) {
			ret = sdhci_adma_table_pre(host, data);
			if (ret) {
				/*
				 * This only happens when someone fed
				 * us an invalid request.
				 */
				WARN_ON(1);
				host->flags &= ~SDHCI_REQ_USE_DMA;
			} else {
				sdhci_writel(host,
					(host->adma_addr & 0xFFFFFFFF),
					SDHCI_ADMA_ADDRESS);

				if ((host->version >= SDHCI_SPEC_400) &&
				    (host->quirks2 &
				     SDHCI_QUIRK2_SUPPORT_64BIT_DMA)) {
					if (host->quirks2 &
					    SDHCI_QUIRK2_USE_64BIT_ADDR) {
						dma_addr.b = host->adma_addr;
						sdhci_writel(host,
						(dma_addr.a >> 32)
							& 0xFFFFFFFF,
						SDHCI_UPPER_ADMA_ADDRESS);
					} else {
						sdhci_writel(host, 0,
						SDHCI_UPPER_ADMA_ADDRESS);
					}
				}
			}
		} else {
			int sg_cnt;

			sg_cnt = dma_map_sg(mmc_dev(host->mmc),
					data->sg, data->sg_len,
					(data->flags & MMC_DATA_READ) ?
						DMA_FROM_DEVICE :
						DMA_TO_DEVICE);
			if (sg_cnt == 0) {
				/*
				 * This only happens when someone fed
				 * us an invalid request.
				 */
				WARN_ON(1);
				host->flags &= ~SDHCI_REQ_USE_DMA;
			} else {
				WARN_ON(sg_cnt != 1);
				sdhci_writel(host, sg_dma_address(data->sg),
					SDHCI_DMA_ADDRESS);
			}
		}
	}

	/*
	 * Always adjust the DMA selection as some controllers
	 * (e.g. JMicron) can't do PIO properly when the selection
	 * is ADMA.
	 */
	if (host->version >= SDHCI_SPEC_200) {
		ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);
		ctrl &= ~SDHCI_CTRL_DMA_MASK;
		if ((host->flags & SDHCI_REQ_USE_DMA) &&
			(host->flags & SDHCI_USE_ADMA))
			ctrl |= SDHCI_CTRL_ADMA2;
		else
			ctrl |= SDHCI_CTRL_SDMA;
		sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
	}

	if (!(host->flags & SDHCI_REQ_USE_DMA)) {
		int flags;

		flags = SG_MITER_ATOMIC;
		if (host->data->flags & MMC_DATA_READ)
			flags |= SG_MITER_TO_SG;
		else
			flags |= SG_MITER_FROM_SG;
		sg_miter_start(&host->sg_miter, data->sg, data->sg_len, flags);
		host->blocks = data->blocks;
	}

	sdhci_set_transfer_irqs(host);

	/* Set the DMA boundary value and block size */
	sdhci_writew(host, SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY_ARG,
		data->blksz), SDHCI_BLOCK_SIZE);
	if (host->version > SDHCI_SPEC_400)
		sdhci_writew(host, data->blocks, SDHCI_BLOCK_COUNT_32BIT);
	else
		sdhci_writew(host, data->blocks, SDHCI_BLOCK_COUNT);
}

static void sdhci_set_transfer_mode(struct sdhci_host *host,
	struct mmc_command *cmd)
{
	u16 mode;
	struct mmc_data *data = cmd->data;

	if (data == NULL)
		return;

	WARN_ON(!host->data);

	mode = SDHCI_TRNS_BLK_CNT_EN;
	if (mmc_op_multi(cmd->opcode) || data->blocks > 1) {
		mode |= SDHCI_TRNS_MULTI;
		/*
		 * If we are sending CMD23, CMD12 never gets sent
		 * on successful completion (so no Auto-CMD12).
		 */
		if (!MMC_CHECK_CMDQ_MODE(host)) {
			if (!host->mrq_cmd->sbc &&
				(host->flags & SDHCI_AUTO_CMD12) &&
				mmc_op_multi(cmd->opcode))
					mode |= SDHCI_TRNS_AUTO_CMD12;
			else if (host->mrq_cmd->sbc &&
				(host->flags & SDHCI_AUTO_CMD23)) {
					mode |= SDHCI_TRNS_AUTO_CMD23;
					sdhci_writel(host,
						host->mrq_cmd->sbc->arg,
						SDHCI_ARGUMENT2);
			}
		}
	}

	if (data->flags & MMC_DATA_READ)
		mode |= SDHCI_TRNS_READ;
	if (host->flags & SDHCI_REQ_USE_DMA)
		mode |= SDHCI_TRNS_DMA;

	sdhci_writew(host, mode, SDHCI_TRANSFER_MODE);
}

#ifdef CONFIG_DEBUG_FS
static void sdhci_div32(
		u32 size_in_bits_x1000, u32 time_usecs,
		u32 *speed_in_kbps)
{
	*speed_in_kbps = DIV_ROUND_CLOSEST(size_in_bits_x1000, time_usecs);
}

static void sdhci_div64(
		u64 size_in_bits_x1000, u64 time_usecs,
		u32 *speed_in_kbps)
{
	int i;

	/* convert 64 bit into 32 bits */
	i = 0;
	while (!(IS_32_BIT(size_in_bits_x1000) && IS_32_BIT(time_usecs))) {
		/* shift right both the operands bytes and time */
		size_in_bits_x1000 >>= 1;
		time_usecs >>= 1;
		i++;
	}
	if (i)
		pr_debug("%s right shifted operands by %d, size=%lld, time=%lld usec\n",
			__func__, i, size_in_bits_x1000, time_usecs);
	/* check for 32 bit operations first */
	sdhci_div32(
		(u32)size_in_bits_x1000, (u32)time_usecs,
		speed_in_kbps);
	return;
}

static void free_stats_nodes(struct sdhci_host *host)
{
	struct data_stat_entry *ptr, *ptr2;

	ptr = host->sdhci_data_stat.head;
	while (ptr) {
		ptr2 = ptr->next;
		host->sdhci_data_stat.stat_size--;
		devm_kfree(host->mmc->parent, ptr);
		ptr = ptr2;
	}
	if (host->sdhci_data_stat.stat_size)
		pr_err("stat_size=%d after free %s\n",
			host->sdhci_data_stat.stat_size,
			__func__);
	host->sdhci_data_stat.head = NULL;
}

static struct data_stat_entry *add_entry_sorted(struct sdhci_host *host,
	unsigned int blk_size, unsigned int blk_count,
	unsigned int data_flags)
{
	struct data_stat_entry *node, *ptr;
	bool is_read;

	if (!blk_count) {
		pr_err("%s %s: call blk_size=%d, blk_count=%d, data_flags=0x%x\n",
			mmc_hostname(host->mmc), __func__,
			blk_size, blk_count, data_flags);
		goto end;
	}

	node = devm_kzalloc(host->mmc->parent, sizeof(struct data_stat_entry),
		GFP_KERNEL);
	if (!node) {
		pr_err("%s, %s, line=%d %s: unable to allocate data_stat_entry\n",
			__FILE__, __func__, __LINE__, mmc_hostname(host->mmc));
		goto end;
	}
	node->stat_blk_size = blk_size;
	node->stat_blks_per_transfer = blk_count;
	is_read = IS_DATA_READ(data_flags);
	node->is_read = is_read;
	host->sdhci_data_stat.stat_size++;
	/* assume existing list is sorted and try to insert this new node
	 * into the increasing order sorted array
	 */
	ptr = host->sdhci_data_stat.head;
	if (!ptr) {
		/* first element */
		host->sdhci_data_stat.head = node;
		return node;
	}
	if (ptr && ((ptr->stat_blk_size > blk_size) ||
		((ptr->stat_blk_size == blk_size) &&
		(ptr->stat_blks_per_transfer > blk_count)))) {
		host->sdhci_data_stat.head = node;
		/* update new head */
		node->next = ptr;
		return node;
	}
	while (ptr->next) {
		if ((ptr->next->stat_blk_size < blk_size) ||
			((ptr->next->stat_blk_size == blk_size) &&
			(ptr->next->stat_blks_per_transfer < blk_count)))
			ptr = ptr->next;
		else
			break;
	}
	/* We are here if -
	 * 1. ptr->next is null or
	 * 2. blk_size of ptr->next is greater than new blk size, so we should
	 *    place the new node between ptr and ptr->next
	 */
	if (!ptr->next) {
		ptr->next = node;
		return node;
	}
	if ((ptr->next->stat_blk_size > blk_size) ||
		((ptr->next->stat_blk_size == blk_size) &&
		(ptr->next->stat_blks_per_transfer > blk_count)) ||
		((ptr->next->stat_blk_size == blk_size) &&
		(ptr->next->stat_blks_per_transfer == blk_count) &&
		(ptr->next->is_read != is_read))) {
		node->next = ptr->next;
		ptr->next = node;
		return node;
	}
	pr_err("%s %s: line=%d should be unreachable ptr-next->blk_size=%d, blks_per_xfer=%d, is_read=%d, new blk_size=%d, blks_per_xfer=%d, data_flags=0x%x\n",
		mmc_hostname(host->mmc), __func__, __LINE__,
		ptr->next->stat_blk_size, ptr->next->stat_blks_per_transfer,
		ptr->next->is_read, blk_size, blk_count, data_flags);
end:
	return NULL;
}

static void free_data_entry(struct sdhci_host *host,
				unsigned int blk_size, unsigned int blk_count,
				unsigned int data_flags)
{
	struct data_stat_entry *ptr, *ptr2;
	bool is_read;

	ptr = host->sdhci_data_stat.head;
	if (!ptr)
		return;
	is_read = IS_DATA_READ(data_flags);
	if (PERF_STAT_COMPARE(ptr, blk_size, blk_count, is_read)) {
		host->sdhci_data_stat.head = ptr->next;
		devm_kfree(host->mmc->parent, ptr);
		host->sdhci_data_stat.stat_size--;
		return;
	}
	while (ptr->next) {
		if (PERF_STAT_COMPARE(ptr->next, blk_size, blk_count,
			is_read)) {
			ptr2 = ptr->next->next;
			devm_kfree(host->mmc->parent, ptr->next);
			host->sdhci_data_stat.stat_size--;
			ptr->next = ptr2;
			return;
		}
		ptr = ptr->next;
	}
	pr_err("Error %s %s: given blk_size=%d not found\n",
		mmc_hostname(host->mmc), __func__, blk_size);
	return;
}

static void update_stat(struct sdhci_host *host, u32 blk_size, u32 blk_count,
			bool is_start_stat, bool is_data_error,
			unsigned int data_flags)
{
	u32 new_kbps;
	struct data_stat_entry *stat;
	ktime_t t;
	bool is_read;

	if (!host->enable_sdhci_perf_stats)
		goto end;

	if (!blk_count) {
		pr_err("%s %s error stats case: blk_size=%d, blk_count=0, is_start_stat=%d, is_data_error=%d, data_flags=0x%x\n",
			mmc_hostname(host->mmc), __func__, blk_size,
			(int)is_start_stat, (int)is_data_error, data_flags);
		goto end;
	}
	stat = host->sdhci_data_stat.head;
	is_read = IS_DATA_READ(data_flags);
	while (stat) {
		if (PERF_STAT_COMPARE(stat, blk_size, blk_count, is_read))
			break;
		stat = stat->next;
	}
	/* allocation skipped in finish call */
	if (!stat) {
		if (!is_start_stat)
			goto end;
		/* allocate an entry */
		stat = add_entry_sorted(host, blk_size, blk_count, data_flags);
		if (!stat) {
			pr_err("%s %s line=%d: stat entry not found\n",
				mmc_hostname(host->mmc), __func__, __LINE__);
			goto end;
		}
	}

	if (is_start_stat) {
		stat->start_ktime = ktime_get();
	} else {
		if (is_data_error) {
			pr_err("%s %s error stats case: blk_size=%d, blk_count=0, is_start_stat=%d, data Error case ... data_flags=0x%x\n",
				mmc_hostname(host->mmc), __func__, blk_size,
				(int)is_start_stat, data_flags);
			memset(&stat->start_ktime, 0, sizeof(ktime_t));
			if (!stat->total_bytes)
				free_data_entry(host, blk_size, blk_count,
					data_flags);
			goto end;
		}
		t = ktime_get();
		stat->duration_usecs = ktime_us_delta(t, stat->start_ktime);
		stat->current_transferred_bytes = (blk_size * blk_count);
		sdhci_div32(
			(((u32)stat->current_transferred_bytes << 3) * 1000),
			stat->duration_usecs,
			&new_kbps);
		if (stat->max_kbps == 0) {
			stat->max_kbps = new_kbps;
			stat->min_kbps = new_kbps;
		} else {
			if (new_kbps > stat->max_kbps)
				stat->max_kbps = new_kbps;
			if (new_kbps < stat->min_kbps)
				stat->min_kbps = new_kbps;
		}
		/* update the total bytes figure for this entry */
		stat->total_usecs += stat->duration_usecs;
		stat->total_bytes += stat->current_transferred_bytes;
		stat->total_transfers++;
	}
end:
	return;
}
#endif

static void sdhci_finish_data(struct sdhci_host *host)
{
	struct mmc_data *data;

	BUG_ON(!host->data);
#ifdef CONFIG_CMD_DUMP
	if (IS_EMMC_CARD(host))
		dbg_add_host_log(host->mmc, 9, 9, (int)host->mrq_dat);
#endif

	data = host->data;
	host->data = NULL;

	if (host->flags & SDHCI_REQ_USE_DMA) {
		if (host->flags & SDHCI_USE_ADMA)
			sdhci_adma_table_post(host, data);
		else {
			dma_unmap_sg(mmc_dev(host->mmc), data->sg,
				data->sg_len, (data->flags & MMC_DATA_READ) ?
					DMA_FROM_DEVICE : DMA_TO_DEVICE);
		}
	}

	/*
	 * The specification states that the block count register must
	 * be updated, but it does not specify at what point in the
	 * data flow. That makes the register entirely useless to read
	 * back so we have to assume that nothing made it to the card
	 * in the event of an error.
	 */
	if (data->error)
		data->bytes_xfered = 0;
	else
		data->bytes_xfered = data->blksz * data->blocks;

	/*
	 * Need to send CMD12 if -
	 * a) open-ended multiblock transfer (no CMD23)
	 * b) error in multiblock transfer
	 */
	if (data->stop &&
	    (data->error ||
	     (!MMC_CHECK_CMDQ_MODE(host) && !host->mrq_dat->sbc))) {

		/*
		 * The controller needs a reset of internal state machines
		 * upon error conditions.
		 */
		if (data->error) {
			if (!MMC_CHECK_CMDQ_MODE(host))
				sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
			else
				sdhci_reset(host, SDHCI_RESET_DATA);
		}
		sdhci_send_command(host, data->stop);
	} else {
		if (MMC_CHECK_CMDQ_MODE(host))
			tasklet_schedule(&host->finish_dat_tasklet);
		else
			tasklet_schedule(&host->finish_tasklet);
	}
#ifdef CONFIG_DEBUG_FS
	if (data->bytes_xfered) {
		update_stat(host, data->blksz, data->blocks, false, false,
			data->flags);
	} else {
		host->no_data_transfer_count++;
		/* performance stats does not include cases of data error */
		update_stat(host, data->blksz, data->blocks, false, true,
			data->flags);
	}
#endif
}

static void sdhci_send_command(struct sdhci_host *host, struct mmc_command *cmd)
{
	int flags;
	u32 mask;
	unsigned long timeout;
	ktime_t cur_time;
	s64 period_time;

	WARN_ON(host->cmd);

	/* Wait max 10 ms */
	timeout = 10;

	if (!host->mrq_cmd && host->mrq_dat)
		host->mrq_cmd = host->mrq_dat;

	mask = SDHCI_CMD_INHIBIT;
	if ((cmd->data != NULL) || (cmd->flags & MMC_RSP_BUSY))
		mask |= SDHCI_DATA_INHIBIT;

	/* We shouldn't wait for data inihibit for stop commands, even
	   though they might use busy signaling */
	if (host->mrq_cmd->data && (cmd == host->mrq_cmd->data->stop))
		mask &= ~SDHCI_DATA_INHIBIT;

	while (sdhci_readl(host, SDHCI_PRESENT_STATE) & mask) {
		if (timeout == 0) {
			pr_err("%s: Controller never released "
				"inhibit bit(s).\n", mmc_hostname(host->mmc));
			sdhci_dumpregs(host);
			cmd->error = -EIO;
			if (MMC_CHECK_CMDQ_MODE(host))
				tasklet_schedule(&host->finish_cmd_tasklet);
			else
				tasklet_schedule(&host->finish_tasklet);
			return;
		}
		timeout--;
		mdelay(1);
	}

	if ((cmd->opcode == MMC_SWITCH) &&
		(((cmd->arg >> 16) & EXT_CSD_SANITIZE_START)
		== EXT_CSD_SANITIZE_START))
		timeout = 600;
	else
		timeout = 30;

	mod_timer(&host->timer, jiffies + timeout * HZ);

	host->cmd = cmd;

	sdhci_prepare_data(host, cmd);

	sdhci_writel(host, cmd->arg, SDHCI_ARGUMENT);

	sdhci_set_transfer_mode(host, cmd);

	if ((cmd->flags & MMC_RSP_136) && (cmd->flags & MMC_RSP_BUSY)) {
		pr_err("%s: Unsupported response type!\n",
			mmc_hostname(host->mmc));
		cmd->error = -EINVAL;
		if (MMC_CHECK_CMDQ_MODE(host))
			tasklet_schedule(&host->finish_cmd_tasklet);
		else
			tasklet_schedule(&host->finish_tasklet);
		return;
	}

	if (!(cmd->flags & MMC_RSP_PRESENT))
		flags = SDHCI_CMD_RESP_NONE;
	else if (cmd->flags & MMC_RSP_136)
		flags = SDHCI_CMD_RESP_LONG;
	else if (cmd->flags & MMC_RSP_BUSY)
		flags = SDHCI_CMD_RESP_SHORT_BUSY;
	else
		flags = SDHCI_CMD_RESP_SHORT;

	if (cmd->flags & MMC_RSP_CRC)
		flags |= SDHCI_CMD_CRC;
	if (cmd->flags & MMC_RSP_OPCODE)
		flags |= SDHCI_CMD_INDEX;

	/* CMD19, CMD21 is special in that the Data Present Select should be set */
	if (cmd->data || cmd->opcode == MMC_SEND_TUNING_BLOCK ||
	    cmd->opcode == MMC_SEND_TUNING_BLOCK_HS200)
		flags |= SDHCI_CMD_DATA;

#ifdef CONFIG_CMD_DUMP
	if (MMC_CHECK_CMDQ_MODE(host))
		dbg_add_host_log(host->mmc, 0, cmd->opcode, cmd->arg);
#endif
#ifdef CONFIG_EMMC_BLKTRACE
	if (!MMC_CHECK_CMDQ_MODE(host)) {
		if (cmd->opcode == MMC_SET_BLOCK_COUNT)
			emmc_trace(MMC_ISSUE, host->mmc->mqrq_cur, host->mmc);
		else if (cmd->opcode == MMC_READ_MULTIPLE_BLOCK ||
				cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK)
			emmc_trace(MMC_ISSUE_DONE,
				host->mmc->mqrq_cur, host->mmc);
	} else {
		if (cmd->opcode == MMC_QUEUED_TASK_ADDRESS)
			emmc_trace(MMC_ISSUE,
				&host->mmc->mq->mqrq[cmd->mrq->areq->mrq->cmd->arg >> 16],
				host->mmc);
		else if (cmd->opcode == MMC_EXECUTE_READ_TASK ||
				cmd->opcode == MMC_EXECUTE_WRITE_TASK)
			emmc_trace(MMC_ISSUE_DONE,
				&host->mmc->mq->mqrq[cmd->arg >> 16],
				host->mmc);
	}
#endif
	if ((host->quirks2 & SDHCI_QUIRK2_PERIODIC_CALIBRATION) &&
		((cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK) ||
		 (cmd->opcode == MMC_WRITE_BLOCK)) &&
		host->is_calibration_done) {
		cur_time = ktime_get();
		period_time = ktime_to_ms(ktime_sub(cur_time,
					host->timestamp));
		if (period_time >= SDHCI_PERIODIC_CALIB_TIMEOUT)
			if (host->ops->switch_signal_voltage_exit)
				host->ops->switch_signal_voltage_exit(host,
						host->mmc->ios.signal_voltage);
	}

	host->command = SDHCI_MAKE_CMD(cmd->opcode, flags);
	sdhci_writew(host, host->command, SDHCI_COMMAND);
}

static void sdhci_finish_command(struct sdhci_host *host)
{
	int i;

	BUG_ON(host->cmd == NULL);
#ifdef CONFIG_CMD_DUMP
	if (IS_EMMC_CARD(host))
		dbg_add_host_log(host->mmc, 8, 8, (int)host->mrq_cmd);
#endif

	if (host->cmd->flags & MMC_RSP_PRESENT) {
		if (host->cmd->flags & MMC_RSP_136) {
			/* CRC is stripped so we need to do some shifting. */
			for (i = 0; i < 4; i++) {
				host->cmd->resp[i] = sdhci_readl(host,
					SDHCI_RESPONSE + (3-i)*4) << 8;
				if (i != 3)
					host->cmd->resp[i] |=
						sdhci_readb(host,
						SDHCI_RESPONSE + (3-i)*4-1);
			}
		} else {
			host->cmd->resp[0] = sdhci_readl(host, SDHCI_RESPONSE);
		}
	}

	host->cmd->error = 0;

#ifdef CONFIG_CMD_DUMP
	if (MMC_CHECK_CMDQ_MODE(host))
		dbg_add_host_log(host->mmc, 0,
			host->cmd->opcode, host->cmd->resp[0]);
#endif
	/* Finished CMD23, now send actual command. */
	if (host->cmd == host->mrq_cmd->sbc) {
		host->cmd = NULL;
		sdhci_send_command(host, host->mrq_cmd->cmd);
	} else {

		/* Processed actual command. */
		if (host->cmd->data && host->data_early) {
			host->cmd = NULL;
			host->mrq_dat = host->mrq_cmd;
			host->mrq_cmd = NULL;
			sdhci_finish_data(host);
		}

		if (!MMC_CHECK_CMDQ_MODE(host)) {
			if (!host->cmd->data)

				tasklet_schedule(&host->finish_tasklet);
			else {
				host->mrq_dat = host->mrq_cmd;
				host->mrq_cmd = NULL;
			}

			host->cmd = NULL;
		} else if (!host->data_early) {
			if (!host->mrq_cmd->cmd->error &&
			!host->cmd->error && host->cmd->data) {
				host->cmd = NULL;
				host->mrq_dat = host->mrq_cmd;
				host->mrq_cmd = NULL;
			}
			tasklet_schedule(&host->finish_cmd_tasklet);
		}
	}
}

static u16 sdhci_get_preset_value(struct sdhci_host *host)
{
	u16 ctrl, preset = 0;

	ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);

	switch (ctrl & SDHCI_CTRL_UHS_MASK) {
	case SDHCI_CTRL_UHS_SDR12:
		preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR12);
		break;
	case SDHCI_CTRL_UHS_SDR25:
		preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR25);
		break;
	case SDHCI_CTRL_UHS_SDR50:
		preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR50);
		break;
	case SDHCI_CTRL_UHS_SDR104:
		preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR104);
		break;
	case SDHCI_CTRL_UHS_DDR50:
		preset = sdhci_readw(host, SDHCI_PRESET_FOR_DDR50);
		break;
	default:
		pr_warn("%s: Invalid UHS-I mode selected\n",
			mmc_hostname(host->mmc));
		preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR12);
		break;
	}
	return preset;
}

static void sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	int div = 0; /* Initialized for compiler warning */
	int real_div = div, clk_mul = 1;
	u16 clk = 0;
	unsigned long timeout;
	u32 caps;

	if (clock && clock == host->clock)
		return;

	host->mmc->actual_clock = 0;

	if (host->quirks & SDHCI_QUIRK_NONSTANDARD_CLOCK)
		return;

	/*
	 * If the entire clock control register is updated with zero, some
	 * controllers might first update clock divisor fields and then update
	 * the INT_CLK_EN and CARD_CLK_EN fields. Disable card clock first
	 * to ensure there is no abnormal clock behavior.
	 */
	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	clk &= ~SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
	clk = 0;
	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

	if (clock == 0)
		goto out;

	if (host->version >= SDHCI_SPEC_300) {
		if (sdhci_readw(host, SDHCI_HOST_CONTROL2) &
			SDHCI_CTRL_PRESET_VAL_ENABLE) {
			u16 pre_val;

			clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
			pre_val = sdhci_get_preset_value(host);
			div = (pre_val & SDHCI_PRESET_SDCLK_FREQ_MASK)
				>> SDHCI_PRESET_SDCLK_FREQ_SHIFT;
			if (host->clk_mul &&
				(pre_val & SDHCI_PRESET_CLKGEN_SEL_MASK)) {
				clk = SDHCI_PROG_CLOCK_MODE;
				real_div = div + 1;
				clk_mul = host->clk_mul;
			} else {
				real_div = max_t(int, 1, div << 1);
			}
			goto clock_set;
		}

		/*
		 * Check if the Host Controller supports Programmable Clock
		 * Mode.
		 */
		if (host->clk_mul) {
			for (div = 1; div <= 1024; div++) {
				if ((host->max_clk * host->clk_mul / div)
					<= clock)
					break;
			}
			/*
			 * Set Programmable Clock Mode in the Clock
			 * Control register.
			 */
			clk = SDHCI_PROG_CLOCK_MODE;
			real_div = div;
			clk_mul = host->clk_mul;
			div--;
		} else {
			/* Version 3.00 divisors must be a multiple of 2. */
			if (host->max_clk <= clock) {
				if (host->mmc->ios.timing ==
					MMC_TIMING_UHS_DDR50)
					div = 2;
				else
					div = 1;
			} else {
				for (div = 2; div < SDHCI_MAX_DIV_SPEC_300;
				     div += 2) {
					if ((host->max_clk / div) <= clock)
						break;
				}
			}
			real_div = div;
			div >>= 1;
		}
	} else {
		/* Version 2.00 divisors must be a power of 2. */
		for (div = 1; div < SDHCI_MAX_DIV_SPEC_200; div *= 2) {
			if ((host->max_clk / div) <= clock)
				break;
		}
		real_div = div;
		div >>= 1;
	}

clock_set:
	if (real_div)
		host->mmc->actual_clock = (host->max_clk * clk_mul) / real_div;

	clk |= (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
	clk |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
		<< SDHCI_DIVIDER_HI_SHIFT;
	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	/*
	 * For Tegra3 sdmmc controller, internal clock will not be stable bit
	 * will get set only after some other register write is done. To
	 * handle, do a dummy reg write to the caps reg if
	 * SDHCI_QUIRK2_INT_CLK_STABLE_REQ_DUMMY_REG_WRITE is set.
	 */
	if (host->quirks2 & SDHCI_QUIRK2_INT_CLK_STABLE_REQ_DUMMY_REG_WRITE) {
		udelay(5);

		caps = sdhci_readl(host, SDHCI_CAPABILITIES);
		caps |= 1;
		sdhci_writel(host, caps, SDHCI_CAPABILITIES);
	}

	/* Wait max 20 ms */
	timeout = 20;
	while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL))
		& SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			pr_err("%s: Internal clock never "
				"stabilised.\n", mmc_hostname(host->mmc));
			sdhci_dumpregs(host);
			return;
		}
		timeout--;
		mdelay(1);
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

out:
	host->clock = clock;
}

static inline void sdhci_update_clock(struct sdhci_host *host)
{
	unsigned int clock;

	clock = host->clock;
	host->clock = 0;
	if (host->ops->set_clock)
		host->ops->set_clock(host, clock);
	sdhci_set_clock(host, clock);
}

static int sdhci_set_power(struct sdhci_host *host, unsigned short power)
{
	u8 pwr = 0;

	if (power != (unsigned short)-1) {
		switch (1 << power) {
		case MMC_VDD_165_195:
			pwr = SDHCI_POWER_180;
			break;
		case MMC_VDD_29_30:
		case MMC_VDD_30_31:
			pwr = SDHCI_POWER_300;
			break;
		case MMC_VDD_32_33:
		case MMC_VDD_33_34:
			pwr = SDHCI_POWER_330;
			break;
		default:
			BUG();
		}
	}

	if (host->pwr == pwr)
		return -1;

	host->pwr = pwr;

	if (pwr == 0) {
		sdhci_writeb(host, 0, SDHCI_POWER_CONTROL);
		return 0;
	}

	/*
	 * Spec says that we should clear the power reg before setting
	 * a new value. Some controllers don't seem to like this though.
	 */
	if (!(host->quirks & SDHCI_QUIRK_SINGLE_POWER_WRITE))
		sdhci_writeb(host, 0, SDHCI_POWER_CONTROL);

	/*
	 * At least the Marvell CaFe chip gets confused if we set the voltage
	 * and set turn on power at the same time, so set the voltage first.
	 */
	if (host->quirks & SDHCI_QUIRK_NO_SIMULT_VDD_AND_POWER)
		sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);

	pwr |= SDHCI_POWER_ON;

	sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);

	/*
	 * Some controllers need an extra 10ms delay of 10ms before they
	 * can apply clock after applying power
	 */
	if (host->quirks & SDHCI_QUIRK_DELAY_AFTER_POWER)
		mdelay(10);

	return power;
}

/* Execute DLL calibration once for MMC device if it is
 * enumerated in HS400 mode at 200MHz clock freq before
 * starting any data transfer.
 */
static void sdhci_post_init(struct mmc_host *mmc)
{
	struct sdhci_host *host;

	host = mmc_priv(mmc);

	sdhci_runtime_pm_get(host);
	if (host->ops->post_init)
		host->ops->post_init(host);
	sdhci_runtime_pm_put(host);
}
/*****************************************************************************\
 *                                                                           *
 * MMC callbacks                                                             *
 *                                                                           *
\*****************************************************************************/

static void sdhci_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct sdhci_host *host;
	int present;
	unsigned long flags;
	u32 tuning_opcode;

	host = mmc_priv(mmc);

#ifdef CONFIG_DEBUG_FS
	if (mrq->data && mrq->data->blocks)
		update_stat(host, mrq->data->blksz, mrq->data->blocks,
			true, false, mrq->data->flags);
#endif
#ifndef CONFIG_MMC_CQ
		sdhci_runtime_pm_get(host);
#endif
	present = mmc_gpio_get_cd(host->mmc);

	spin_lock_irqsave(&host->lock, flags);

	WARN_ON(host->mrq_cmd != NULL);

#ifndef SDHCI_USE_LEDS_CLASS
	sdhci_activate_led(host);
#endif

	/*
	 * Ensure we don't send the STOP for non-SET_BLOCK_COUNTED
	 * requests if Auto-CMD12 is enabled.
	 */
	if (!MMC_CHECK_CMDQ_MODE(host) && !mrq->sbc && (host->flags & SDHCI_AUTO_CMD12)) {
		if (mrq->stop) {
			mrq->data->stop = NULL;
			mrq->stop = NULL;
		}
	}

	host->mrq_cmd = mrq;
	host->mrq_cmd->data_early = 0;

	/*
	 * Firstly check card presence from cd-gpio.  The return could
	 * be one of the following possibilities:
	 *     negative: cd-gpio is not available
	 *     zero: cd-gpio is used, and card is removed
	 *     one: cd-gpio is used, and card is present
	 */
	if (present < 0) {
		/* If polling, assume that the card is always present. */
		if (host->quirks & SDHCI_QUIRK_BROKEN_CARD_DETECTION)
			if (host->ops->get_cd)
				present = host->ops->get_cd(host);
			else
				present = 1;
		else
			present = sdhci_readl(host, SDHCI_PRESENT_STATE) &
					SDHCI_CARD_PRESENT;
	}

	if (!present || host->flags & SDHCI_DEVICE_DEAD) {
		host->mrq_cmd->cmd->error = -ENOMEDIUM;
		if (MMC_CHECK_CMDQ_MODE(host))
			tasklet_schedule(&host->finish_cmd_tasklet);
		else
			tasklet_schedule(&host->finish_tasklet);
	} else {
		u32 present_state;

		present_state = sdhci_readl(host, SDHCI_PRESENT_STATE);
		/*
		 * Check if the re-tuning timer has already expired and there
		 * is no on-going data transfer. If so, we need to execute
		 * tuning procedure before sending command.
		 */
		if ((host->flags & SDHCI_NEEDS_RETUNING) &&
		    !(present_state & (SDHCI_DOING_WRITE | SDHCI_DOING_READ))) {
			if (!mmc->need_tuning || !mmc->ready_tuning) {
				if (!mmc->need_tuning)
					mmc->need_tuning = 1;
				goto end_tuning;
			}

			if (mmc->card) {
				/* eMMC uses cmd21 but sd and sdio use cmd19 */
				tuning_opcode =
					mmc->card->type == MMC_TYPE_MMC ?
					MMC_SEND_TUNING_BLOCK_HS200 :
					MMC_SEND_TUNING_BLOCK;
				host->mrq_cmd = NULL;
				spin_unlock_irqrestore(&host->lock, flags);
				sdhci_execute_tuning(mmc, tuning_opcode);
				mmc->need_tuning = 0;
				mmc->ready_tuning = 0;
				spin_lock_irqsave(&host->lock, flags);

end_tuning:
				/* Restore original mmc_request structure */
				host->mrq_cmd = mrq;
			}
		}

		/* For a data cmd, check for plat specific preparation */
		spin_unlock_irqrestore(&host->lock, flags);
		if (mrq->data)
			host->ops->platform_get_bus(host);
		spin_lock_irqsave(&host->lock, flags);

		if (!MMC_CHECK_CMDQ_MODE(host) &&
			(mrq->sbc && !(host->flags & SDHCI_AUTO_CMD23)))
				sdhci_send_command(host, mrq->sbc);
		else if (MMC_CHECK_CMDQ_MODE(host) && mrq->sbc)
			sdhci_send_command(host, mrq->sbc);
		else {
			sdhci_send_command(host, mrq->cmd);
		}
	}

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);
}

static void sdhci_do_set_ios(struct sdhci_host *host, struct mmc_ios *ios)
{
	unsigned long flags;
	int vdd_bit = -1;
	u8 ctrl;

	/* cancel delayed clk gate work */
	if (host->quirks2 & SDHCI_QUIRK2_DELAYED_CLK_GATE)
		cancel_delayed_work_sync(&host->delayed_clk_gate_wrk);

	/* Do any required preparations prior to setting ios */
	if (host->ops->platform_ios_config_enter)
		host->ops->platform_ios_config_enter(host, ios);

	spin_lock_irqsave(&host->lock, flags);

	if (host->flags & SDHCI_DEVICE_DEAD) {
		spin_unlock_irqrestore(&host->lock, flags);
		if (host->vmmc && ios->power_mode == MMC_POWER_OFF)
			mmc_regulator_set_ocr(host->mmc, host->vmmc, 0);
		return;
	}

	/*
	 * Reset the chip on each power off.
	 * Should clear out any weird states.
	 */
	if (ios->power_mode == MMC_POWER_OFF) {
		sdhci_writel(host, 0, SDHCI_SIGNAL_ENABLE);
		sdhci_reinit(host);
	}

	if (host->version >= SDHCI_SPEC_300 &&
		(ios->power_mode == MMC_POWER_UP))
		sdhci_enable_preset_value(host, false);

	if (ios->power_mode == MMC_POWER_OFF)
		vdd_bit = sdhci_set_power(host, -1);
	else
		vdd_bit = sdhci_set_power(host, ios->vdd);

	if (host->vmmc && vdd_bit != -1) {
		spin_unlock_irqrestore(&host->lock, flags);
		mmc_regulator_set_ocr(host->mmc, host->vmmc, vdd_bit);
		spin_lock_irqsave(&host->lock, flags);
	}

	sdhci_set_clock(host, ios->clock);

	if (host->ops->platform_send_init_74_clocks)
		host->ops->platform_send_init_74_clocks(host, ios->power_mode);

	/*
	 * If your platform has 8-bit width support but is not a v3 controller,
	 * or if it requires special setup code, you should implement that in
	 * platform_bus_width().
	 */
	if (host->ops->platform_bus_width) {
		host->ops->platform_bus_width(host, ios->bus_width);
	} else {
		ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);
		if (ios->bus_width == MMC_BUS_WIDTH_8) {
			ctrl &= ~SDHCI_CTRL_4BITBUS;
			if (host->version >= SDHCI_SPEC_300)
				ctrl |= SDHCI_CTRL_8BITBUS;
		} else {
			if (host->version >= SDHCI_SPEC_300)
				ctrl &= ~SDHCI_CTRL_8BITBUS;
			if (ios->bus_width == MMC_BUS_WIDTH_4)
				ctrl |= SDHCI_CTRL_4BITBUS;
			else
				ctrl &= ~SDHCI_CTRL_4BITBUS;
		}
		sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
	}

	ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);

	if ((ios->timing == MMC_TIMING_SD_HS ||
	     ios->timing == MMC_TIMING_MMC_HS)
	    && !(host->quirks & SDHCI_QUIRK_NO_HISPD_BIT))
		ctrl |= SDHCI_CTRL_HISPD;
	else
		ctrl &= ~SDHCI_CTRL_HISPD;

	if (host->version >= SDHCI_SPEC_300) {
		u16 clk, ctrl_2;

		/* In case of UHS-I modes, set High Speed Enable */
		if (((ios->timing == MMC_TIMING_MMC_HS200) ||
		    (ios->timing == MMC_TIMING_UHS_SDR50) ||
		    (ios->timing == MMC_TIMING_UHS_SDR104) ||
		    (ios->timing == MMC_TIMING_UHS_DDR50) ||
		    (ios->timing == MMC_TIMING_UHS_SDR25))
		    && !(host->quirks & SDHCI_QUIRK_NO_HISPD_BIT))
			ctrl |= SDHCI_CTRL_HISPD;

		ctrl_2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		if (!(ctrl_2 & SDHCI_CTRL_PRESET_VAL_ENABLE)) {
			sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
			/*
			 * We only need to set Driver Strength if the
			 * preset value enable is not set.
			 */
			ctrl_2 &= ~SDHCI_CTRL_DRV_TYPE_MASK;
			if (ios->drv_type == MMC_SET_DRIVER_TYPE_A)
				ctrl_2 |= SDHCI_CTRL_DRV_TYPE_A;
			else if (ios->drv_type == MMC_SET_DRIVER_TYPE_C)
				ctrl_2 |= SDHCI_CTRL_DRV_TYPE_C;

			sdhci_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);
		} else {
			/*
			 * According to SDHC Spec v3.00, if the Preset Value
			 * Enable in the Host Control 2 register is set, we
			 * need to reset SD Clock Enable before changing High
			 * Speed Enable to avoid generating clock gliches.
			 */

			/* Reset SD Clock Enable */
			clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
			clk &= ~SDHCI_CLOCK_CARD_EN;
			sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

			sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);

			/* Re-enable SD Clock */
			clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
			clk |= SDHCI_CLOCK_CARD_EN;
			sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
		}


		/* Reset SD Clock Enable */
		clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		clk &= ~SDHCI_CLOCK_CARD_EN;
		sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

		if (host->ops->set_uhs_signaling)
			host->ops->set_uhs_signaling(host, ios->timing);
		else {
			ctrl_2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
			/* Select Bus Speed Mode for host */
			ctrl_2 &= ~SDHCI_CTRL_UHS_MASK;
			if (ios->timing == MMC_TIMING_MMC_HS200)
				ctrl_2 |= SDHCI_CTRL_HS_SDR200;
			else if (ios->timing == MMC_TIMING_UHS_SDR12)
				ctrl_2 |= SDHCI_CTRL_UHS_SDR12;
			else if (ios->timing == MMC_TIMING_UHS_SDR25)
				ctrl_2 |= SDHCI_CTRL_UHS_SDR25;
			else if (ios->timing == MMC_TIMING_UHS_SDR50)
				ctrl_2 |= SDHCI_CTRL_UHS_SDR50;
			else if (ios->timing == MMC_TIMING_UHS_SDR104)
				ctrl_2 |= SDHCI_CTRL_UHS_SDR104;
			else if (ios->timing == MMC_TIMING_UHS_DDR50)
				ctrl_2 |= SDHCI_CTRL_UHS_DDR50;
			sdhci_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);
		}

		if (!(host->quirks2 & SDHCI_QUIRK2_PRESET_VALUE_BROKEN) &&
				((ios->timing == MMC_TIMING_UHS_SDR12) ||
				 (ios->timing == MMC_TIMING_UHS_SDR25) ||
				 (ios->timing == MMC_TIMING_UHS_SDR50) ||
				 (ios->timing == MMC_TIMING_UHS_SDR104) ||
				 (ios->timing == MMC_TIMING_UHS_DDR50))) {
			u16 preset;

			sdhci_enable_preset_value(host, true);
			preset = sdhci_get_preset_value(host);
			ios->drv_type = (preset & SDHCI_PRESET_DRV_MASK)
				>> SDHCI_PRESET_DRV_SHIFT;
		}

		/* Re-enable SD Clock */
		clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		clk |= SDHCI_CLOCK_CARD_EN;
		sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
	} else
		sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);

	/*
	 * Some (ENE) controllers go apeshit on some ios operation,
	 * signalling timeout and CRC errors even on CMD0. Resetting
	 * it on each ios seems to solve the problem.
	 */
	if (host->quirks & SDHCI_QUIRK_RESET_CMD_DATA_ON_IOS)
		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);

	/* Platform specific handling post ios setting */
	if (host->ops->platform_ios_config_exit)
		host->ops->platform_ios_config_exit(host, ios);

}

static void sdhci_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
#ifndef CONFIG_MMC_CQ
	sdhci_runtime_pm_get(host);
#endif
	sdhci_do_set_ios(host, ios);
#ifndef CONFIG_MMC_CQ
	sdhci_runtime_pm_put(host);
#endif
}

static int sdhci_do_get_cd(struct sdhci_host *host)
{
	int gpio_cd = mmc_gpio_get_cd(host->mmc);

	if (host->flags & SDHCI_DEVICE_DEAD)
		return 0;

	/* If polling/nonremovable, assume that the card is always present. */
	if (((host->quirks & SDHCI_QUIRK_BROKEN_CARD_DETECTION) &&
	    (!host->ops->get_cd)) ||
	    (host->mmc->caps & MMC_CAP_NONREMOVABLE))
		return 1;

	if (host->ops->get_cd)
		return host->ops->get_cd(host);

	/* Try slot gpio detect */
	if (!IS_ERR_VALUE(gpio_cd))
		return !!gpio_cd;

	/* Host native card detect */
	return !!(sdhci_readl(host, SDHCI_PRESENT_STATE) & SDHCI_CARD_PRESENT);
}

static int sdhci_get_cd(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);
	int ret;

	sdhci_runtime_pm_get(host);
	ret = sdhci_do_get_cd(host);
	sdhci_runtime_pm_put(host);
	return ret;
}

static int sdhci_check_ro(struct sdhci_host *host)
{
	unsigned long flags;
	int is_readonly;

	spin_lock_irqsave(&host->lock, flags);

	if (host->flags & SDHCI_DEVICE_DEAD)
		is_readonly = 0;
	else if (host->ops->get_ro) {
		spin_unlock_irqrestore(&host->lock, flags);
		is_readonly = host->ops->get_ro(host);
		spin_lock_irqsave(&host->lock, flags);
	}
	else
		is_readonly = !(sdhci_readl(host, SDHCI_PRESENT_STATE)
				& SDHCI_WRITE_PROTECT);

	spin_unlock_irqrestore(&host->lock, flags);

	/* This quirk needs to be replaced by a callback-function later */
	return host->quirks & SDHCI_QUIRK_INVERTED_WRITE_PROTECT ?
		!is_readonly : is_readonly;
}

#define SAMPLE_COUNT	5

static int sdhci_do_get_ro(struct sdhci_host *host)
{
	int i, ro_count;

	if (!(host->quirks & SDHCI_QUIRK_UNSTABLE_RO_DETECT))
		return sdhci_check_ro(host);

	ro_count = 0;
	for (i = 0; i < SAMPLE_COUNT; i++) {
		if (sdhci_check_ro(host)) {
			if (++ro_count > SAMPLE_COUNT / 2)
				return 1;
		}
		msleep(30);
	}
	return 0;
}

static void sdhci_hw_reset(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);

	if (host->ops && host->ops->hw_reset)
		host->ops->hw_reset(host);
}

static int sdhci_get_ro(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);
	int ret;

	sdhci_runtime_pm_get(host);
	ret = sdhci_do_get_ro(host);
	sdhci_runtime_pm_put(host);
	return ret;
}

static void sdhci_enable_sdio_irq_nolock(struct sdhci_host *host, int enable)
{
	if (host->flags & SDHCI_DEVICE_DEAD)
		goto out;

	if (enable)
		host->flags |= SDHCI_SDIO_IRQ_ENABLED;
	else
		host->flags &= ~SDHCI_SDIO_IRQ_ENABLED;

	/* SDIO IRQ will be enabled as appropriate in runtime resume */
	if (host->runtime_suspended)
		goto out;

	if (enable)
		sdhci_unmask_irqs(host, SDHCI_INT_CARD_INT);
	else
		sdhci_mask_irqs(host, SDHCI_INT_CARD_INT);
out:
	mmiowb();
}

static void sdhci_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct sdhci_host *host = mmc_priv(mmc);
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	sdhci_enable_sdio_irq_nolock(host, enable);
	spin_unlock_irqrestore(&host->lock, flags);
}

static int sdhci_do_start_signal_voltage_switch(struct sdhci_host *host,
						struct mmc_ios *ios)
{
	u16 ctrl;
	int ret;

	/*
	 * Signal Voltage Switching is only applicable for Host Controllers
	 * v3.00 and above.
	 */
	if (host->version < SDHCI_SPEC_300)
		return 0;

	if (host->quirks2 & SDHCI_QUIRK2_NON_STD_VOLTAGE_SWITCHING) {
		if (host->ops->switch_signal_voltage)
			return host->ops->switch_signal_voltage(
				host, ios->signal_voltage);
	}

	ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);

	switch (ios->signal_voltage) {
	case MMC_SIGNAL_VOLTAGE_330:
		/* Set 1.8V Signal Enable in the Host Control2 register to 0 */
		ctrl &= ~SDHCI_CTRL_VDD_180;
		sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);

		if (host->vqmmc) {
			ret = regulator_set_voltage(host->vqmmc, 2700000, 3600000);
			if (ret) {
				pr_warning("%s: Switching to 3.3V signalling voltage "
						" failed\n", mmc_hostname(host->mmc));
				return -EIO;
			}
		}
		/* Wait for 5ms */
		usleep_range(5000, 5500);

		/* 3.3V regulator output should be stable within 5 ms */
		ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		if (!(ctrl & SDHCI_CTRL_VDD_180))
			return 0;

		pr_warning("%s: 3.3V regulator output did not became stable\n",
				mmc_hostname(host->mmc));

		return -EAGAIN;
	case MMC_SIGNAL_VOLTAGE_180:
		if (host->vqmmc) {
			ret = regulator_set_voltage(host->vqmmc,
					1700000, 1950000);
			if (ret) {
				pr_warning("%s: Switching to 1.8V signalling voltage "
						" failed\n", mmc_hostname(host->mmc));
				return -EIO;
			}
		}

		/*
		 * Enable 1.8V Signal Enable in the Host Control2
		 * register
		 */
		ctrl |= SDHCI_CTRL_VDD_180;
		sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);

		/* Wait for 5ms */
		usleep_range(5000, 5500);

		/* 1.8V regulator output should be stable within 5 ms */
		ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		if (ctrl & SDHCI_CTRL_VDD_180)
			return 0;

		pr_warning("%s: 1.8V regulator output did not became stable\n",
				mmc_hostname(host->mmc));

		return -EAGAIN;
	case MMC_SIGNAL_VOLTAGE_120:
		if (host->vqmmc) {
			ret = regulator_set_voltage(host->vqmmc, 1100000, 1300000);
			if (ret) {
				pr_warning("%s: Switching to 1.2V signalling voltage "
						" failed\n", mmc_hostname(host->mmc));
				return -EIO;
			}
		}
		return 0;
	default:
		/* No signal voltage switch required */
		return 0;
	}
}

static int sdhci_start_signal_voltage_switch(struct mmc_host *mmc,
	struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
	int err;

	if (host->version < SDHCI_SPEC_300)
		return 0;
	sdhci_runtime_pm_get(host);
	err = sdhci_do_start_signal_voltage_switch(host, ios);
	/* Do any post voltage switch platform specific configuration */
	if  (host->ops->switch_signal_voltage_exit)
		host->ops->switch_signal_voltage_exit(host,
			ios->signal_voltage);
	sdhci_runtime_pm_put(host);
	return err;
}

static int sdhci_card_busy(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);
	u32 present_state;

	sdhci_runtime_pm_get(host);
	/* Check whether DAT[3:0] is 0000 */
	present_state = sdhci_readl(host, SDHCI_PRESENT_STATE);
	sdhci_runtime_pm_put(host);

	return !(present_state & SDHCI_DATA_LVL_MASK);
}

static void sdhci_config_tap(struct mmc_host *mmc, u8 option)
{
	struct sdhci_host *host = mmc_priv(mmc);

	if (host->ops->config_tap_delay)
		host->ops->config_tap_delay(host, option);
}

static int sdhci_validate_sd2_0(struct mmc_host *mmc)
{
	struct sdhci_host *host;
	int err = 0;

	host = mmc_priv(mmc);

	if (host->ops->validate_sd2_0)
		err = host->ops->validate_sd2_0(host);
	return err;
}

static int sdhci_execute_tuning(struct mmc_host *mmc, u32 opcode)
{
	struct sdhci_host *host;
	u16 ctrl;
	u32 ier;
	int tuning_loop_counter = MAX_TUNING_LOOP;
	unsigned long timeout;
	int err = 0;
	bool requires_tuning_nonuhs = false;
	u16 clk = 0;

	host = mmc_priv(mmc);

	sdhci_runtime_pm_get(host);
	disable_irq(host->irq);

	if ((host->quirks2 & SDHCI_QUIRK2_NON_STANDARD_TUNING) &&
		host->ops->execute_freq_tuning) {
		err = host->ops->execute_freq_tuning(host, opcode);
		enable_irq(host->irq);
		sdhci_runtime_pm_put(host);
		return err;
	}

	if ((host->quirks2 & SDHCI_QUIRK2_SKIP_TUNING) &&
		host->ops->is_tuning_done) {
		if(host->ops->is_tuning_done(host)) {
			enable_irq(host->irq);
			sdhci_runtime_pm_put(host);
			return 0;
		}
	}

	if ((host->quirks2 & SDHCI_QUIRK2_NON_STD_TUNING_LOOP_CNTR) &&
		(host->ops->get_max_tuning_loop_counter))
		tuning_loop_counter =
			host->ops->get_max_tuning_loop_counter(host);

	spin_lock(&host->lock);
	ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);

	/*
	 * The Host Controller needs tuning only in case of SDR104 mode
	 * and for SDR50 mode when Use Tuning for SDR50 is set in the
	 * Capabilities register.
	 * If the Host Controller supports the HS200 mode then the
	 * tuning function has to be executed.
	 */
	if (((ctrl & SDHCI_CTRL_UHS_MASK) == SDHCI_CTRL_UHS_SDR50) &&
	    (host->flags & SDHCI_SDR50_NEEDS_TUNING ||
	     host->flags & SDHCI_HS200_NEEDS_TUNING))
		requires_tuning_nonuhs = true;

	if (((ctrl & SDHCI_CTRL_UHS_MASK) == SDHCI_CTRL_UHS_SDR104) ||
	    requires_tuning_nonuhs)
		ctrl |= SDHCI_CTRL_EXEC_TUNING;
	else {
		spin_unlock(&host->lock);
		enable_irq(host->irq);
		sdhci_runtime_pm_put(host);
		return 0;
	}

	sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);

	/*
	 * As per the Host Controller spec v3.00, tuning command
	 * generates Buffer Read Ready interrupt, so enable that.
	 *
	 * Note: The spec clearly says that when tuning sequence
	 * is being performed, the controller does not generate
	 * interrupts other than Buffer Read Ready interrupt. But
	 * to make sure we don't hit a controller bug, we _only_
	 * enable Buffer Read Ready interrupt here.
	 */
	ier = host->ier;
	sdhci_clear_set_irqs(host, ier, SDHCI_INT_DATA_AVAIL);

	/*
	 * Issue CMD19 repeatedly till Execute Tuning is set to 0 or the number
	 * of loops reaches 40 times or a timeout of 150ms occurs.
	 */
	timeout = 150;
	do {
		struct mmc_command cmd = {0};
		struct mmc_request mrq = {NULL};

		if (!tuning_loop_counter && !timeout)
			break;

		cmd.opcode = opcode;
		cmd.arg = 0;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;
		cmd.retries = 0;
		cmd.data = NULL;
		cmd.error = 0;

		mrq.cmd = &cmd;
		host->mrq_cmd = &mrq;

		if (host->quirks2 & SDHCI_QUIRK2_NON_STD_TUN_CARD_CLOCK) {
			clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
			clk &= ~SDHCI_CLOCK_CARD_EN;
			sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
		}

		/*
		 * In response to CMD19, the card sends 64 bytes of tuning
		 * block to the Host Controller. So we set the block size
		 * to 64 here.
		 * In response to CMD21, the card sends 128 bytes of tuning
		 * block for MMC_BUS_WIDTH_8 and 64 bytes for MMC_BUS_WIDTH_4
		 * to the Host Controller. So we set the block size to 64 here.
		 */
		if (cmd.opcode == MMC_SEND_TUNING_BLOCK_HS200) {
			if (mmc->ios.bus_width == MMC_BUS_WIDTH_8)
				sdhci_writew(host, SDHCI_MAKE_BLKSZ(7, 128),
					     SDHCI_BLOCK_SIZE);
			else if (mmc->ios.bus_width == MMC_BUS_WIDTH_4)
				sdhci_writew(host, SDHCI_MAKE_BLKSZ(7, 64),
					     SDHCI_BLOCK_SIZE);
		} else {
			sdhci_writew(host, SDHCI_MAKE_BLKSZ(7, 64),
				     SDHCI_BLOCK_SIZE);
		}

		/*
		 * The tuning block is sent by the card to the host controller.
		 * So we set the TRNS_READ bit in the Transfer Mode register.
		 * This also takes care of setting DMA Enable and Multi Block
		 * Select in the same register to 0.
		 */
		sdhci_writew(host, SDHCI_TRNS_READ, SDHCI_TRANSFER_MODE);

		sdhci_send_command(host, &cmd);

		host->cmd = NULL;
		host->mrq_cmd = NULL;

		spin_unlock(&host->lock);
		enable_irq(host->irq);

		if (host->quirks2 & SDHCI_QUIRK2_NON_STD_TUN_CARD_CLOCK) {
			udelay(1);
			sdhci_reset(host, SDHCI_RESET_CMD|SDHCI_RESET_DATA);
			clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
			clk |= SDHCI_CLOCK_CARD_EN;
			sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
		}

		/* Wait for Buffer Read Ready interrupt */
		wait_event_interruptible_timeout(host->buf_ready_int,
					(host->tuning_done == 1),
					msecs_to_jiffies(50));
		disable_irq(host->irq);
		spin_lock(&host->lock);

		if (!host->tuning_done) {
			pr_info(DRIVER_NAME ": Timeout waiting for "
				"Buffer Read Ready interrupt during tuning "
				"procedure, falling back to fixed sampling "
				"clock\n");
			ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
			ctrl &= ~SDHCI_CTRL_TUNED_CLK;
			ctrl &= ~SDHCI_CTRL_EXEC_TUNING;
			sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);

			err = -EIO;
			goto out;
		}

		host->tuning_done = 0;

		ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		tuning_loop_counter--;
		timeout--;
		mdelay(1);
	} while (ctrl & SDHCI_CTRL_EXEC_TUNING);

	/*
	 * The Host Driver has exhausted the maximum number of loops allowed,
	 * so use fixed sampling frequency.
	 */
	if (!tuning_loop_counter || !timeout) {
		ctrl &= ~SDHCI_CTRL_TUNED_CLK;
		sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);
	} else {
		if (!(ctrl & SDHCI_CTRL_TUNED_CLK)) {
			pr_info(DRIVER_NAME ": Tuning procedure"
				" failed, falling back to fixed sampling"
				" clock\n");
			err = -EIO;
		} else {
			sdhci_config_tap(mmc, SAVE_TUNED_TAP);
			pr_info("%s: tap value and tuning window after hw tuning completion ...\n",
				mmc_hostname(mmc));
			/* log tap, trim and tuning windows */
			if (host->ops->dump_host_cust_regs)
				host->ops->dump_host_cust_regs(host);
		}
	}

out:
	/*
	 * If this is the very first time we are here, we start the retuning
	 * timer. Since only during the first time, SDHCI_NEEDS_RETUNING
	 * flag won't be set, we check this condition before actually starting
	 * the timer.
	 */
	if (!(host->flags & SDHCI_NEEDS_RETUNING) && host->tuning_count &&
	    (host->tuning_mode == SDHCI_TUNING_MODE_1)) {
		host->flags |= SDHCI_USING_RETUNING_TIMER;
		mod_timer(&host->tuning_timer, jiffies +
			host->tuning_count * HZ);
		/* Tuning mode 1 limits the maximum data length to 4MB */
		mmc->max_blk_count = (4 * 1024 * 1024) / mmc->max_blk_size;
	} else {
		host->flags &= ~SDHCI_NEEDS_RETUNING;
		/* Reload the new initial value for timer */
		if (host->tuning_mode == SDHCI_TUNING_MODE_1)
			mod_timer(&host->tuning_timer, jiffies +
				host->tuning_count * HZ);
	}

	/*
	 * In case tuning fails, host controllers which support re-tuning can
	 * try tuning again at a later time, when the re-tuning timer expires.
	 * So for these controllers, we return 0. Since there might be other
	 * controllers who do not have this capability, we return error for
	 * them. SDHCI_USING_RETUNING_TIMER means the host is currently using
	 * a retuning timer to do the retuning for the card.
	 */
	if (err && (host->flags & SDHCI_USING_RETUNING_TIMER))
		err = 0;

	sdhci_clear_set_irqs(host, SDHCI_INT_DATA_AVAIL, ier);
	spin_unlock(&host->lock);
	enable_irq(host->irq);
	sdhci_runtime_pm_put(host);

	return err;
}


static void sdhci_enable_preset_value(struct sdhci_host *host, bool enable)
{
	u16 ctrl;

	/* Host Controller v3.00 defines preset value registers */
	if (host->version < SDHCI_SPEC_300)
		return;

	ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);

	/*
	 * We only enable or disable Preset Value if they are not already
	 * enabled or disabled respectively. Otherwise, we bail out.
	 */
	if (enable && !(ctrl & SDHCI_CTRL_PRESET_VAL_ENABLE)) {
		ctrl |= SDHCI_CTRL_PRESET_VAL_ENABLE;
		sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);
		host->flags |= SDHCI_PV_ENABLED;
	} else if (!enable && (ctrl & SDHCI_CTRL_PRESET_VAL_ENABLE)) {
		ctrl &= ~SDHCI_CTRL_PRESET_VAL_ENABLE;
		sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);
		host->flags &= ~SDHCI_PV_ENABLED;
	}
}

static void sdhci_card_event(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);
	unsigned long flags;

	/* sdhci_runtime_pm_get cannot be called here since
	 * tasklet/softirq context cannot call
	 * sleeping function like __pm_runtime_resume
         */
	spin_lock_irqsave(&host->lock, flags);

	/* Check host->mrq_cmd first in case we are runtime suspended */
	if ((host->mrq_cmd || host->mrq_dat) &&
		/* TODO: check if clocks are already ON when
		 * mrq_cmd or mrq_dat are enabled
		 */
	    !(sdhci_readl(host, SDHCI_PRESENT_STATE) & SDHCI_CARD_PRESENT)) {
		pr_err("%s: Card removed during transfer!\n",
			mmc_hostname(host->mmc));
		pr_err("%s: Resetting controller.\n",
			mmc_hostname(host->mmc));

		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);

		if (host->mrq_cmd) {
			host->mrq_cmd->cmd->error = -ENOMEDIUM;
			if (MMC_CHECK_CMDQ_MODE(host))
				tasklet_schedule(&host->finish_cmd_tasklet);
			else
				tasklet_schedule(&host->finish_tasklet);
		}
		if (host->mrq_dat) {
			host->mrq_dat->cmd->error = -ENOMEDIUM;
			if (MMC_CHECK_CMDQ_MODE(host))
				tasklet_schedule(&host->finish_dat_tasklet);
			else
				tasklet_schedule(&host->finish_tasklet);
		}
	}

	spin_unlock_irqrestore(&host->lock, flags);
}

int sdhci_enable(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);

	if (!mmc->card || !(mmc->caps2 & MMC_CAP2_CLOCK_GATING))
		return 0;

	/* cancel delayed clk gate work */
	if (host->quirks2 & SDHCI_QUIRK2_DELAYED_CLK_GATE)
		cancel_delayed_work_sync(&host->delayed_clk_gate_wrk);

	sysedp_set_state(host->sysedpc, 1);

	if (mmc->ios.clock) {
		if (host->ops->set_clock)
			host->ops->set_clock(host, mmc->ios.clock);
		sdhci_set_clock(host, mmc->ios.clock);
	}

	return 0;
}

static void mmc_host_clk_gate(struct sdhci_host *host)
{
	sdhci_set_clock(host, 0);
	if (host->ops->set_clock)
		host->ops->set_clock(host, 0);

	sysedp_set_state(host->sysedpc, 0);

	return;
}

void delayed_clk_gate_cb(struct work_struct *work)
{
	struct sdhci_host *host = container_of(work, struct sdhci_host,
					      delayed_clk_gate_wrk.work);

	/* power off check */
	if (host->mmc->ios.power_mode == MMC_POWER_OFF)
		goto end;

	mmc_host_clk_gate(host);
end:
	return;
}
EXPORT_SYMBOL_GPL(delayed_clk_gate_cb);

int sdhci_disable(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);

	if (!mmc->card || !(mmc->caps2 & MMC_CAP2_CLOCK_GATING))
		return 0;

	if (IS_DELAYED_CLK_GATE(host)) {
		if (host->is_clk_on) {
			if (IS_SDIO_CARD(host))
				host->clk_gate_tmout_ticks =
					SDIO_CLK_GATING_TICK_TMOUT;
			else if (IS_EMMC_CARD(host))
				host->clk_gate_tmout_ticks =
					EMMC_CLK_GATING_TICK_TMOUT;
			if (host->clk_gate_tmout_ticks > 0)
				schedule_delayed_work(
					&host->delayed_clk_gate_wrk,
					host->clk_gate_tmout_ticks);
		}
		return 0;
	}

	mmc_host_clk_gate(host);

	return 0;
}

#ifdef CONFIG_MMC_FREQ_SCALING
/*
 * Wrapper functions to call any platform specific implementation for
 * supporting dynamic frequency scaling for SD/MMC devices.
 */
static int sdhci_gov_get_target(struct mmc_host *mmc, unsigned long *freq)
{
	struct sdhci_host *host = mmc_priv(mmc);

	if (host->ops->dfs_gov_get_target_freq)
		*freq = host->ops->dfs_gov_get_target_freq(host,
			mmc->devfreq_stats);

	return 0;
}

static int sdhci_gov_init(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);

	if (host->ops->dfs_gov_init)
		return host->ops->dfs_gov_init(host);

	return 0;
}

static void sdhci_gov_exit(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);

	if (host->ops->dfs_gov_exit)
		host->ops->dfs_gov_exit(host);
}
#endif

static int sdhci_select_drive_strength(struct mmc_host *mmc,
				       unsigned int max_dtr,
				       int host_drv,
				       int card_drv)
{
	struct sdhci_host *host = mmc_priv(mmc);
	unsigned char	drv_type;

	/* return default strength if no handler in driver */
	if (!host->ops->get_drive_strength)
		return MMC_SET_DRIVER_TYPE_B;

	drv_type = host->ops->get_drive_strength(host, max_dtr,
			host_drv, card_drv);

	if (drv_type > MMC_SET_DRIVER_TYPE_D) {
		pr_err("%s: Error on getting drive strength. Got drv_type %d\n"
			, mmc_hostname(host->mmc), drv_type);
		return MMC_SET_DRIVER_TYPE_B;
	}

	return drv_type;
}
static void sdhci_init_card(struct mmc_host *mmc, struct mmc_card *card)
{
	struct sdhci_host *host = mmc_priv(mmc);

	/*
	 * Get the max pio transfer limits if defined. This would be used to
	 * dynamically choose between dma and pio modes depending on the
	 * transfer parameters.
	 */
	if (host->ops->get_max_pio_transfer_limits)
		host->ops->get_max_pio_transfer_limits(host);
}
static const struct mmc_host_ops sdhci_ops = {
	.request	= sdhci_request,
	.set_ios	= sdhci_set_ios,
	.get_cd		= sdhci_get_cd,
	.get_ro		= sdhci_get_ro,
	.hw_reset	= sdhci_hw_reset,
	.enable		= sdhci_enable,
	.disable	= sdhci_disable,
	.enable_sdio_irq = sdhci_enable_sdio_irq,
	.start_signal_voltage_switch	= sdhci_start_signal_voltage_switch,
	.execute_tuning			= sdhci_execute_tuning,
	.validate_sd2_0			= sdhci_validate_sd2_0,
	.card_event			= sdhci_card_event,
	.card_busy	= sdhci_card_busy,
#ifdef CONFIG_MMC_FREQ_SCALING
	.dfs_governor_init		= sdhci_gov_init,
	.dfs_governor_exit		= sdhci_gov_exit,
	.dfs_governor_get_target	= sdhci_gov_get_target,
#endif
	.select_drive_strength		= sdhci_select_drive_strength,
	.post_init	= sdhci_post_init,
	.init_card	= sdhci_init_card,
};

/*****************************************************************************\
 *                                                                           *
 * Tasklets                                                                  *
 *                                                                           *
\*****************************************************************************/

static void sdhci_tasklet_card(unsigned long param)
{
	struct sdhci_host *host = (struct sdhci_host *)param;

	sdhci_card_event(host->mmc);
	if (host->detect_resume)
		mmc_detect_change(host->mmc, msecs_to_jiffies(700));
	else
		mmc_detect_change(host->mmc, msecs_to_jiffies(200));
}

static void sdhci_tasklet_finish(unsigned long param)
{
	struct sdhci_host *host;
	unsigned long flags;
	struct mmc_request *mrq = NULL;

	host = (struct sdhci_host *)param;

	spin_lock_irqsave(&host->lock, flags);

	/*
	 * If this tasklet gets rescheduled while running, it will
	 * be run again afterwards but without any active request.
	 */
	if (!host->mrq_cmd && !host->mrq_dat) {
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}

	del_timer(&host->timer);

	if (host->mrq_cmd)
		mrq = host->mrq_cmd;
	else if (host->mrq_dat)
		mrq = host->mrq_dat;

	/*
	 * The controller needs a reset of internal state machines
	 * upon error conditions.
	 */
	if (!(host->flags & SDHCI_DEVICE_DEAD) &&
	    ((mrq->cmd && mrq->cmd->error) ||
		 (mrq->data && (mrq->data->error ||
		  (mrq->data->stop && mrq->data->stop->error))) ||
		   (host->quirks & SDHCI_QUIRK_RESET_AFTER_REQUEST))) {

		/* Some controllers need this kick or reset won't work here */
		if (host->quirks & SDHCI_QUIRK_CLOCK_BEFORE_RESET)
			/* This is to force an update */
			sdhci_update_clock(host);

		/* Spec says we should do both at the same time */
		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
	}

	host->mrq_cmd = NULL;
	host->mrq_dat = NULL;
	host->cmd = NULL;
	host->data = NULL;

#ifndef SDHCI_USE_LEDS_CLASS
	sdhci_deactivate_led(host);
#endif

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);

	mmc_request_done(host->mmc, mrq);
#ifndef CONFIG_MMC_CQ
	sdhci_runtime_pm_put(host);
#endif
}

/*
 * This tasklet gets scheduled to handle CMD only requests in CQ.
 */
static void sdhci_tasklet_cmd_finish(unsigned long param)
{
	struct sdhci_host *host;
	unsigned long flags;
	struct mmc_request *mrq;

	host = (struct sdhci_host *)param;

	if (!host->mrq_cmd && host->mrq_dat) {
		mmc_handle_queued_request(host->mmc, MMC_HANDLE_CLR_CMD);
		return;
	}

	spin_lock_irqsave(&host->lock, flags);

	/*
	 * If this tasklet gets rescheduled while running, it will
	 * be run again afterwards but without any active request.
	 */
	if (!host->mrq_cmd) {
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}

	del_timer(&host->timer);

	mrq = host->mrq_cmd;

	/*
	 * The controller needs a reset of internal state machines
	 * upon error conditions.
	 */
	if (!(host->flags & SDHCI_DEVICE_DEAD) &&
	    ((mrq->cmd && mrq->cmd->error) ||
		 (mrq->data && (mrq->data->error ||
		  (mrq->data->stop && mrq->data->stop->error))) ||
		   (host->quirks & SDHCI_QUIRK_RESET_AFTER_REQUEST))) {

		/* Some controllers need this kick or reset won't work here */
		if (host->quirks & SDHCI_QUIRK_CLOCK_BEFORE_RESET)
			/* This is to force an update */
			sdhci_update_clock(host);

		sdhci_reset(host, SDHCI_RESET_CMD);
	}

	host->mrq_cmd = NULL;
	host->cmd = NULL;

#ifndef SDHCI_USE_LEDS_CLASS
	sdhci_deactivate_led(host);
#endif

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);

	mmc_request_done(host->mmc, mrq);
#ifndef CONFIG_MMC_CQ
	sdhci_runtime_pm_put(host);
#endif
}

/*
 * This tasklet gets scheduled to handle CMD with DATA requests in CQ.
 */
static void sdhci_tasklet_dat_finish(unsigned long param)
{
	struct sdhci_host *host;
	unsigned long flags;
	struct mmc_request *mrq;

	host = (struct sdhci_host *)param;

	spin_lock_irqsave(&host->lock, flags);

	/*
	 * If this tasklet gets rescheduled while running, it will
	 * be run again afterwards but without any active request.
	 */
	if (!host->mrq_dat) {
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}

	del_timer(&host->timer);

	mrq = host->mrq_dat;

	if (host->data_early)
		mrq->data_early = 1;

	/*
	 * The controller needs a reset of internal state machines
	 * upon error conditions.
	 */
	if (!(host->flags & SDHCI_DEVICE_DEAD) &&
	    ((mrq->cmd && mrq->cmd->error) ||
		 (mrq->data && (mrq->data->error ||
		  (mrq->data->stop && mrq->data->stop->error))) ||
		   (host->quirks & SDHCI_QUIRK_RESET_AFTER_REQUEST))) {

		/* Some controllers need this kick or reset won't work here */
		if (host->quirks & SDHCI_QUIRK_CLOCK_BEFORE_RESET)
			/* This is to force an update */
			sdhci_update_clock(host);

		sdhci_reset(host, SDHCI_RESET_DATA);
	}

	host->mrq_dat = NULL;
	host->data = NULL;

#ifndef SDHCI_USE_LEDS_CLASS
	sdhci_deactivate_led(host);
#endif

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);

	mmc_request_done(host->mmc, mrq);
#ifndef CONFIG_MMC_CQ
	sdhci_runtime_pm_put(host);
#endif
}

static void sdhci_timeout_timer(unsigned long data)
{
	struct sdhci_host *host;
	unsigned long flags;

	host = (struct sdhci_host *)data;

	spin_lock_irqsave(&host->lock, flags);

	if (host->mrq_cmd || host->mrq_dat) {
		pr_err("%s: Timeout waiting for hardware "
			"interrupt.\n", mmc_hostname(host->mmc));
		sdhci_dumpregs(host);

		if (host->data) {
			host->data->error = -ETIMEDOUT;
			sdhci_finish_data(host);
		} else {
			if (host->cmd)
				host->cmd->error = -ETIMEDOUT;
			else if (host->mrq_dat)
				host->mrq_dat->cmd->error = -ETIMEDOUT;

			if (MMC_CHECK_CMDQ_MODE(host))
				tasklet_schedule(&host->finish_cmd_tasklet);
			else
				tasklet_schedule(&host->finish_tasklet);
		}
	}

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);
}

static void sdhci_tuning_timer(unsigned long data)
{
	struct sdhci_host *host;
	unsigned long flags;

	host = (struct sdhci_host *)data;

	spin_lock_irqsave(&host->lock, flags);

	host->flags |= SDHCI_NEEDS_RETUNING;

	spin_unlock_irqrestore(&host->lock, flags);
}

/*****************************************************************************\
 *                                                                           *
 * Interrupt handling                                                        *
 *                                                                           *
\*****************************************************************************/

static void sdhci_cmd_irq(struct sdhci_host *host, u32 intmask)
{
	bool skip_dump = false;

	BUG_ON(intmask == 0);

	if (!host->cmd) {
		pr_err("%s: Got command interrupt 0x%08x even "
			"though no command operation was in progress.\n",
			mmc_hostname(host->mmc), (unsigned)intmask);
		sdhci_dumpregs(host);
		return;
	}

	if (intmask & SDHCI_INT_TIMEOUT) {
		host->cmd->error = -ETIMEDOUT;
	} else if (intmask & (SDHCI_INT_CRC | SDHCI_INT_END_BIT |
			SDHCI_INT_INDEX)) {
		host->cmd->error = -EILSEQ;

		if (host->ops->skip_register_dump)
			skip_dump = host->ops->skip_register_dump(host);
		if (skip_dump &&
			(intmask & SDHCI_INT_INDEX))
			goto lbl_suppress_dump;

		sdhci_dumpregs(host);
		if (intmask & SDHCI_INT_INDEX)
			pr_err("%s: Command INDEX error, intmask: %x Interface clock = %uHz\n",
			mmc_hostname(host->mmc), intmask, host->max_clk);
		else if (intmask & SDHCI_INT_CRC)
			pr_err("%s: Command CRC error, intmask: %x Interface clock = %uHz\n",
			mmc_hostname(host->mmc), intmask, host->max_clk);
		else if (intmask & SDHCI_INT_END_BIT)
			pr_err("%s: Command END BIT error, intmask: %x Interface clock = %uHz\n",
			mmc_hostname(host->mmc), intmask, host->max_clk);
	}

lbl_suppress_dump:
	if (host->cmd->error) {
		if (MMC_CHECK_CMDQ_MODE(host))
			tasklet_schedule(&host->finish_cmd_tasklet);
		else
			tasklet_schedule(&host->finish_tasklet);
		return;
	}

	/*
	 * The host can send and interrupt when the busy state has
	 * ended, allowing us to wait without wasting CPU cycles.
	 * Unfortunately this is overloaded on the "data complete"
	 * interrupt, so we need to take some care when handling
	 * it.
	 *
	 * Note: The 1.0 specification is a bit ambiguous about this
	 *       feature so there might be some problems with older
	 *       controllers.
	 */
	if (host->cmd->flags & MMC_RSP_BUSY) {
		if (host->cmd->data)
			DBG("Cannot wait for busy signal when also "
				"doing a data transfer");
		else if (!(host->quirks & SDHCI_QUIRK_NO_BUSY_IRQ))
			return;

		/* The controller does not support the end-of-busy IRQ,
		 * fall through and take the SDHCI_INT_RESPONSE */
	}

	if (intmask & SDHCI_INT_RESPONSE)
		sdhci_finish_command(host);
}

#ifdef CONFIG_MMC_DEBUG
static void sdhci_show_adma_error(struct sdhci_host *host)
{
	const char *name = mmc_hostname(host->mmc);
	u8 *desc = host->adma_desc;
	__le32 *dma;
	__le16 *len;
	u8 attr;

	u32 ctrl;
	int next_desc;
	ctrl = sdhci_readl(host, SDHCI_ACMD12_ERR);
	if (ctrl & SDHCI_ADDRESSING_64BIT_EN) {
		if (ctrl & SDHCI_HOST_VERSION_4_EN)
			next_desc = 16;
		else
			next_desc = 12;
	} else {
		/* 32 bit DMA mode supported*/
		next_desc = 8;
	}

	sdhci_dumpregs(host);

	while (true) {
		dma = (__le32 *)(desc + 4);
		len = (__le16 *)(desc + 2);
		attr = *desc;

		if (next_desc == 8) {
			DBG("%s: %p: DMA-32 0x%08x, LEN 0x%04x, Attr=0x%02x\n",
				name, desc, le32_to_cpu(*dma), le16_to_cpu(*len), attr);
		} else if (next_desc == 16) {
			DBG("%s: %p: DMA-64 0x%16x, LEN 0x%04x, Attr=0x%02x\n",
				name, desc, le64_to_cpu(*((__le64 *)dma)), le16_to_cpu(*len), attr);
		}
		desc += next_desc;
		if (attr & 2)
			break;
	}
}
#else
static void sdhci_show_adma_error(struct sdhci_host *host) { }
#endif

static void sdhci_data_irq(struct sdhci_host *host, u32 intmask)
{
	u32 command;
	BUG_ON(intmask == 0);

	/* CMD19, CMD21 generates _only_ Buffer Read Ready interrupt */
	if (intmask & SDHCI_INT_DATA_AVAIL) {
		command = SDHCI_GET_CMD(sdhci_readw(host, SDHCI_COMMAND));
		if (command == MMC_SEND_TUNING_BLOCK ||
		    command == MMC_SEND_TUNING_BLOCK_HS200) {
			host->tuning_done = 1;
			wake_up(&host->buf_ready_int);
			return;
		}
	}

	if (!host->data) {
		/*
		 * The "data complete" interrupt is also used to
		 * indicate that a busy state has ended. See comment
		 * above in sdhci_cmd_irq().
		 */
		if (host->cmd && (host->cmd->flags & MMC_RSP_BUSY)) {
			if (intmask & SDHCI_INT_DATA_END) {
				sdhci_finish_command(host);
				return;
			}
		}

		pr_err("%s: Got data interrupt 0x%08x even "
			"though no data operation was in progress.\n",
			mmc_hostname(host->mmc), (unsigned)intmask);
		sdhci_dumpregs(host);

		return;
	}

	if (intmask & SDHCI_INT_DATA_TIMEOUT) {
		host->data->error = -ETIMEDOUT;
		pr_err("%s: Data Timeout error, intmask: %x Interface clock = %uHz\n",
			mmc_hostname(host->mmc), intmask, host->max_clk);
		sdhci_dumpregs(host);
	} else if (intmask & SDHCI_INT_DATA_END_BIT) {
		host->data->error = -EILSEQ;
		pr_err("%s: Data END Bit error, intmask: %x Interface clock = %uHz\n",
			mmc_hostname(host->mmc), intmask, host->max_clk);
	} else if ((intmask & SDHCI_INT_DATA_CRC) &&
		SDHCI_GET_CMD(sdhci_readw(host, SDHCI_COMMAND))
			!= MMC_BUS_TEST_R) {
		host->data->error = -EILSEQ;
		pr_err("%s: Data CRC error, intmask: %x Interface clock = %uHz\n",
			mmc_hostname(host->mmc), intmask, host->max_clk);
		sdhci_dumpregs(host);
	} else if (intmask & SDHCI_INT_ADMA_ERROR) {
		pr_err("%s: ADMA error\n", mmc_hostname(host->mmc));
		sdhci_dumpregs(host);
		sdhci_show_adma_error(host);
		host->data->error = -EIO;
		if (host->ops->adma_workaround)
			host->ops->adma_workaround(host, intmask);
	}

	if (host->data->error)
		sdhci_finish_data(host);
	else {
		if (intmask & (SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL))
			sdhci_transfer_pio(host);

		/*
		 * We currently don't do anything fancy with DMA
		 * boundaries, but as we can't disable the feature
		 * we need to at least restart the transfer.
		 *
		 * According to the spec sdhci_readl(host, SDHCI_DMA_ADDRESS)
		 * should return a valid address to continue from, but as
		 * some controllers are faulty, don't trust them.
		 */
		if (intmask & SDHCI_INT_DMA_END) {
			u32 dmastart, dmanow;
			dmastart = sg_dma_address(host->data->sg);
			dmanow = dmastart + host->data->bytes_xfered;
			/*
			 * Force update to the next DMA block boundary.
			 */
			dmanow = (dmanow &
				~(SDHCI_DEFAULT_BOUNDARY_SIZE - 1)) +
				SDHCI_DEFAULT_BOUNDARY_SIZE;
			host->data->bytes_xfered = dmanow - dmastart;
			DBG("%s: DMA base 0x%08x, transferred 0x%06x bytes,"
				" next 0x%08x\n",
				mmc_hostname(host->mmc), dmastart,
				host->data->bytes_xfered, dmanow);
			sdhci_writel(host, dmanow, SDHCI_DMA_ADDRESS);
		}

		if (intmask & SDHCI_INT_DATA_END) {
			if ((!MMC_CHECK_CMDQ_MODE(host) && host->cmd) ||
				(MMC_CHECK_CMDQ_MODE(host) && host->cmd && (host->mrq_dat->cmd == host->cmd))) {

				/*
				 * Data managed to finish before the
				 * command completed. Make sure we do
				 * things in the proper order.
				 */
				host->data_early = 1;
			} else
				sdhci_finish_data(host);
		}
	}
}

static irqreturn_t sdhci_irq(int irq, void *dev_id)
{
	irqreturn_t result;
	struct sdhci_host *host = dev_id;
	u32 intmask, unexpected = 0;
	int cardint = 0, max_loops = 16;

	spin_lock(&host->lock);

	if (host->runtime_suspended) {
		spin_unlock(&host->lock);
		pr_warning("%s: got irq while runtime suspended\n",
		       mmc_hostname(host->mmc));
		return IRQ_HANDLED;
	}

	intmask = sdhci_readl(host, SDHCI_INT_STATUS);

	if (!intmask || intmask == 0xffffffff) {
		result = IRQ_NONE;
		goto out;
	}

again:
	DBG("*** %s got interrupt: 0x%08x\n",
		mmc_hostname(host->mmc), intmask);

	if (intmask & (SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE)) {
		u32 present = sdhci_readl(host, SDHCI_PRESENT_STATE) &
			      SDHCI_CARD_PRESENT;

		/*
		 * There is a observation on i.mx esdhc.  INSERT bit will be
		 * immediately set again when it gets cleared, if a card is
		 * inserted.  We have to mask the irq to prevent interrupt
		 * storm which will freeze the system.  And the REMOVE gets
		 * the same situation.
		 *
		 * More testing are needed here to ensure it works for other
		 * platforms though.
		 */
		sdhci_mask_irqs(host, present ? SDHCI_INT_CARD_INSERT :
						SDHCI_INT_CARD_REMOVE);
		sdhci_unmask_irqs(host, present ? SDHCI_INT_CARD_REMOVE :
						  SDHCI_INT_CARD_INSERT);

		sdhci_writel(host, intmask & (SDHCI_INT_CARD_INSERT |
			     SDHCI_INT_CARD_REMOVE), SDHCI_INT_STATUS);
		intmask &= ~(SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE);
		tasklet_schedule(&host->card_tasklet);
	}

#ifdef CONFIG_CMD_DUMP
	if (mmc_hostname(host->mmc)[3] == '0')
		dbg_add_host_log(host->mmc, 7,  intmask, 0xffffffff);
#endif

	if (intmask & SDHCI_INT_CMD_MASK) {
		sdhci_writel(host, intmask & SDHCI_INT_CMD_MASK,
			SDHCI_INT_STATUS);
		sdhci_cmd_irq(host, intmask & SDHCI_INT_CMD_MASK);
	}

	if (intmask & SDHCI_INT_DATA_MASK) {
		sdhci_writel(host, intmask & SDHCI_INT_DATA_MASK,
			SDHCI_INT_STATUS);
		sdhci_data_irq(host, intmask & SDHCI_INT_DATA_MASK);
	}

	if (intmask & SDHCI_INT_RETUNING_EVENT)
		host->flags |= SDHCI_NEEDS_RETUNING;

	if ((intmask & SDHCI_INT_DATA_MASK) || (intmask & SDHCI_INT_CMD_MASK))
		if (host->ops->sd_error_stats)
			host->ops->sd_error_stats(host, intmask);

	intmask &= ~(SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK);

	intmask &= ~SDHCI_INT_ERROR;

	if (intmask & SDHCI_INT_BUS_POWER) {
		pr_err("%s: Current limit error, intmask: %x Interface clock = %uHz\n",
			mmc_hostname(host->mmc), intmask, host->max_clk);
		pr_err("%s: Card is consuming too much power!\n",
			mmc_hostname(host->mmc));
		sdhci_writel(host, SDHCI_INT_BUS_POWER, SDHCI_INT_STATUS);
	}

	/* print the errors based on the SD Host controller spec */
	if ((intmask & SDHCI_INT_TIMEOUT) || (intmask & SDHCI_INT_CRC)) {
		pr_err("%s: %s, intmask: %x Interface clock = %uHz\n",
			mmc_hostname(host->mmc),
			resp_error[RESP_ERROR_INDEX(intmask)],
			intmask, host->max_clk);
	}

	intmask &= ~SDHCI_INT_BUS_POWER;

	if (intmask & SDHCI_INT_CARD_INT)
		cardint = 1;

	intmask &= ~SDHCI_INT_CARD_INT;

	if (intmask) {
		unexpected |= intmask;
		sdhci_writel(host, intmask, SDHCI_INT_STATUS);
	}

	result = IRQ_HANDLED;

	intmask = sdhci_readl(host, SDHCI_INT_STATUS);
	if (intmask && --max_loops)
		goto again;
out:
	spin_unlock(&host->lock);

	if (unexpected) {
		pr_err("%s: Unexpected interrupt 0x%08x.\n",
			   mmc_hostname(host->mmc), unexpected);
		sdhci_dumpregs(host);
	}
	/*
	 * We have to delay this as it calls back into the driver.
	 */
	if (cardint)
		mmc_signal_sdio_irq(host->mmc);

	return result;
}

/*****************************************************************************\
 *                                                                           *
 * Suspend/resume                                                            *
 *                                                                           *
\*****************************************************************************/

#ifdef CONFIG_PM
void sdhci_enable_irq_wakeups(struct sdhci_host *host)
{
	u8 val;
	u8 mask = SDHCI_WAKE_ON_INSERT | SDHCI_WAKE_ON_REMOVE
			| SDHCI_WAKE_ON_INT;

	val = sdhci_readb(host, SDHCI_WAKE_UP_CONTROL);
	val |= mask ;
	/* Avoid fake wake up */
	if (host->quirks & SDHCI_QUIRK_BROKEN_CARD_DETECTION)
		val &= ~(SDHCI_WAKE_ON_INSERT | SDHCI_WAKE_ON_REMOVE);
	sdhci_writeb(host, val, SDHCI_WAKE_UP_CONTROL);
}
EXPORT_SYMBOL_GPL(sdhci_enable_irq_wakeups);

void sdhci_disable_irq_wakeups(struct sdhci_host *host)
{
	u8 val;
	u8 mask = SDHCI_WAKE_ON_INSERT | SDHCI_WAKE_ON_REMOVE
			| SDHCI_WAKE_ON_INT;

	val = sdhci_readb(host, SDHCI_WAKE_UP_CONTROL);
	val &= ~mask;
	sdhci_writeb(host, val, SDHCI_WAKE_UP_CONTROL);
}
EXPORT_SYMBOL_GPL(sdhci_disable_irq_wakeups);

int sdhci_suspend_host(struct sdhci_host *host)
{
	int ret;
	struct mmc_host *mmc = host->mmc;

	/* we need to enable clocks during
	 * mmc_suspend_host for SDHCI_QUIRK2_NON_STD_RTPM
	 */
	if (!(host->quirks2 & SDHCI_QUIRK2_NON_STD_RTPM))
		host->suspend_task = current;

	if (host->ops->platform_suspend)
		host->ops->platform_suspend(host);

	sdhci_disable_card_detection(host);

	/* Disable tuning since we are suspending */
	if (host->flags & SDHCI_USING_RETUNING_TIMER) {
		del_timer_sync(&host->tuning_timer);
		host->flags &= ~SDHCI_NEEDS_RETUNING;
	}

	/*
	 * If eMMC cards are put in sleep state, Vccq can be disabled
	 * but Vcc would still be powered on. In resume, we only restore
	 * the controller context. So, set MMC_PM_KEEP_POWER flag.
	 */
	if (mmc_card_can_sleep(mmc) && !(mmc->caps2 & MMC_CAP2_NO_SLEEP_CMD))
		mmc->pm_flags |= MMC_PM_KEEP_POWER;

	ret = mmc_suspend_host(host->mmc);
	if (ret) {
		if (host->flags & SDHCI_USING_RETUNING_TIMER) {
			host->flags |= SDHCI_NEEDS_RETUNING;
			mod_timer(&host->tuning_timer, jiffies +
					host->tuning_count * HZ);
		}

		sdhci_enable_card_detection(host);

		if (!(host->quirks2 & SDHCI_QUIRK2_NON_STD_RTPM))
			host->suspend_task = NULL;
		return ret;
	}
	/* cancel delayed clk gate work */
	if (host->quirks2 & SDHCI_QUIRK2_DELAYED_CLK_GATE)
		cancel_delayed_work_sync(&host->delayed_clk_gate_wrk);

	/*
	 * If host clock is disabled but the register access requires host
	 * clock, then enable the clock, mask the interrupts and disable
	 * the clock.
	 */
	if (host->quirks2 & SDHCI_QUIRK2_REG_ACCESS_REQ_HOST_CLK)
		if ((!host->clock && host->ops->set_clock) &&
			(host->quirks2 & SDHCI_QUIRK2_DELAYED_CLK_GATE))
			host->ops->set_clock(host, max(mmc->ios.clock, mmc->f_min));

	if (mmc->pm_flags & MMC_PM_KEEP_POWER)
		host->card_int_set = host->ier &
			SDHCI_INT_CARD_INT;

	if (!device_may_wakeup(mmc_dev(host->mmc))) {
		sdhci_mask_irqs(host, SDHCI_INT_ALL_MASK);

		if (host->quirks2 & SDHCI_QUIRK2_REG_ACCESS_REQ_HOST_CLK)
			if ((!host->clock && host->ops->set_clock) &&
			(host->quirks2 & SDHCI_QUIRK2_DELAYED_CLK_GATE))
				host->ops->set_clock(host, 0);

		if (host->irq)
			disable_irq(host->irq);
	} else {
		sdhci_enable_irq_wakeups(host);
		enable_irq_wake(host->irq);

		if (host->quirks2 & SDHCI_QUIRK2_REG_ACCESS_REQ_HOST_CLK)
			if ((!host->clock && host->ops->set_clock) &&
			(host->quirks2 & SDHCI_QUIRK2_DELAYED_CLK_GATE))
				host->ops->set_clock(host, 0);
	}

	if (!(host->quirks2 & SDHCI_QUIRK2_NON_STD_RTPM))
		host->suspend_task = NULL;

	return ret;
}

EXPORT_SYMBOL_GPL(sdhci_suspend_host);

int sdhci_resume_host(struct sdhci_host *host)
{
	int ret;
	struct mmc_host *mmc = host->mmc;

	host->suspend_task = current;


	if (host->flags & (SDHCI_USE_SDMA | SDHCI_USE_ADMA)) {
		if (host->ops->enable_dma)
			host->ops->enable_dma(host);
	}

	if (!device_may_wakeup(mmc_dev(host->mmc))) {
		if (host->irq)
			enable_irq(host->irq);
	} else {
		sdhci_disable_irq_wakeups(host);
		disable_irq_wake(host->irq);
	}

	if ((host->mmc->pm_flags & MMC_PM_KEEP_POWER) &&
	    (host->quirks2 & SDHCI_QUIRK2_HOST_OFF_CARD_ON)) {
		/* Card keeps power but host controller does not */
		sdhci_init(host, 0);
		host->pwr = 0;
		host->clock = 0;
		sdhci_do_set_ios(host, &host->mmc->ios);
	} else {
		sdhci_init(host, (host->mmc->pm_flags & MMC_PM_KEEP_POWER));
		mmiowb();
	}

	ret = mmc_resume_host(host->mmc);
	/* Enable card interrupt as it is overwritten in sdhci_init */
	if ((mmc->caps & MMC_CAP_SDIO_IRQ) &&
		(mmc->pm_flags & MMC_PM_KEEP_POWER))
			if (host->card_int_set)
				mmc->ops->enable_sdio_irq(mmc, true);

	sdhci_enable_card_detection(host);

	if (host->ops->platform_resume)
		host->ops->platform_resume(host);

	/* Set the re-tuning expiration flag */
	if (host->flags & SDHCI_USING_RETUNING_TIMER)
		host->flags |= SDHCI_NEEDS_RETUNING;

	host->suspend_task = NULL;

	return ret;
}

EXPORT_SYMBOL_GPL(sdhci_resume_host);
#endif /* CONFIG_PM */

#ifdef CONFIG_PM_RUNTIME

static int sdhci_runtime_pm_get(struct sdhci_host *host)
{
	int present;

	if (!(host->quirks2 & SDHCI_QUIRK2_MMC_RTPM))
		return 0;

	present = mmc_gpio_get_cd(host->mmc);
	if (present < 0) {
		/* If polling, assume that the card is always present. */
		if (host->quirks & SDHCI_QUIRK_BROKEN_CARD_DETECTION)
			if (host->ops->get_cd)
				present = host->ops->get_cd(host);
			else
				present = 1;
		else
			present = sdhci_readl(host, SDHCI_PRESENT_STATE) &
					SDHCI_CARD_PRESENT;
	}

	if ((present && !host->mmc->card && (host->runtime_suspended == false))
					|| host->suspend_task == current) {
		pm_runtime_get_noresume(host->mmc->parent);
		return 0;
	}

	return pm_runtime_get_sync(host->mmc->parent);
}

static int sdhci_runtime_pm_put(struct sdhci_host *host)
{
	int present;

	if (!(host->quirks2 & SDHCI_QUIRK2_MMC_RTPM))
		return 0;

	present = mmc_gpio_get_cd(host->mmc);
	if (present < 0) {
		/* If polling, assume that the card is always present. */
		if (host->quirks & SDHCI_QUIRK_BROKEN_CARD_DETECTION)
			if (host->ops->get_cd)
				present = host->ops->get_cd(host);
			else
				present = 1;
		else
			present = sdhci_readl(host, SDHCI_PRESENT_STATE) &
					SDHCI_CARD_PRESENT;
	}
	if ((present && !host->mmc->card) || host->suspend_task == current) {
		pm_runtime_mark_last_busy(host->mmc->parent);
		pm_runtime_put_noidle(host->mmc->parent);
		return 0;
	}

	pm_runtime_mark_last_busy(host->mmc->parent);
	return pm_runtime_put_autosuspend(host->mmc->parent);
}

int sdhci_runtime_suspend_host(struct sdhci_host *host)
{
	unsigned long flags;
	int ret = 0;

	if (!(host->quirks2 & SDHCI_QUIRK2_MMC_RTPM))
		return 0;

	if (host->quirks2 & SDHCI_QUIRK2_NON_STD_RTPM) {
		spin_lock_irqsave(&host->lock, flags);
		host->runtime_suspended = true;
		spin_unlock_irqrestore(&host->lock, flags);

		sdhci_set_clock(host, 0);
		if (host->ops->set_clock)
			host->ops->set_clock(host, 0);
		sysedp_set_state(host->sysedpc, 0);
		goto lbl_end;
	}

	/* Disable tuning since we are suspending */
	if (host->flags & SDHCI_USING_RETUNING_TIMER) {
		del_timer_sync(&host->tuning_timer);
		host->flags &= ~SDHCI_NEEDS_RETUNING;
	}

	if (host->ops->set_clock)
		host->ops->set_clock(host, host->mmc->f_min);
	sdhci_set_clock(host, host->mmc->f_min);

	spin_lock_irqsave(&host->lock, flags);
	sdhci_mask_irqs(host, SDHCI_INT_ALL_MASK);
	spin_unlock_irqrestore(&host->lock, flags);

	synchronize_irq(host->irq);

	spin_lock_irqsave(&host->lock, flags);
	host->runtime_suspended = true;
	spin_unlock_irqrestore(&host->lock, flags);

	sdhci_set_clock(host, 0);
	if (host->ops->set_clock)
		host->ops->set_clock(host, 0);

lbl_end:
	return ret;
}
EXPORT_SYMBOL_GPL(sdhci_runtime_suspend_host);

int sdhci_runtime_resume_host(struct sdhci_host *host)
{
	unsigned long flags;
	int ret = 0, host_flags = host->flags;
	unsigned int freq;

	if (!(host->quirks2 & SDHCI_QUIRK2_MMC_RTPM))
		return 0;

	if (host->quirks2 & SDHCI_QUIRK2_NON_STD_RTPM) {
		if (host->mmc->ios.clock) {
			freq = host->mmc->ios.clock;
		} else {
			if (!host->mmc->f_min)
				host->mmc->f_min = MIN_SDMMC_FREQ;
			freq = host->mmc->f_min;
			host->clock = freq;
		}

		if (host->ops->set_clock)
			host->ops->set_clock(host, freq);
		sdhci_set_clock(host, freq);

		sysedp_set_state(host->sysedpc, 1);
		spin_lock_irqsave(&host->lock, flags);
		host->runtime_suspended = false;
		spin_unlock_irqrestore(&host->lock, flags);
		goto lbl_end;
	}

	if (host->ops->set_clock)
		host->ops->set_clock(host, host->mmc->f_min);
	sdhci_set_clock(host, host->mmc->f_min);

	if (host_flags & (SDHCI_USE_SDMA | SDHCI_USE_ADMA)) {
		if (host->ops->enable_dma)
			host->ops->enable_dma(host);
	}

	sdhci_init(host, 0);

	/* Force clock and power re-program */
	host->pwr = 0;
	host->clock = 0;
	sdhci_do_set_ios(host, &host->mmc->ios);

	if (host->mmc->ios.clock) {
		sdhci_do_start_signal_voltage_switch(host, &host->mmc->ios);
	/* Do any post voltage switch platform specific configuration */
		if (host->ops->switch_signal_voltage_exit)
			host->ops->switch_signal_voltage_exit(host,
				host->mmc->ios.signal_voltage);
	}

	if ((host_flags & SDHCI_PV_ENABLED) &&
		!(host->quirks2 & SDHCI_QUIRK2_PRESET_VALUE_BROKEN)) {
		spin_lock_irqsave(&host->lock, flags);
		sdhci_enable_preset_value(host, true);
		spin_unlock_irqrestore(&host->lock, flags);
	}

	/* Set the re-tuning expiration flag */
	if (host->flags & SDHCI_USING_RETUNING_TIMER)
		host->flags |= SDHCI_NEEDS_RETUNING;

	spin_lock_irqsave(&host->lock, flags);

	host->runtime_suspended = false;

	/* Enable SDIO IRQ */
	if ((host->flags & SDHCI_SDIO_IRQ_ENABLED))
		sdhci_enable_sdio_irq_nolock(host, true);

	/* Enable Card Detection */
	sdhci_enable_card_detection(host);

	spin_unlock_irqrestore(&host->lock, flags);

lbl_end:
	return ret;
}
EXPORT_SYMBOL_GPL(sdhci_runtime_resume_host);

#endif

/*****************************************************************************\
 *                                                                           *
 * Device allocation/registration                                            *
 *                                                                           *
\*****************************************************************************/

struct sdhci_host *sdhci_alloc_host(struct device *dev,
	size_t priv_size)
{
	struct mmc_host *mmc;
	struct sdhci_host *host;

	WARN_ON(dev == NULL);

	mmc = mmc_alloc_host(sizeof(struct sdhci_host) + priv_size, dev);
	if (!mmc)
		return ERR_PTR(-ENOMEM);

	host = mmc_priv(mmc);
	host->mmc = mmc;

	return host;
}

EXPORT_SYMBOL_GPL(sdhci_alloc_host);

#ifdef CONFIG_DEBUG_FS
static int show_sdhci_perf_stats(struct seq_file *s, void *data)
{
	struct sdhci_host *host = s->private;
	int i;
	u32 avg_perf2;
	u32 last_perf_in_class;
	struct data_stat_entry *stat = NULL;
	char buf[250];
	u64 total_rd_bytes;
	u64 total_wr_bytes;
	u64 total_rd_usecs;
	u64 total_wr_usecs;
	unsigned int overall_avg_rd_perf2;
	unsigned int overall_avg_wr_perf2;
	int rd_percent, wr_percent;

	seq_printf(s, "SDHCI(%s): perf statistics stat_size=%d\n",
		mmc_hostname(host->mmc),
		host->sdhci_data_stat.stat_size
		);
	if (host->sdhci_data_stat.stat_size) {
		seq_printf(s, "SDHCI(%s): perf statistics:\n",
			mmc_hostname(host->mmc));
		seq_puts(s,
		"Note: Performance figures in kilo bits per sec(kbps)\n");
		seq_puts(s,
		"S.No.    Block       Direction    Num blks/        Total     Total           Total          Last            Last usec          Avg kbps        Last kbps           Min kbps   Max kbps\n");
		seq_puts(s,
		"         Size        (R/W)        transfer         Bytes     Transfers       Time(usec)     Bytes           Duration           Perf            Perf                Perf       Perf\n");
	}
	total_rd_bytes = 0;
	total_wr_bytes = 0;
	total_rd_usecs = 0;
	total_wr_usecs = 0;
	for (i = 0; i < host->sdhci_data_stat.stat_size; i++) {
		if (!stat)
			stat = host->sdhci_data_stat.head;
		else
			stat = stat->next;
		if (!stat) {
			pr_err("%s %s: sdhci data stat head NULL i=%d\n",
				mmc_hostname(host->mmc), __func__, i);
			break;
		}
		sdhci_div64(
			((stat->total_bytes << 3) * 1000),
			stat->total_usecs, &avg_perf2);
		sdhci_div32(
			(((u32)stat->current_transferred_bytes << 3) * 1000),
			stat->duration_usecs,
			&last_perf_in_class);
		if (stat->is_read) {
			total_rd_bytes += stat->total_bytes;
			total_rd_usecs += stat->total_usecs;
		} else {
			total_wr_bytes += stat->total_bytes;
			total_wr_usecs += stat->total_usecs;
		}
		snprintf(buf, 250,
			"%2d    %4d           %c       %8d    %16lld    %8d        %16lld    %8d            %8d           %8d         %8d         %8d    %8d\n",
			(i + 1),
			stat->stat_blk_size,
			stat->is_read ? 'R' : 'W',
			stat->stat_blks_per_transfer,
			stat->total_bytes,
			stat->total_transfers,
			stat->total_usecs,
			stat->current_transferred_bytes,
			stat->duration_usecs,
			avg_perf2,
			last_perf_in_class,
			stat->min_kbps,
			stat->max_kbps
			);
		seq_puts(s, buf);
	}
	sdhci_div64(
		((total_rd_bytes << 3) * 1000),
		total_rd_usecs, &overall_avg_rd_perf2);
	sdhci_div64(
		(total_rd_bytes * 1000),
		(total_rd_bytes + total_wr_bytes), &rd_percent);
	snprintf(buf, 250,
		"Read Total_bytes=%lldB, time=%lldusecs, overall kbps=%d Rd percent=%d.%d\n",
		total_rd_bytes, total_rd_usecs,
		overall_avg_rd_perf2,
		(rd_percent / 10), (rd_percent % 10));
	seq_puts(s, buf);
	sdhci_div64(
		((total_wr_bytes << 3) * 1000),
		total_wr_usecs, &overall_avg_wr_perf2);
	sdhci_div64(
		(total_wr_bytes * 1000),
		(total_rd_bytes + total_wr_bytes), &wr_percent);
	snprintf(buf, 250,
		"Write Total_bytes=%lldB, time=%lldusecs, overall kbps=%d, Wr percent=%d.%d\n",
		total_wr_bytes, total_wr_usecs,
		overall_avg_wr_perf2,
		(wr_percent / 10), (wr_percent % 10));
	seq_puts(s, buf);

	return 0;
}

static int sdhci_perf_stats_dump(struct inode *inode, struct file *file)
{
	return single_open(file, show_sdhci_perf_stats, inode->i_private);
}

static const struct file_operations flush_sdhci_perf_stats_fops = {
	.open		= sdhci_perf_stats_dump,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int restart_sdhci_perf_stats(struct seq_file *s, void *data)
{
	struct sdhci_host *host = s->private;

	free_stats_nodes(host);
	return 0;
}

static int sdhci_perf_stats_restart(struct inode *inode, struct file *file)
{
	return single_open(file, restart_sdhci_perf_stats, inode->i_private);
}

static const struct file_operations reset_sdhci_perf_stats_fops = {
	.open		= sdhci_perf_stats_restart,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void sdhci_debugfs_init(struct sdhci_host *host)
{
	struct dentry *root = host->debugfs_root;

	/*
	 * debugfs nodes earlier were created from sdhci-tegra,
	 * In this change root debugfs node is created first-come-first-serve
	 */
	if (!root) {
		root = debugfs_create_dir(dev_name(mmc_dev(host->mmc)), NULL);
		if (IS_ERR_OR_NULL(root))
			goto err_root;
		host->debugfs_root = root;
	}

	if (!debugfs_create_u32("enable_sdhci_perf_stats", S_IRUGO | S_IWUSR,
		root, (u32 *)&host->enable_sdhci_perf_stats))
		goto err_root;

	if (!debugfs_create_file("reset_sdhci_perf_stats", S_IRUGO,
		root, host, &reset_sdhci_perf_stats_fops))
		goto err_root;

	if (!debugfs_create_file("sdhci_perf_stats", S_IRUGO,
		root, host, &flush_sdhci_perf_stats_fops))
		goto err_root;

	if (!debugfs_create_u32("sdhci_perf_no_data_transfer_count", S_IRUGO,
		root, (u32 *)&host->no_data_transfer_count))
		goto err_root;

	if (!debugfs_create_u32("max_pio_size", S_IRUGO | S_IWUSR,
		root, (u32 *)&host->max_pio_size))
		goto err_root;

	if (!debugfs_create_u32("max_pio_blocks", S_IRUGO | S_IWUSR,
		root, (u32 *)&host->max_pio_blocks))
		goto err_root;

	return;

err_root:
	debugfs_remove_recursive(root);
	host->debugfs_root = NULL;

	return;
}
#endif

/* runtime pm is not enabled before add host */
int sdhci_add_host(struct sdhci_host *host)
{
	struct mmc_host *mmc;
	u32 caps[2] = {0, 0};
	u32 max_current_caps;
	unsigned int ocr_avail;
	int ret;

	WARN_ON(host == NULL);
	if (host == NULL)
		return -EINVAL;

	mmc = host->mmc;

	if (debug_quirks)
		host->quirks = debug_quirks;
	if (debug_quirks2)
		host->quirks2 = debug_quirks2;

	sdhci_reset(host, SDHCI_RESET_ALL);

	host->version = sdhci_readw(host, SDHCI_HOST_VERSION);
	host->version = (host->version & SDHCI_SPEC_VER_MASK)
				>> SDHCI_SPEC_VER_SHIFT;
	if (host->version > SDHCI_SPEC_410) {
		pr_err("%s: Unknown controller version (%d). "
			"You may experience problems.\n", mmc_hostname(mmc),
			host->version);
	}

	host->mrq_cmd = NULL;
	host->mrq_dat = NULL;
	caps[0] = (host->quirks & SDHCI_QUIRK_MISSING_CAPS) ? host->caps :
		sdhci_readl(host, SDHCI_CAPABILITIES);

	if (host->version >= SDHCI_SPEC_300)
		caps[1] = (host->quirks & SDHCI_QUIRK_MISSING_CAPS) ?
			host->caps1 :
			sdhci_readl(host, SDHCI_CAPABILITIES_1);

	if (host->quirks & SDHCI_QUIRK_FORCE_DMA)
		host->flags |= SDHCI_USE_SDMA;
	else if (!(caps[0] & SDHCI_CAN_DO_SDMA))
		DBG("Controller doesn't have SDMA capability\n");
	else
		host->flags |= SDHCI_USE_SDMA;

	if ((host->quirks & SDHCI_QUIRK_BROKEN_DMA) &&
		(host->flags & SDHCI_USE_SDMA)) {
		DBG("Disabling DMA as it is marked broken\n");
		host->flags &= ~SDHCI_USE_SDMA;
	}

	if ((host->version >= SDHCI_SPEC_200) &&
		(caps[0] & SDHCI_CAN_DO_ADMA2))
		host->flags |= SDHCI_USE_ADMA;

	if ((host->quirks & SDHCI_QUIRK_BROKEN_ADMA) &&
		(host->flags & SDHCI_USE_ADMA)) {
		DBG("Disabling ADMA as it is marked broken\n");
		host->flags &= ~SDHCI_USE_ADMA;
	}

	if (host->flags & (SDHCI_USE_SDMA | SDHCI_USE_ADMA)) {
		if (host->ops->enable_dma) {
			if (host->ops->enable_dma(host)) {
				pr_warning("%s: No suitable DMA "
					"available. Falling back to PIO.\n",
					mmc_hostname(mmc));
				host->flags &=
					~(SDHCI_USE_SDMA | SDHCI_USE_ADMA);
			}
		}
	}

	if (host->flags & SDHCI_USE_ADMA) {
		/*
		 * We need to allocate descriptors for all sg entries
		 * (128) and potentially one alignment transfer for
		 * each of those entries. Simply allocating 128 bits
		 * for each entry
		 */
		if (mmc_dev(host->mmc)->dma_mask &&
				mmc_dev(host->mmc)->coherent_dma_mask) {
			host->adma_desc = dma_alloc_coherent(
					mmc_dev(host->mmc), (128 * 2 + 1) * 8,
					&host->adma_addr, GFP_KERNEL);
			if (!host->adma_desc)
				goto err_dma_alloc;

			host->align_buffer = dma_alloc_coherent(
					mmc_dev(host->mmc), 128 * 8,
					&host->align_addr, GFP_KERNEL);
			if (!host->align_buffer) {
				dma_free_coherent(mmc_dev(host->mmc),
						(128 * 2 + 1) * 8,
						host->adma_desc,
						host->adma_addr);
				host->adma_desc = NULL;
				goto err_dma_alloc;
			}

			host->use_dma_alloc = true;

			BUG_ON(host->adma_addr & 0x3);
			BUG_ON(host->align_addr & 0x3);
			goto out_dma_alloc;
		}
err_dma_alloc:

		host->adma_desc = kmalloc((128 * 2 + 1) * 8, GFP_KERNEL);
		host->align_buffer = kmalloc(128 * 8, GFP_KERNEL);
		if (!host->adma_desc || !host->align_buffer) {
			kfree(host->adma_desc);
			kfree(host->align_buffer);
			pr_warning("%s: Unable to allocate ADMA "
				"buffers. Falling back to standard DMA.\n",
				mmc_hostname(mmc));
			host->flags &= ~SDHCI_USE_ADMA;
		}
	}
out_dma_alloc:

	/*
	 * If we use DMA, then it's up to the caller to set the DMA
	 * mask, but PIO does not need the hw shim so we set a new
	 * mask here in that case.
	 */
	if (!(host->flags & (SDHCI_USE_SDMA | SDHCI_USE_ADMA))) {
		host->dma_mask = DMA_BIT_MASK(64);
		mmc_dev(host->mmc)->dma_mask = &host->dma_mask;
	}

	if (host->version >= SDHCI_SPEC_300)
		host->max_clk = (caps[0] & SDHCI_CLOCK_V3_BASE_MASK)
			>> SDHCI_CLOCK_BASE_SHIFT;
	else
		host->max_clk = (caps[0] & SDHCI_CLOCK_BASE_MASK)
			>> SDHCI_CLOCK_BASE_SHIFT;

	host->max_clk *= 1000000;

	if (mmc->caps2 & MMC_CAP2_HS533)
		host->max_clk = MMC_HS533_MAX_DTR;

	if (host->max_clk == 0 || host->quirks &
			SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN) {
		if (!host->ops->get_max_clock) {
			pr_err("%s: Hardware doesn't specify base clock "
			       "frequency.\n", mmc_hostname(mmc));
			return -ENODEV;
		}
		host->max_clk = host->ops->get_max_clock(host);
	}

	/*
	 * In case of Host Controller v3.00, find out whether clock
	 * multiplier is supported.
	 */
	host->clk_mul = (caps[1] & SDHCI_CLOCK_MUL_MASK) >>
			SDHCI_CLOCK_MUL_SHIFT;

	/*
	 * In case the value in Clock Multiplier is 0, then programmable
	 * clock mode is not supported, otherwise the actual clock
	 * multiplier is one more than the value of Clock Multiplier
	 * in the Capabilities Register.
	 */
	if (host->clk_mul)
		host->clk_mul += 1;

	/*
	 * Set host parameters.
	 */
	mmc->ops = &sdhci_ops;
	mmc->f_max = host->max_clk;
	if (host->ops->get_min_clock)
		mmc->f_min = host->ops->get_min_clock(host);
	else if (host->version >= SDHCI_SPEC_300) {
		if (host->clk_mul) {
			mmc->f_min = (host->max_clk * host->clk_mul) / 1024;
			mmc->f_max = host->max_clk * host->clk_mul;
		} else
			mmc->f_min = host->max_clk / SDHCI_MAX_DIV_SPEC_300;
	} else
		mmc->f_min = host->max_clk / SDHCI_MAX_DIV_SPEC_200;
	if (host->is_sdio)
		printk_once("%s f_min=%d f_max=%d\n", mmc_hostname(mmc),
			mmc->f_min, mmc->f_max);

	host->timeout_clk =
		(caps[0] & SDHCI_TIMEOUT_CLK_MASK) >> SDHCI_TIMEOUT_CLK_SHIFT;
	if (host->timeout_clk == 0) {
		if (host->ops->get_timeout_clock) {
			host->timeout_clk = host->ops->get_timeout_clock(host);
		} else if (!(host->quirks &
				SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK)) {
			pr_err("%s: Hardware doesn't specify timeout clock "
			       "frequency.\n", mmc_hostname(mmc));
			return -ENODEV;
		}
	}
	if (caps[0] & SDHCI_TIMEOUT_CLK_UNIT)
		host->timeout_clk *= 1000;

	if (host->quirks & SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK)
		host->timeout_clk = mmc->f_max / 1000;

	if (!(host->quirks2 & SDHCI_QUIRK2_NO_CALC_MAX_DISCARD_TO))
		mmc->max_discard_to = (1 << 27) / host->timeout_clk;

	if (host->quirks & SDHCI_QUIRK_MULTIBLOCK_READ_ACMD12)
		host->flags |= SDHCI_AUTO_CMD12;

	/* Auto-CMD23 stuff only works in ADMA or PIO. */
	if ((host->version >= SDHCI_SPEC_300) &&
	    ((host->flags & SDHCI_USE_ADMA) ||
	     !(host->flags & SDHCI_USE_SDMA))) {
		host->flags |= SDHCI_AUTO_CMD23;
		DBG("%s: Auto-CMD23 available\n", mmc_hostname(mmc));
	} else {
		DBG("%s: Auto-CMD23 unavailable\n", mmc_hostname(mmc));
	}

	/*
	 * A controller may support 8-bit width, but the board itself
	 * might not have the pins brought out.  Boards that support
	 * 8-bit width must set "mmc->caps |= MMC_CAP_8_BIT_DATA;" in
	 * their platform code before calling sdhci_add_host(), and we
	 * won't assume 8-bit width for hosts without that CAP.
	 */
	if (!(host->quirks & SDHCI_QUIRK_FORCE_1_BIT_DATA))
		mmc->caps |= MMC_CAP_4_BIT_DATA;

	if (host->quirks2 & SDHCI_QUIRK2_HOST_NO_CMD23)
		mmc->caps &= ~MMC_CAP_CMD23;

	if (caps[0] & SDHCI_CAN_DO_HISPD)
		mmc->caps |= MMC_CAP_SD_HIGHSPEED | MMC_CAP_MMC_HIGHSPEED;

	if ((host->quirks & SDHCI_QUIRK_BROKEN_CARD_DETECTION) &&
	    !(host->mmc->caps & MMC_CAP_NONREMOVABLE) && !(host->ops->get_cd))
		mmc->caps |= MMC_CAP_NEEDS_POLL;

	/* If vqmmc regulator and no 1.8V signalling, then there's no UHS */
	host->vqmmc = regulator_get(mmc_dev(mmc), "vqmmc");
	if (IS_ERR_OR_NULL(host->vqmmc)) {
		if (PTR_ERR(host->vqmmc) < 0) {
			pr_info("%s: no vqmmc regulator found\n",
				mmc_hostname(mmc));
			host->vqmmc = NULL;
		}
	} else {
		ret = regulator_enable(host->vqmmc);
		if (!regulator_is_supported_voltage(host->vqmmc, 1700000,
			1950000))
			caps[1] &= ~(SDHCI_SUPPORT_SDR104 |
					SDHCI_SUPPORT_SDR50 |
					SDHCI_SUPPORT_DDR50);
		if (ret) {
			pr_warn("%s: Failed to enable vqmmc regulator: %d\n",
				mmc_hostname(mmc), ret);
			host->vqmmc = NULL;
		}
	}

	if (host->quirks2 & SDHCI_QUIRK2_NO_1_8_V)
		caps[1] &= ~(SDHCI_SUPPORT_SDR104 | SDHCI_SUPPORT_SDR50 |
		       SDHCI_SUPPORT_DDR50);

	/* Any UHS-I mode in caps implies SDR12 and SDR25 support. */
	if (caps[1] & (SDHCI_SUPPORT_SDR104 | SDHCI_SUPPORT_SDR50 |
		       SDHCI_SUPPORT_DDR50))
		mmc->caps |= MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25;

	/* SDR104 supports also implies SDR50 support */
	if (caps[1] & SDHCI_SUPPORT_SDR104)
		mmc->caps |= MMC_CAP_UHS_SDR104 | MMC_CAP_UHS_SDR50;
	else if (caps[1] & SDHCI_SUPPORT_SDR50)
		mmc->caps |= MMC_CAP_UHS_SDR50;

	if (caps[1] & SDHCI_SUPPORT_DDR50)
		mmc->caps |= MMC_CAP_UHS_DDR50;

	/* Does the host need tuning for SDR50? */
	if (caps[1] & SDHCI_USE_SDR50_TUNING)
		host->flags |= SDHCI_SDR50_NEEDS_TUNING;

	/* Does the host need tuning for HS200? */
	if (mmc->caps2 & MMC_CAP2_HS200)
		host->flags |= SDHCI_HS200_NEEDS_TUNING;

	/* Driver Type(s) (A, C, D) supported by the host */
	if (caps[1] & SDHCI_DRIVER_TYPE_A)
		mmc->caps |= MMC_CAP_DRIVER_TYPE_A;
	if (caps[1] & SDHCI_DRIVER_TYPE_C)
		mmc->caps |= MMC_CAP_DRIVER_TYPE_C;
	if (caps[1] & SDHCI_DRIVER_TYPE_D)
		mmc->caps |= MMC_CAP_DRIVER_TYPE_D;

	/* Initial value for re-tuning timer count */
	host->tuning_count = (caps[1] & SDHCI_RETUNING_TIMER_COUNT_MASK) >>
			      SDHCI_RETUNING_TIMER_COUNT_SHIFT;
	/*
	 * If the re-tuning timer count value is 0xF, the timer count
	 * information should be obtained in a non-standard way.
	 */
	if (host->tuning_count == 0xF) {
		if (host->ops->get_tuning_counter) {
			host->tuning_count =
				host->ops->get_tuning_counter(host);
		} else {
			host->tuning_count = 0;
		}
	}

	/*
	 * In case Re-tuning Timer is not disabled, the actual value of
	 * re-tuning timer will be 2 ^ (n - 1).
	 */
	if (host->tuning_count)
		host->tuning_count = 1 << (host->tuning_count - 1);

	/* Re-tuning mode supported by the Host Controller */
	host->tuning_mode = (caps[1] & SDHCI_RETUNING_MODE_MASK) >>
			     SDHCI_RETUNING_MODE_SHIFT;

	ocr_avail = 0;

	host->vmmc = regulator_get(mmc_dev(mmc), "vmmc");
	if (IS_ERR_OR_NULL(host->vmmc)) {
		if (PTR_ERR(host->vmmc) < 0) {
			pr_info("%s: no vmmc regulator found\n",
				mmc_hostname(mmc));
			host->vmmc = NULL;
		}
	}

#ifdef CONFIG_REGULATOR
	/*
	 * Voltage range check makes sense only if regulator reports
	 * any voltage value.
	 */
	if (host->vmmc && regulator_get_voltage(host->vmmc) > 0) {
		ret = regulator_is_supported_voltage(host->vmmc, 2700000,
			3600000);
		if ((ret <= 0) || (!(caps[0] & SDHCI_CAN_VDD_330)))
			caps[0] &= ~SDHCI_CAN_VDD_330;
		if ((ret <= 0) || (!(caps[0] & SDHCI_CAN_VDD_300)))
			caps[0] &= ~SDHCI_CAN_VDD_300;
		ret = regulator_is_supported_voltage(host->vmmc, 1700000,
			1950000);
		if ((ret <= 0) || (!(caps[0] & SDHCI_CAN_VDD_180)))
			caps[0] &= ~SDHCI_CAN_VDD_180;
	}
#endif /* CONFIG_REGULATOR */

	/*
	 * According to SD Host Controller spec v3.00, if the Host System
	 * can afford more than 150mA, Host Driver should set XPC to 1. Also
	 * the value is meaningful only if Voltage Support in the Capabilities
	 * register is set. The actual current value is 4 times the register
	 * value.
	 */
	max_current_caps = sdhci_readl(host, SDHCI_MAX_CURRENT);
	if (!max_current_caps && host->vmmc) {
		u32 curr = regulator_get_current_limit(host->vmmc);
		if (curr > 0) {

			/* convert to SDHCI_MAX_CURRENT format */
			curr = curr/1000;  /* convert to mA */
			curr = curr/SDHCI_MAX_CURRENT_MULTIPLIER;

			curr = min_t(u32, curr, SDHCI_MAX_CURRENT_LIMIT);
			max_current_caps =
				(curr << SDHCI_MAX_CURRENT_330_SHIFT) |
				(curr << SDHCI_MAX_CURRENT_300_SHIFT) |
				(curr << SDHCI_MAX_CURRENT_180_SHIFT);
		}
	}

	if (caps[0] & SDHCI_CAN_VDD_330) {
		ocr_avail |= MMC_VDD_32_33 | MMC_VDD_33_34;

		mmc->max_current_330 = ((max_current_caps &
				   SDHCI_MAX_CURRENT_330_MASK) >>
				   SDHCI_MAX_CURRENT_330_SHIFT) *
				   SDHCI_MAX_CURRENT_MULTIPLIER;
	}
	if (caps[0] & SDHCI_CAN_VDD_300) {
		ocr_avail |= MMC_VDD_29_30 | MMC_VDD_30_31;

		mmc->max_current_300 = ((max_current_caps &
				   SDHCI_MAX_CURRENT_300_MASK) >>
				   SDHCI_MAX_CURRENT_300_SHIFT) *
				   SDHCI_MAX_CURRENT_MULTIPLIER;
	}
	if (caps[0] & SDHCI_CAN_VDD_180) {
		ocr_avail |= MMC_VDD_165_195;

		mmc->max_current_180 = ((max_current_caps &
				   SDHCI_MAX_CURRENT_180_MASK) >>
				   SDHCI_MAX_CURRENT_180_SHIFT) *
				   SDHCI_MAX_CURRENT_MULTIPLIER;
	}

	mmc->ocr_avail = ocr_avail;
	mmc->ocr_avail_sdio = ocr_avail;
	if (host->ocr_avail_sdio)
		mmc->ocr_avail_sdio &= host->ocr_avail_sdio;
	mmc->ocr_avail_sd = ocr_avail;
	if (host->ocr_avail_sd)
		mmc->ocr_avail_sd &= host->ocr_avail_sd;
	else /* normal SD controllers don't support 1.8V */
		mmc->ocr_avail_sd &= ~MMC_VDD_165_195;
	mmc->ocr_avail_mmc = ocr_avail;
	if (host->ocr_avail_mmc)
		mmc->ocr_avail_mmc &= host->ocr_avail_mmc;

	if (mmc->ocr_avail == 0) {
		pr_err("%s: Hardware doesn't report any "
			"support voltages.\n", mmc_hostname(mmc));
		return -ENODEV;
	}

	spin_lock_init(&host->lock);

	/*
	 * Maximum number of segments. Depends on if the hardware
	 * can do scatter/gather or not.
	 */
	if (host->flags & SDHCI_USE_ADMA)
		mmc->max_segs = 128;
	else if (host->flags & SDHCI_USE_SDMA)
		mmc->max_segs = 1;
	else /* PIO */
		mmc->max_segs = 128;

	/*
	 * Maximum number of sectors in one transfer. Limited by DMA boundary
	 * size (512KiB).
	 */
	mmc->max_req_size = 524288;

	/*
	 * Maximum segment size. Could be one segment with the maximum number
	 * of bytes. When doing hardware scatter/gather, each entry cannot
	 * be larger than 64 KiB though.
	 */
	if (host->flags & SDHCI_USE_ADMA) {
		if (host->quirks & SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC)
			mmc->max_seg_size = 65535;
		else
			mmc->max_seg_size = 65536;
	} else {
		mmc->max_seg_size = mmc->max_req_size;
	}

	/*
	 * Maximum block size. This varies from controller to controller and
	 * is specified in the capabilities register.
	 */
	if (host->quirks & SDHCI_QUIRK_FORCE_BLK_SZ_2048) {
		mmc->max_blk_size = 2;
	} else {
		mmc->max_blk_size = (caps[0] & SDHCI_MAX_BLOCK_MASK) >>
				SDHCI_MAX_BLOCK_SHIFT;
		if (mmc->max_blk_size >= 3) {
			pr_info("%s: Invalid maximum block size, "
				"assuming 512 bytes\n", mmc_hostname(mmc));
			mmc->max_blk_size = 0;
		}
	}

	mmc->max_blk_size = 512 << mmc->max_blk_size;

	/*
	 * Maximum block count.
	 */
	if (host->quirks & SDHCI_QUIRK_NO_MULTIBLOCK)
		mmc->max_blk_count = 1;
	else
		mmc->max_blk_count = (host->version > SDHCI_SPEC_400) ?
				((1ULL << BLOCK_COUNT_32BIT) - 1) :
				((1 << BLOCK_COUNT_16BIT) - 1);

#ifdef CONFIG_CMD_DUMP
	mmc->dbg_host_cnt = 0;
#endif

	/*
	 * Init tasklets.
	 */
	tasklet_init(&host->card_tasklet,
		sdhci_tasklet_card, (unsigned long)host);
	tasklet_init(&host->finish_tasklet,
		sdhci_tasklet_finish, (unsigned long)host);
	tasklet_init(&host->finish_cmd_tasklet,
		sdhci_tasklet_cmd_finish, (unsigned long)host);
	tasklet_init(&host->finish_dat_tasklet,
		sdhci_tasklet_dat_finish, (unsigned long)host);

	setup_timer(&host->timer, sdhci_timeout_timer, (unsigned long)host);

	if (host->version >= SDHCI_SPEC_300) {
		init_waitqueue_head(&host->buf_ready_int);

		/* Initialize re-tuning timer */
		init_timer(&host->tuning_timer);
		host->tuning_timer.data = (unsigned long)host;
		host->tuning_timer.function = sdhci_tuning_timer;
	}

	ret = request_irq(host->irq, sdhci_irq, IRQF_SHARED,
		mmc_hostname(mmc), host);
	if (ret) {
		pr_err("%s: Failed to request IRQ %d: %d\n",
		       mmc_hostname(mmc), host->irq, ret);
		goto untasklet;
	}

	sdhci_init(host, 0);

	host->sysedpc = sysedp_create_consumer(dev_name(mmc_dev(mmc)),
					       dev_name(mmc_dev(mmc)));

#ifdef CONFIG_MMC_DEBUG
	sdhci_dumpregs(host);
#endif

#ifdef SDHCI_USE_LEDS_CLASS
	snprintf(host->led_name, sizeof(host->led_name),
		"%s::", mmc_hostname(mmc));
	host->led.name = host->led_name;
	host->led.brightness = LED_OFF;
	host->led.default_trigger = mmc_hostname(mmc);
	host->led.brightness_set = sdhci_led_control;

	ret = led_classdev_register(mmc_dev(mmc), &host->led);
	if (ret) {
		pr_err("%s: Failed to register LED device: %d\n",
		       mmc_hostname(mmc), ret);
		goto reset;
	}
#endif

	mmiowb();

	mmc_add_host(mmc);

	pr_info("%s: SDHCI controller on %s [%s] using %s\n",
		mmc_hostname(mmc), host->hw_name, dev_name(mmc_dev(mmc)),
		(host->flags & SDHCI_USE_ADMA) ? "ADMA" :
		(host->flags & SDHCI_USE_SDMA) ? "DMA" : "PIO");

	sdhci_enable_card_detection(host);

	pm_runtime_enable(mmc_dev(mmc));
	pm_runtime_use_autosuspend(mmc_dev(mmc));
	if (host->quirks2 & SDHCI_QUIRK2_MMC_RTPM) {
		/*
		 * Below Autosuspend delay can be increased/decreased based on
		 * power and perf data
		 */
		pm_runtime_set_autosuspend_delay(mmc_dev(mmc),
			MMC_RTPM_MSEC_TMOUT);
	}
	host->runtime_pm_init_done = true;

#ifdef CONFIG_DEBUG_FS
	/* Add debugfs nodes */
	sdhci_debugfs_init(host);
#endif

	return 0;

#ifdef SDHCI_USE_LEDS_CLASS
reset:
	sdhci_reset(host, SDHCI_RESET_ALL);
	sdhci_mask_irqs(host, SDHCI_INT_ALL_MASK);
	free_irq(host->irq, host);
#endif
untasklet:
	tasklet_kill(&host->card_tasklet);
	tasklet_kill(&host->finish_tasklet);
	tasklet_kill(&host->finish_cmd_tasklet);
	tasklet_kill(&host->finish_dat_tasklet);

	return ret;
}

EXPORT_SYMBOL_GPL(sdhci_add_host);

void sdhci_runtime_forbid(struct sdhci_host *host)
{
	pm_runtime_forbid(mmc_dev(host->mmc));
}
EXPORT_SYMBOL_GPL(sdhci_runtime_forbid);

void sdhci_remove_host(struct sdhci_host *host, int dead)
{
	unsigned long flags;

	sdhci_runtime_pm_get(host);
	if (dead) {
		spin_lock_irqsave(&host->lock, flags);

		host->flags |= SDHCI_DEVICE_DEAD;

		if (host->mrq_cmd || host->mrq_dat) {
			pr_err("%s: Controller removed during "
				" transfer!\n", mmc_hostname(host->mmc));

			if (host->mrq_cmd) {
				host->mrq_cmd->cmd->error = -ENOMEDIUM;
				if (MMC_CHECK_CMDQ_MODE(host))
					tasklet_schedule(&host->finish_cmd_tasklet);
				else
					tasklet_schedule(&host->finish_tasklet);
			}
			if (host->mrq_dat) {
				host->mrq_dat->cmd->error = -ENOMEDIUM;
				if (MMC_CHECK_CMDQ_MODE(host))
					tasklet_schedule(&host->finish_dat_tasklet);
				else
					tasklet_schedule(&host->finish_tasklet);
			}
		}

		spin_unlock_irqrestore(&host->lock, flags);
	}

	sdhci_disable_card_detection(host);

	mmc_remove_host(host->mmc);

#ifdef SDHCI_USE_LEDS_CLASS
	led_classdev_unregister(&host->led);
#endif

	if (!dead)
		sdhci_reset(host, SDHCI_RESET_ALL);

	sdhci_mask_irqs(host, SDHCI_INT_ALL_MASK);
	free_irq(host->irq, host);

	del_timer_sync(&host->timer);

	tasklet_kill(&host->card_tasklet);
	tasklet_kill(&host->finish_tasklet);
	tasklet_kill(&host->finish_cmd_tasklet);
	tasklet_kill(&host->finish_dat_tasklet);

	if (host->vmmc) {
		regulator_disable(host->vmmc);
		regulator_put(host->vmmc);
	}

	if (host->vqmmc) {
		regulator_disable(host->vqmmc);
		regulator_put(host->vqmmc);
	}

	if (host->use_dma_alloc) {
		dma_free_coherent(mmc_dev(host->mmc), (128 * 2 + 1) * 8,
				host->adma_desc, host->adma_addr);
		dma_free_coherent(mmc_dev(host->mmc), 128 * 8,
				host->align_buffer, host->align_addr);
	} else {
		kfree(host->adma_desc);
		kfree(host->align_buffer);
	}

	host->adma_desc = NULL;
	host->align_buffer = NULL;

	sdhci_runtime_pm_put(host);
	sysedp_free_consumer(host->sysedpc);
	host->sysedpc = NULL;
}

EXPORT_SYMBOL_GPL(sdhci_remove_host);

void sdhci_free_host(struct sdhci_host *host)
{
	mmc_free_host(host->mmc);
}

EXPORT_SYMBOL_GPL(sdhci_free_host);

/*****************************************************************************\
 *                                                                           *
 * Driver init/exit                                                          *
 *                                                                           *
\*****************************************************************************/

static int __init sdhci_drv_init(void)
{
	pr_info(DRIVER_NAME
		": Secure Digital Host Controller Interface driver\n");
	pr_info(DRIVER_NAME ": Copyright(c) Pierre Ossman\n");

	return 0;
}

static void __exit sdhci_drv_exit(void)
{
}

module_init(sdhci_drv_init);
module_exit(sdhci_drv_exit);

module_param(debug_quirks, uint, 0444);
module_param(debug_quirks2, uint, 0444);

MODULE_AUTHOR("Pierre Ossman <pierre@ossman.eu>");
MODULE_DESCRIPTION("Secure Digital Host Controller Interface core driver");
MODULE_LICENSE("GPL");

MODULE_PARM_DESC(debug_quirks, "Force certain quirks.");
MODULE_PARM_DESC(debug_quirks2, "Force certain other quirks.");
