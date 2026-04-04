/*
 * ISP Trace — persistent ring buffer for ISP debug tracing
 *
 * Writes to a 16MB reserved physical memory region that survives reboot.
 * Provides /proc/isp_trace for reading and /proc/isp_trace_reset for clearing.
 *
 * Copyright (c) 2026, Artem Bambalov. All rights reserved.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/string.h>

#include "isp_trace.h"

static phys_addr_t trace_phys;
static unsigned long trace_size;
static void *trace_base;	/* cached mapping — direct pointer access */
static struct isp_trace_header *trace_hdr;
static char *trace_data;
static int trace_is_phys;	/* 1 = ioremap_cached, 0 = vmalloc */
static DEFINE_SPINLOCK(trace_lock);

static struct proc_dir_entry *proc_trace;
static struct proc_dir_entry *proc_reset;

/* Set physical address — called from board init */
void isp_trace_set_phys(phys_addr_t phys, unsigned long size)
{
	trace_phys = phys;
	trace_size = size;
	pr_info("isp_trace: phys=0x%pa size=%lu\n", &phys, size);
}
EXPORT_SYMBOL(isp_trace_set_phys);

/* Write a string into the ring buffer — caller must hold trace_lock */
static void trace_write_raw(const char *buf, int len)
{
	u32 pos, data_size, space;

	if (!trace_data || !trace_hdr)
		return;

	data_size = trace_hdr->data_size;
	pos = trace_hdr->write_pos;

	/* Fast path: no wrap needed */
	space = data_size - pos;
	if (len <= space) {
		memcpy(trace_data + pos, buf, len);
		pos += len;
		if (pos >= data_size) {
			pos = 0;
			trace_hdr->wrap_count++;
		}
	} else {
		/* Wrap around */
		memcpy(trace_data + pos, buf, space);
		memcpy(trace_data, buf + space, len - space);
		pos = len - space;
		trace_hdr->wrap_count++;
	}

	trace_hdr->write_pos = pos;
	trace_hdr->entry_count++;
}

/* Main logging function — safe from any context */
void isp_trace_log(const char *fmt, ...)
{
	va_list args;
	char line[512];
	int len;
	unsigned long flags;
	s64 ts;

	if (!trace_data)
		return;

	ts = ktime_to_us(ktime_get());

	va_start(args, fmt);
	len = snprintf(line, sizeof(line), "[%lld] ", ts);
	len += vsnprintf(line + len, sizeof(line) - len, fmt, args);
	va_end(args);

	if (len >= sizeof(line))
		len = sizeof(line) - 1;
	if (len > 0 && line[len - 1] != '\n')
		line[len++] = '\n';

	spin_lock_irqsave(&trace_lock, flags);
	trace_write_raw(line, len);
	spin_unlock_irqrestore(&trace_lock, flags);
}
EXPORT_SYMBOL(isp_trace_log);

/* Hex dump helper — formats outside lock, writes inside */
void isp_trace_hex(const char *tag, const u32 *data, int words)
{
	char line[256];
	int i, j, len, remaining;
	unsigned long flags;
	s64 ts;

	if (!trace_data || !data)
		return;

	ts = ktime_to_us(ktime_get());

	for (i = 0; i < words; i += 8) {
		remaining = words - i;
		if (remaining > 8)
			remaining = 8;

		/* Format outside lock */
		len = snprintf(line, sizeof(line), "[%lld] %s[%03d]:", ts, tag, i);
		for (j = 0; j < remaining; j++)
			len += snprintf(line + len, sizeof(line) - len,
					" %08x", data[i + j]);
		if (len < sizeof(line) - 1)
			line[len++] = '\n';

		/* Write inside lock */
		spin_lock_irqsave(&trace_lock, flags);
		trace_write_raw(line, len);
		spin_unlock_irqrestore(&trace_lock, flags);
	}
}
EXPORT_SYMBOL(isp_trace_hex);

/* /proc/isp_trace — direct read (no seq_file, handles large buffers) */
static ssize_t isp_trace_read(struct file *file, char __user *buf,
			      size_t count, loff_t *ppos)
{
	u32 pos, data_size, wrap_count;
	u32 total, start;
	ssize_t ret;
	loff_t off = *ppos;

	if (!trace_hdr || !trace_data)
		return -ENODEV;

	pos = trace_hdr->write_pos;
	data_size = trace_hdr->data_size;
	wrap_count = trace_hdr->wrap_count;

	total = wrap_count > 0 ? data_size : pos;
	start = wrap_count > 0 ? pos : 0;

	if (off >= total)
		return 0;
	if (off + count > total)
		count = total - off;

	/* Map logical offset to ring buffer position */
	{
		u32 ring_pos = (start + (u32)off) % data_size;
		u32 first_chunk = data_size - ring_pos;

		if (count <= first_chunk) {
			if (copy_to_user(buf, trace_data + ring_pos, count))
				return -EFAULT;
		} else {
			if (copy_to_user(buf, trace_data + ring_pos, first_chunk))
				return -EFAULT;
			if (copy_to_user(buf + first_chunk, trace_data,
					 count - first_chunk))
				return -EFAULT;
		}
	}

	*ppos = off + count;
	return count;
}

static const struct file_operations isp_trace_fops = {
	.read    = isp_trace_read,
};

/* /proc/isp_trace_reset — write "1" to clear */
static ssize_t isp_trace_reset_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	unsigned long flags;

	if (!trace_hdr)
		return -ENODEV;

	spin_lock_irqsave(&trace_lock, flags);
	trace_hdr->magic = ISP_TRACE_MAGIC;
	trace_hdr->write_pos = 0;
	trace_hdr->wrap_count = 0;
	trace_hdr->data_size = ISP_TRACE_DATA_SIZE;
	trace_hdr->entry_count = 0;
	memset(trace_data, 0, ISP_TRACE_DATA_SIZE);
	spin_unlock_irqrestore(&trace_lock, flags);

	pr_info("isp_trace: buffer reset\n");
	return count;
}

static const struct file_operations isp_trace_reset_fops = {
	.write = isp_trace_reset_write,
};

/* Init — called after board reserves memory */
int isp_trace_init(void)
{
	if (!trace_phys || !trace_size) {
		pr_warn("isp_trace: no reserved memory, using vmalloc fallback\n");
		trace_base = vzalloc(ISP_TRACE_BUF_SIZE);
		if (!trace_base)
			return -ENOMEM;
		trace_is_phys = 0;
		trace_hdr = (struct isp_trace_header *)trace_base;
		trace_data = (char *)trace_base + sizeof(struct isp_trace_header);
		goto init_header;
	}

	/* Try cached first, fall back to uncached (like ramoops does) */
	trace_base = ioremap_cached(trace_phys, trace_size);
	if (!trace_base) {
		pr_warn("isp_trace: ioremap_cached failed, trying ioremap\n");
		trace_base = ioremap(trace_phys, trace_size);
	}
	if (!trace_base) {
		pr_err("isp_trace: ioremap failed for 0x%pa, using vmalloc\n",
		       &trace_phys);
		trace_base = vzalloc(ISP_TRACE_BUF_SIZE);
		if (!trace_base)
			return -ENOMEM;
		trace_is_phys = 0;
		trace_hdr = (struct isp_trace_header *)trace_base;
		trace_data = (char *)trace_base + sizeof(struct isp_trace_header);
		goto init_header;
	}
	trace_is_phys = 1;

	trace_hdr = (struct isp_trace_header *)trace_base;
	trace_data = (char *)trace_base + sizeof(struct isp_trace_header);

	/* Check if buffer has valid data from previous boot */
	if (trace_hdr->magic == ISP_TRACE_MAGIC) {
		u32 entries = trace_hdr->entry_count;
		pr_info("isp_trace: found existing data (%u entries) from previous boot\n",
			entries);
		goto create_proc;
	}

init_header:
	trace_hdr->magic = ISP_TRACE_MAGIC;
	trace_hdr->write_pos = 0;
	trace_hdr->wrap_count = 0;
	trace_hdr->data_size = ISP_TRACE_DATA_SIZE;
	trace_hdr->entry_count = 0;

create_proc:
	proc_trace = proc_create("isp_trace", 0444, NULL, &isp_trace_fops);
	proc_reset = proc_create("isp_trace_reset", 0200, NULL,
				 &isp_trace_reset_fops);

	isp_trace_log("=== ISP TRACE STARTED ===");
	pr_info("isp_trace: ready, phys=0x%pa size=%lu cached=%d\n",
		&trace_phys, trace_size, trace_is_phys);
	return 0;
}
EXPORT_SYMBOL(isp_trace_init);

void isp_trace_cleanup(void)
{
	if (proc_trace)
		remove_proc_entry("isp_trace", NULL);
	if (proc_reset)
		remove_proc_entry("isp_trace_reset", NULL);

	if (trace_base) {
		if (trace_is_phys)
			iounmap(trace_base);
		else
			vfree(trace_base);
		trace_base = NULL;
	}
	trace_hdr = NULL;
	trace_data = NULL;
}
EXPORT_SYMBOL(isp_trace_cleanup);

static int __init isp_trace_module_init(void)
{
	return isp_trace_init();
}

subsys_initcall(isp_trace_module_init);
