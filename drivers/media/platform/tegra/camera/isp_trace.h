/*
 * ISP Trace — persistent ring buffer for ISP debug tracing
 *
 * 16MB reserved physical memory region, survives reboot.
 * Read via /proc/isp_trace, reset via /proc/isp_trace_reset.
 *
 * Copyright (c) 2026, Artem Bambalov. All rights reserved.
 */

#ifndef __ISP_TRACE_H__
#define __ISP_TRACE_H__

#include <linux/types.h>

#define ISP_TRACE_BUF_SIZE	(16 * 1024 * 1024)	/* 16MB */
#define ISP_TRACE_MAGIC		0x49535054		/* 'ISPT' */

/* Ring buffer header — lives at start of reserved region */
struct isp_trace_header {
	u32 magic;
	u32 write_pos;		/* byte offset after header */
	u32 wrap_count;
	u32 data_size;		/* total data area size (buf_size - sizeof(header)) */
	u32 entry_count;
	u32 reserved[3];
};

#define ISP_TRACE_DATA_SIZE	(ISP_TRACE_BUF_SIZE - sizeof(struct isp_trace_header))

/* Init/cleanup — called from board init and isp probe */
void isp_trace_set_phys(phys_addr_t phys, unsigned long size);
int isp_trace_init(void);
void isp_trace_cleanup(void);

/* Main logging function — safe from any context (uses spinlock) */
void isp_trace_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Hex dump helper for gather buffers */
void isp_trace_hex(const char *tag, const u32 *data, int words);

#endif /* __ISP_TRACE_H__ */
