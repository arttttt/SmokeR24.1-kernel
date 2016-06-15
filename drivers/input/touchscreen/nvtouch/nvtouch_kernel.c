/*
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>

#include <linux/timer.h>
#include <linux/delay.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/export.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/kmod.h>
#include <linux/time.h>
#include <linux/platform_device.h>
#include <linux/backlight.h>
#include <linux/input/mt.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/fs.h>


#include "nvtouch_kernel.h"

#define DEVICE_NAME "nvtouch"

#define NVTOUCH_DEBUG 1

#define GPFIFO_SIZE_SPI 4
#define GPFIFO_SIZE_TRACE 1024

struct nvtouch_sample_gpfifo {
	u32 getp;
	u32 putp;
	u32 size;
	wait_queue_head_t data_waitqueue;
	struct nvtouch_data_frame *frames;
};

struct nvtouch_rect {
	u32 left;
	u32 top;
	u32 right;
	u32 bottom;
};

struct nvtouch_dta_state {
	spinlock_t slock;
	u32 client_refcount;
	u32 authorized_pid;
	struct nvtouch_rect authorized_win_device;
	u32 device_height;
	u32 device_width;
	u32 orientation;
	u8 send_debug_touch;
	struct nvtouch_ioctl_dta_config ioctl_dta_config;
	struct nvtouch_ioctl_dta_admin_config ioctl_dta_admin_config;
	struct nvtouch_ioctl_dta_admin_update ioctl_dta_admin_update;
	struct nvtouch_ioctl_dta_admin_query ioctl_dta_admin_query;

	struct nvtouch_ioctl_debug_touch ioctl_dta_debug_touch_send;
	struct nvtouch_ioctl_debug_touch ioctl_dta_debug_touch_recv;
	struct nvtouch_ioctl_dta ioctl_dta;
} g_state_dta;

struct dentry               *dfs_immediate_data;
struct dentry               *dfs_unpacked_data;
struct debugfs_blob_wrapper immediate_data;
struct debugfs_blob_wrapper captured_data;
struct debugfs_blob_wrapper unpacked_data;

static void nvtouch_kernel_process_locked(void *samples,
		int samples_size, u8 report_mode,
		u64 current_time);

u8 s_frame_immediate_data[NVTOUCH_SENSOR_DATA_RESERVED];


static void nvtouch_init_blobs(struct dentry *dfs_nvtouch_dir)
{
	immediate_data.size = NVTOUCH_SENSOR_DATA_RESERVED;
	immediate_data.data = s_frame_immediate_data;

	unpacked_data.size = NVTOUCH_SENSOR_DATA_RESERVED;
	unpacked_data.data = &g_state_dta.ioctl_dta.data;

#ifdef NVTOUCH_DEBUG
	if (!dfs_immediate_data)
		dfs_immediate_data = debugfs_create_blob(
			"sim_events_data", S_IRUSR,
			dfs_nvtouch_dir, &immediate_data);
	if (!dfs_unpacked_data)
		dfs_unpacked_data = debugfs_create_blob(
			"unpacked_data", S_IRUSR,
			dfs_nvtouch_dir, &unpacked_data);
	if (!dfs_immediate_data || !dfs_unpacked_data)
		nv_printf("__TOUCH: debugfs_create_blob error\n");
#endif
}

#define NVTOUCH_MT_SLOT_COUNT 10

struct trace_header {
	u32 id;
	u32 nvtouch_version;
	u64 start_timestamp;
};

struct trace_item {
	struct nvtouch_events detected_events;
	struct nvtouch_data_frame data_frame;
	struct nvtouch_events vendor_events;
};

struct trace_state {
	u32 header_ptr;
	u32 item_ptr;
	u64 playback_start_time;
	u32 frame_count;
	struct trace_header header;
	struct trace_item item;
};

struct nvtouch_kernel_state {
	struct nvtouch_ioctl_config userspace_config;
	struct input_dev *input_device;
	struct dentry *debug_dfs;
	struct nvtouch_sample_gpfifo *sample_gpfifo;
	struct nvtouch_sample_gpfifo *trace_gpfifo;
	struct mutex sample_input_mutex;
	u8 mutithreaded_sample_access_enabled;
	atomic_t nvtouch_dev_ref_count;
	u64 frame_counter;
	u32 last_event_count;
	u64 time_of_last_touch;
	u32 refresh_rate_hz;
	struct nvtouch_events last_reported;
	u32 enable_debug_log;
	u32 last_events_id;
	u32 trace_mode;
	u8 send_debug_touch;
	u64 last_debug_send_timestamp;
	u32 driver_mode;
	/*
	 * input coordinates for dual mode (both vendor and
	 * nvtouch can report events)
	 */
	u32 dual_mode_vendor_delta_x;
	u32 dual_mode_vendor_delta_y;
	u32 dual_mode_nvtouch_delta_x;
	u32 dual_mode_nvtouch_delta_y;

	u32 dual_mode_enable_nvtouch_events;
	u32 dual_mode_enable_vendor_events;

	u32 pm_active_to_lp_timeout_ms;
	u32 pm_active_to_idle_timeout_ms;

	u32 enable_calibration;
	u32 enable_calibration_debug;

	u32 tune_debug;
	u32 tune_param;

	struct nvtouch_events vendor_events;

	struct backlight_device *backlight_device;
	u32 debug_blink_on_touch;
	u32 debug_blink_on_event;
	u64 touch_irq_timestamp;
	u8 report_mode;
	u8 is_initialized;

	char version_string[64];

	struct nvtouch_ioctl_data ioctl_data_update;
	struct nvtouch_ioctl_config config_buf;

	u32 update_time_acc;
	u32 update_count;
	u64 last_print_timestamp;
	u32 input_slot_bits;
	u32 input_slot_ids[NVTOUCH_MT_SLOT_COUNT];

	struct trace_state replay_trace;

	struct device *nvtouch_dev;


} g_state_kernel;



/* NvTouch reports touch id 1..127
 * lr388k7 supports 10 slots
 * Use get_mt_slot_id() and clear_mt_slot()
 * functions for compatibility */
static int get_mt_slot_id(u32 touch_id)
{
	int i = 0;
	for (i = 0; i < NVTOUCH_MT_SLOT_COUNT; i++) {
		if (g_state_kernel.input_slot_ids[i] == touch_id)
			return i;
	}
	/* if slot doesn't exist, allocate a new one */
	for (i = 0; i < NVTOUCH_MT_SLOT_COUNT; i++) {
		if (g_state_kernel.input_slot_ids[i] == -1) {
			g_state_kernel.input_slot_ids[i] = touch_id;
			return i;
		}
	}
	return -1;
}

static void clear_mt_slot(u32 mt_id)
{
	g_state_kernel.input_slot_ids[mt_id] = -1;
}

void nvtouch_kernel_timestamp_touch_irq()
{
	g_state_kernel.touch_irq_timestamp = nvtouch_get_time_us();
}
EXPORT_SYMBOL(nvtouch_kernel_timestamp_touch_irq);

static void init_debug_blink_on_touch(void)
{
	char *bl_device_name = "pwm-backlight";
	g_state_kernel.backlight_device =
		get_backlight_device_by_name(bl_device_name);
	if (!g_state_kernel.backlight_device) {
		pr_err("NVTOUCH: backlight debug cannot be initialized\n");
	} else {
		EXPOSE_VALUE_U32(g_state_kernel.debug_blink_on_touch);
		EXPOSE_VALUE_U32(g_state_kernel.debug_blink_on_event);
	}
}

struct nvtouch_events *nvtouch_kernel_get_vendor_events_ptr()
{
	return &g_state_kernel.vendor_events;
}
EXPORT_SYMBOL(nvtouch_kernel_get_vendor_events_ptr);

static void get_debug_touch(struct nvtouch_events *events)
{
		events->timestamp = nvtouch_get_time_us();
		events->event_count = 1;
		events->events[0].pressure = 0;
}

/*
 * creates gpfifo of desired size
 */
static struct nvtouch_sample_gpfifo *nvtouch_gpfifo_create(u32 size)
{
	struct nvtouch_sample_gpfifo *gpfifo =
		kmalloc(sizeof(struct nvtouch_sample_gpfifo), GFP_KERNEL);
	if (gpfifo) {
		memset(gpfifo, 0, sizeof(struct nvtouch_sample_gpfifo));
		init_waitqueue_head(&gpfifo->data_waitqueue);
		gpfifo->size = size;
		gpfifo->frames =
			vmalloc(size * sizeof(struct nvtouch_data_frame));

		if (gpfifo->frames) {
			memset(gpfifo->frames, 0,
				size * sizeof(struct nvtouch_data_frame));
		} else {
			kfree(gpfifo);
			gpfifo = NULL;
		}
	}
	return gpfifo;
}

/*
 *
 */
static void nvtouch_gpfifo_free(struct nvtouch_sample_gpfifo *gpfifo)
{
	if (gpfifo) {
		if (gpfifo->frames)
			vfree(gpfifo->frames);
		kfree(gpfifo);
	}
}

/*
 * Returns status of gpfifo
 */
static int nvtouch_gpfifo_empty(struct nvtouch_sample_gpfifo *gpfifo)
{
	wmb();
	return (gpfifo->putp - gpfifo->getp) == 0;
}

/*
 * Returns pointer for writing data to gpfifo
 * NULL is fifo is full
 */
static struct nvtouch_data_frame *nvtouch_gpfifo_put(
		struct nvtouch_sample_gpfifo *gpfifo)
{
	struct nvtouch_data_frame *frame = NULL;
	wmb();
	if ((gpfifo->putp - gpfifo->getp) < gpfifo->size)
		frame = &gpfifo->frames[gpfifo->putp % gpfifo->size];
	return frame;
}

/*
 * Returns pointer for reading data
 * NULL is fifo is empty
 */
static struct nvtouch_data_frame *nvtouch_gpfifo_get(
		struct nvtouch_sample_gpfifo *gpfifo)
{
	struct nvtouch_data_frame *frame = NULL;
	wmb();
	if ((gpfifo->putp - gpfifo->getp) > 0)
		frame = &gpfifo->frames[gpfifo->getp % gpfifo->size];
	return frame;
}

static int frames_skipped;

static int trace_replay_thread(void *param)
{
	(void)param;
	/* delay to buffer enough data for smooth playback */
	msleep(1000);
	g_state_kernel.replay_trace.playback_start_time =
			nvtouch_get_time_us();
	while (1) {
		struct nvtouch_data_frame *data_frame;
		u64 frame_time;
		u64 current_time;
		if (nvtouch_gpfifo_empty(g_state_kernel.trace_gpfifo)) {
			usleep_range(500, 1500);
			continue;
		}

		data_frame = nvtouch_gpfifo_get(g_state_kernel.trace_gpfifo);

		BUG_ON(!data_frame);

		/* end of replay signature */
		if (data_frame->timestamp == -1ll) {
			wmb();
			g_state_kernel.trace_gpfifo->getp++;
			pr_info("Replay thread finished\n");
			return 0;
		}

		frame_time = g_state_kernel.replay_trace.playback_start_time +
			data_frame->timestamp -
			g_state_kernel.replay_trace.header.start_timestamp;

		current_time = nvtouch_get_time_us();

		while (frame_time > current_time) {
			usleep_range(500, 1500);
			if (frame_time - current_time > 10000000ll) {
				pr_err("nvtouch: input timeout %llu us\n",
						frame_time - current_time);
				break;
			}
			current_time = nvtouch_get_time_us();
		}

		if (current_time - frame_time > 3000) {
			pr_warn("replay latency is high %llu us\n",
					nvtouch_get_time_us() - frame_time);
		}

		nvtouch_kernel_process_locked(data_frame->samples,
					NVTOUCH_SENSOR_DATA_RESERVED,
					g_state_kernel.report_mode,
					current_time);
		wmb();
		g_state_kernel.trace_gpfifo->getp++;
	}
	return 0;
}


static ssize_t debugfs_write_trace(struct file *filp,
		   const char *buf,
		   size_t count_total,
		   loff_t *off)
{
	u32 count_processed = 0;
	u32 count_to_read;
	u8 *dest;
	u64 frame_time;
	u64 current_time;
	struct nvtouch_data_frame *data_frame;
	unsigned long count_unread;

	while (count_processed < count_total) {
		int count_remaining = count_total - count_processed;

		if (g_state_kernel.replay_trace.header_ptr
				< sizeof(struct trace_header)) {
			u32 count_to_read = sizeof(struct trace_header) -
				g_state_kernel.replay_trace.header_ptr;

			u8 *dest = (u8 *)&g_state_kernel.replay_trace.header;

			count_to_read = count_to_read < count_remaining ?
					count_to_read : count_remaining;

			count_unread = copy_from_user((void *)
				&dest[g_state_kernel.replay_trace.header_ptr],
				&buf[count_processed],
				count_to_read);

			if (count_unread) {
				WARN_ON("Cannot read trace from userspace");
				return -EFAULT;
			}

			g_state_kernel.replay_trace.header_ptr +=
					count_to_read;
			count_processed += count_to_read;

			/* If full header is retrieved, start the playback */
			if (g_state_kernel.replay_trace.header_ptr ==
					sizeof(struct trace_header)) {

				pr_info("Trace playback started, v %d\n",
					g_state_kernel.replay_trace.header
					.nvtouch_version);

				g_state_kernel.replay_trace
					.playback_start_time =
						nvtouch_get_time_us();
				wmb();
				kthread_run(trace_replay_thread, NULL,
						"nvtouch_replay");
			}

			/* jump to beginning of the data processing
			 * loop to process next chunk of data */
			continue;
		}


		/* Header must be fully initialized */
		BUG_ON(g_state_kernel.replay_trace.header_ptr !=
				sizeof(struct trace_header));

		BUG_ON(g_state_kernel.replay_trace.item_ptr >=
				sizeof(struct trace_item));

		/* get a new frame */
		count_to_read = sizeof(struct trace_item) -
			g_state_kernel.replay_trace.item_ptr;

		dest = (u8 *) &g_state_kernel.replay_trace.item;

		count_to_read = count_to_read < count_remaining ?
				count_to_read : count_remaining;

		count_unread = copy_from_user(
			(void *)&dest[g_state_kernel.replay_trace.item_ptr],
			&buf[count_processed],
			count_to_read);

		if (count_unread) {
			WARN_ON("Cannot read trace from userspace");
			return -EFAULT;
		}

		g_state_kernel.replay_trace.item_ptr += count_to_read;
		count_processed += count_to_read;

		/* If full frame is not retrieved, jump to
		 * process next chunk of data */
		if (g_state_kernel.replay_trace.item_ptr !=
				sizeof(struct trace_item))
			continue;


#if 0
		/* Synchronous replay, simple but doesn't
		 * keep timing well */
		frame_time =
			g_state_kernel.replay_trace.playback_start_time +
			g_state_kernel.replay_trace.item.data_frame.timestamp -
			g_state_kernel.replay_trace.header.start_timestamp;
		current_time = nvtouch_get_time_us();
		while (frame_time > current_time) {
			usleep_range(500, 1500);
			current_time = nvtouch_get_time_us();
		}

		nvtouch_kernel_process_locked(
			g_state_kernel.replay_trace.item.data_frame.samples,
			NVTOUCH_SENSOR_DATA_RESERVED,
			g_state_kernel.report_mode,
			current_time);

		if (current_time - frame_time > 3000) {
			pr_warn("replay latency is high %llu us\n",
					current_time - frame_time);
		}

#else
		/* Asynchronous replay - more complex, but keeps timing more
		 * accurate */
		(void) current_time;
		(void) frame_time;
		while (!nvtouch_gpfifo_put(g_state_kernel.trace_gpfifo))
			usleep_range(3000, 4000);

		data_frame = nvtouch_gpfifo_put(g_state_kernel.trace_gpfifo);

		BUG_ON(!data_frame);

		memcpy(data_frame->samples,
			g_state_kernel.replay_trace.item.data_frame.samples,
			NVTOUCH_SENSOR_DATA_RESERVED);
		data_frame->timestamp =
			g_state_kernel.replay_trace.item.data_frame.timestamp;
		wmb();
		g_state_kernel.trace_gpfifo->putp++;
#endif
		g_state_kernel.replay_trace.frame_count++;
		/* Prepare for new frame */
		g_state_kernel.replay_trace.item_ptr = 0;

	}

	BUG_ON(count_processed != count_total);
	*off += count_processed;

	return count_processed;
}

static int debugfs_replay_open(struct inode *inode,
	struct file *file)
{
	if (!g_state_kernel.mutithreaded_sample_access_enabled) {
		mutex_init(&g_state_kernel.sample_input_mutex);
		wmb();
		g_state_kernel.mutithreaded_sample_access_enabled = true;
	}

	mutex_lock(&g_state_kernel.sample_input_mutex);

	/* Should not be allocated */
	BUG_ON(g_state_kernel.trace_gpfifo);

	g_state_kernel.trace_gpfifo =
			nvtouch_gpfifo_create(GPFIFO_SIZE_TRACE);

	memset(&g_state_kernel.replay_trace, 0, sizeof(struct trace_state));

	pr_info("nvtouch: start touch trace replay...\n");
	return 0;
}

static int debugfs_replay_release(struct inode *inode,
	struct file *file)
{
	struct nvtouch_data_frame *data_frame;

	while (!nvtouch_gpfifo_put(g_state_kernel.trace_gpfifo))
		usleep_range(1000, 2000);

	data_frame = nvtouch_gpfifo_put(g_state_kernel.trace_gpfifo);

	/* Send end of data marker */
	data_frame->timestamp = -1ll;
	wmb();
	g_state_kernel.trace_gpfifo->putp++;

	/* wait for playback to finish */
	while (!nvtouch_gpfifo_empty(g_state_kernel.trace_gpfifo))
		usleep_range(1000, 3000);

	pr_info("nvtouch: stop touch trace replay.\n");

	nvtouch_gpfifo_free(g_state_kernel.trace_gpfifo);
	g_state_kernel.trace_gpfifo = NULL;

	mutex_unlock(&g_state_kernel.sample_input_mutex);
	return 0;
}

static const struct file_operations trace_replay_ops = {
		.open = debugfs_replay_open,
		.release = debugfs_replay_release,
		.write = debugfs_write_trace
};


static ssize_t sysfs_driver_mode_show(struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	return sprintf(buf, "%d\n",
			g_state_kernel.driver_mode);
}

static ssize_t sysfs_driver_mode_set(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	ssize_t ret;

	if (count < 2)
		return -EINVAL;

	ret = (ssize_t) count;

	if (count == 2) {
		switch (buf[0]) {
		case '0':
			g_state_kernel.driver_mode = 0;
			break;
		case '1':
			g_state_kernel.driver_mode = 1;
			break;
		case '2':
			g_state_kernel.driver_mode = 2;
			break;
		case '3':
			g_state_kernel.driver_mode = 3;
			break;
		default:
			return -EINVAL;
		}
	}

	/* send last data frame to userspace to notify about
	 * driver mode changes */
	if (g_state_kernel.is_initialized &&
		nvtouch_gpfifo_empty(g_state_kernel.sample_gpfifo)) {
		wmb();
		g_state_kernel.sample_gpfifo->getp--;
		wake_up(&g_state_kernel.sample_gpfifo->data_waitqueue);
	}
	return ret;
}

static DEVICE_ATTR(driver_mode, 0600, sysfs_driver_mode_show,
		sysfs_driver_mode_set);

/*
 * Timestamp the samples and pass them to further processing
 */
static void nvtouch_kernel_process_locked(void *samples, int samples_size,
		u8 report_mode, u64 data_timestamp)
{
	struct nvtouch_data_frame *data_frame;

	g_state_kernel.report_mode = report_mode;

	/* if no DTA clients, quit */
	if (g_state_kernel.driver_mode == NVTOUCH_DRIVER_CONFIG_MODE_VENDOR_DTA
			&& g_state_dta.client_refcount <= 0)
		return;

	if (g_state_kernel.debug_blink_on_touch) {
		g_state_kernel.backlight_device->props.brightness = 0;
		g_state_kernel.backlight_device->ops->update_status(
			g_state_kernel.backlight_device
		);
		g_state_kernel.debug_blink_on_touch = 0;
	}

	/* TODO: remove immediate data? */
#ifdef NVTOUCH_DEBUG
	if (immediate_data.data)
		memcpy(immediate_data.data, samples,
			samples_size < NVTOUCH_SENSOR_DATA_RESERVED ?
			samples_size : NVTOUCH_SENSOR_DATA_RESERVED);
#endif
	if (samples_size > NVTOUCH_SENSOR_DATA_RESERVED) {
		pr_err("nvtouch: samples size = %d > NVTOUCH_SENSOR_DATA_RESERVED\n",
				samples_size);
		/* Unexpected frame size, don't process this data */
		return;
	}

	data_frame = nvtouch_gpfifo_put(g_state_kernel.sample_gpfifo);
	if (data_frame) {
		data_frame->timestamp = data_timestamp;
		data_frame->irq_timestamp = g_state_kernel.touch_irq_timestamp;
		data_frame->frame_counter = g_state_kernel.frame_counter++;
		memcpy(data_frame->samples, samples, samples_size);
		wmb();
		g_state_kernel.sample_gpfifo->putp++;
		if (g_state_kernel.is_initialized) {
			/* Wake up touch recognition engine to
			process new data frame */
			wake_up(&g_state_kernel.sample_gpfifo->data_waitqueue);
		}
	} else {
		if (!(frames_skipped & 0xff))
			pr_err(
			"nvtouch: userspace_daemon processing: No space in GP fifo! "
			"Skipped frames %d\n",
			frames_skipped
			);
		frames_skipped++;
	}
}

/*
 * Timestamp the samples and pass them to further processing
 */
void nvtouch_kernel_process(void *samples, int samples_size, u8 report_mode)
{
	if (g_state_kernel.mutithreaded_sample_access_enabled) {
		/* This codepath is only enabled if debugfs replay_trace
		 * feature was used at least once.
		 * Skip update if trace replay is in progress */
		if (mutex_trylock(&g_state_kernel.sample_input_mutex)) {
			nvtouch_kernel_process_locked(samples, samples_size,
					report_mode,
					nvtouch_get_time_us());
			mutex_unlock(&g_state_kernel.sample_input_mutex);
		}
	} else {
		/* Default codepath, if replay_trace was never used */
		nvtouch_kernel_process_locked(samples, samples_size,
				report_mode,
				nvtouch_get_time_us());
	}
}

EXPORT_SYMBOL(nvtouch_kernel_process);

static int device_open(struct inode *inode,
	struct file *file)
{
	int ref_count;

	pr_info("nvtouch device_open(%p)\n", file);

	ref_count = atomic_inc_return(&g_state_kernel.nvtouch_dev_ref_count);

	if (ref_count > 1) {
		pr_err("nvtouch error: more than a single user "
				"attempt to access nvtouch dev node\n");
		atomic_dec(&g_state_kernel.nvtouch_dev_ref_count);
		return -EBUSY;
	}

	return 0;
}

static int device_release(struct inode *inode,
	struct file *file)
{
	atomic_dec(&g_state_kernel.nvtouch_dev_ref_count);
	pr_info("nvtouch device_release(%p,%p)\n", inode, file);
	return 0;
}

char *nvtouch_kernel_get_version_string()
{
	sprintf(g_state_kernel.version_string, "NvTouch umd %d kmd %d",
		g_state_kernel.userspace_config.driver_version_userspace,
		g_state_kernel.userspace_config.driver_version_kernel
	);
	return g_state_kernel.version_string;
}

int nvtouch_ioctl_do_config(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	struct nvtouch_ioctl_config *params;

	if (copy_from_user(&g_state_kernel.config_buf,
			(void __user *)arg, _IOC_SIZE(cmd))) {
		pr_err("NVTOUCH_IOCTL_CONFIG: _IOC IO fail\n");
		return -EFAULT;
	}

	params = &g_state_kernel.config_buf;
	pr_err("NVTOUCH_IOCTL_CONFIG: userspace driver version %d\n",
			params->driver_version_userspace);

	g_state_kernel.userspace_config.driver_version_userspace =
		params->driver_version_userspace;

	params->driver_version_kernel = NVTOUCH_DRIVER_VERSION;

	g_state_kernel.userspace_config.driver_config =
		g_state_kernel.driver_mode;

	if (g_state_dta.client_refcount > 0)
		g_state_kernel.userspace_config.driver_config |=
			NVTOUCH_CONFIG_ENABLE_DTA;
	else
		g_state_kernel.userspace_config.driver_config &=
			~NVTOUCH_CONFIG_ENABLE_DTA;

	memcpy(params, &g_state_kernel.userspace_config,
			sizeof(struct nvtouch_ioctl_config));

	return copy_to_user((void __user *)arg, &g_state_kernel.config_buf,
			_IOC_SIZE(cmd));
}

int nvtouch_ioctl_do_update(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	struct nvtouch_data_frame *data_frame;
	struct nvtouch_ioctl_data *params;
	struct nvtouch_userspace_dta_info info;
	int wait_count;
	u32 in_tune_param;

	if (copy_from_user(&g_state_kernel.ioctl_data_update,
			(void __user *)arg, _IOC_SIZE(cmd))) {
		pr_err("NVTOUCH_IOCTL_UPDATE: _IOC IO fail\n");
		return -EFAULT;
	}

	/*
	 * Don't send data to userspace if only DTA & no
	 * DTA clients connected
	 */
	if (g_state_kernel.driver_mode == NVTOUCH_DRIVER_CONFIG_MODE_VENDOR_DTA
			&& g_state_dta.client_refcount <= 0) {
		/*
		 * NVTOUCH: ioctl IO failed, retrying
		 * no connected clients in vendor dta mode\n"
		 */
		return -EAGAIN;
	}

	params = &g_state_kernel.ioctl_data_update;

	if (g_state_kernel.enable_debug_log &&
			(g_state_kernel.sample_gpfifo->getp & 1023) == 0) {
		pr_err("NVTOUCH_IOCTL_UPDATE %d\n",
			(int)g_state_kernel.sample_gpfifo->getp);
	}

	params->pm_active_to_lp_timeout_ms =
		g_state_kernel.pm_active_to_lp_timeout_ms;
	params->pm_active_to_idle_timeout_ms =
		g_state_kernel.pm_active_to_idle_timeout_ms;

	if (g_state_kernel.send_debug_touch) {
		get_debug_touch(&params->detected_events);
		g_state_kernel.last_debug_send_timestamp =
			params->detected_events.timestamp;
		g_state_kernel.send_debug_touch = 0;
	}
	/* report events */
	if (g_state_kernel.driver_mode != NVTOUCH_DRIVER_CONFIG_MODE_VENDOR_DTA)
		nvtouch_report_events(&params->detected_events);

	/* update unpacked data */

	memset(&g_state_dta.ioctl_dta.detected_events, 0,
	       sizeof(struct nvtouch_events));
	memset(&g_state_dta.ioctl_dta.data, 0, NVTOUCH_SENSOR_DATA_RESERVED);

	if (g_state_dta.client_refcount > 0) {
		int info_offset = NVTOUCH_SENSOR_DATA_RESERVED -
			sizeof(struct nvtouch_userspace_dta_info);
		memcpy(&g_state_dta.ioctl_dta.data,
		       params->unpacked_data, NVTOUCH_SENSOR_DATA_RESERVED);
		memcpy(&g_state_dta.ioctl_dta.detected_events,
		       &params->vendor_events, sizeof(struct nvtouch_events));

		info.left_pixels = g_state_dta.authorized_win_device.left;
		info.top_pixels = g_state_dta.authorized_win_device.top;
		info.right_pixels = g_state_dta.authorized_win_device.right;
		info.bottom_pixels = g_state_dta.authorized_win_device.bottom;
		info.device_height = g_state_dta.device_height;
		info.device_width = g_state_dta.device_width;
		info.orientation = g_state_dta.orientation;
		memcpy(params->unpacked_data+info_offset, &info,
		       sizeof(struct nvtouch_userspace_dta_info));
	}


	/* save events */
	memcpy(&g_state_kernel.last_reported,
			&params->detected_events,
			sizeof(struct nvtouch_events));

	wait_count = 0;

	while (nvtouch_gpfifo_empty(g_state_kernel.sample_gpfifo)) {
		usleep_range(500, 1500);
		wait_count++;
		if (wait_count > 10) {
			wait_event_interruptible(
			g_state_kernel.sample_gpfifo->data_waitqueue,
			!nvtouch_gpfifo_empty(g_state_kernel.sample_gpfifo));
		}

		if (wait_count > 100) {
			/*
			 * fail-safe to release userspace zombie
			 * process
			 * */
			if (g_state_kernel.enable_debug_log)
				pr_err("NVTOUCH ioctl: -EAGAIN\n");
			return -EAGAIN;
		}
	}

	/* Get and copy dataframe to userspace */
	data_frame = nvtouch_gpfifo_get(g_state_kernel.sample_gpfifo);

	memcpy(&params->data_frame, data_frame,
			sizeof(struct nvtouch_data_frame));
	memcpy(&params->vendor_events,
			&g_state_kernel.vendor_events,
			sizeof(struct nvtouch_events));
	wmb();
	g_state_kernel.sample_gpfifo->getp++;

	in_tune_param = (params->driver_config &
			NVTOUCH_CONFIG_TUNE_PARAM_MASK) >>
			NVTOUCH_CONFIG_TUNE_PARAM_SHIFT;

	/* copy debugfs params */
	params->trace_mode = g_state_kernel.trace_mode;
	params->driver_config = g_state_kernel.driver_mode;

	if (g_state_dta.client_refcount > 0)
		params->driver_config |=
			NVTOUCH_CONFIG_ENABLE_DTA;
	else
		params->driver_config &=
			~NVTOUCH_CONFIG_ENABLE_DTA;

	if (g_state_kernel.enable_calibration)
		params->driver_config |=
			NVTOUCH_CONFIG_ENABLE_CALIBRATION;
	else
		params->driver_config &=
			~NVTOUCH_CONFIG_ENABLE_CALIBRATION;

	if (g_state_kernel.tune_debug)
		params->driver_config |=
			NVTOUCH_CONFIG_TUNE_DEBUG;
	else
		params->driver_config &=
			~NVTOUCH_CONFIG_TUNE_DEBUG;

	if (g_state_kernel.enable_calibration_debug)
		params->driver_config |=
			NVTOUCH_CONFIG_ENABLE_CALIBRATION_DEBUG;
	else
		params->driver_config &=
			~NVTOUCH_CONFIG_ENABLE_CALIBRATION_DEBUG;

	params->driver_config |= (g_state_kernel.report_mode <<
			NVTOUCH_CONFIG_REPORT_MODE_SHIFT) &
			NVTOUCH_CONFIG_REPORT_MODE_MASK;

	if (g_state_kernel.tune_debug) {
		params->driver_config |= (g_state_kernel.tune_param <<
				NVTOUCH_CONFIG_TUNE_PARAM_SHIFT) &
				NVTOUCH_CONFIG_TUNE_PARAM_MASK;
	} else {
		g_state_kernel.tune_param = in_tune_param;
	}

	return copy_to_user((void __user *)arg,
			params, _IOC_SIZE(cmd));
}

long nvtouch_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	if ((_IOC_TYPE(cmd) != NVTOUCH_IOCTL_MAGIC) ||
			(_IOC_NR(cmd) == 0) ||
			(_IOC_NR(cmd) > NVTOUCH_IOCTL_LAST) ||
			(_IOC_SIZE(cmd) > NVTOUCH_IOCTL_MAX_ARG_SIZE)) {
		pr_err("_IOC fail\n");
		return -EFAULT;
	}

	if (!g_state_kernel.is_initialized)
		return -EFAULT;

	switch (cmd) {
	case NVTOUCH_IOCTL_CONFIG:
		{
			return nvtouch_ioctl_do_config(filp, cmd, arg);
		}
	case NVTOUCH_IOCTL_UPDATE:
		{
			return nvtouch_ioctl_do_update(filp, cmd, arg);
		}
	default:
		pr_err("unrecognized ioctl cmd: 0x%x\n", cmd);
		return -ENOTTY;
	}

	return 0;
}

static int nvtouch_mmap(struct file *filp, struct vm_area_struct *vma)
{
	/* TODO:  implement shared mem interface */
	return 0;
}


static const struct file_operations fops = {
		.owner = THIS_MODULE,
		.open = device_open,
		.release = device_release,
		.unlocked_ioctl = nvtouch_ioctl,
		.mmap = nvtouch_mmap,
#ifdef CONFIG_COMPAT
	.compat_ioctl = nvtouch_ioctl,
#endif

};

static void report_event(struct nvtouch_event_data *event_data, int delta_id,
		int dx, int dy)
{
	int event_x = event_data->position_x + dx;
	int event_y = event_data->position_y + dy;
#ifdef PROTOCOL_A
	int tracking_id = event_data->tracking_id + delta_id;
#else
	int tracking_id = get_mt_slot_id(event_data->tracking_id + delta_id);
	/* don't report if no empty slot found */
	if (tracking_id < 0)
		return;
	g_state_kernel.input_slot_bits |= 1 << tracking_id;
	input_mt_slot(g_state_kernel.input_device,
			tracking_id);
#endif

	input_report_abs(g_state_kernel.input_device,
			ABS_MT_TRACKING_ID,
			tracking_id);

	input_report_abs(g_state_kernel.input_device,
			ABS_MT_TOUCH_MAJOR,
			event_data->touch_major);

	input_report_abs(g_state_kernel.input_device,
			ABS_MT_WIDTH_MAJOR,
			event_data->width_major);

	input_report_abs(g_state_kernel.input_device,
			ABS_MT_PRESSURE,
			event_data->pressure);

	input_report_abs(g_state_kernel.input_device,
			ABS_MT_POSITION_X, /* TODO: fix xy swap */
			event_y);

	input_report_abs(g_state_kernel.input_device,
			ABS_MT_POSITION_Y, /* TODO: fix xy swap */
			event_x);

	if (g_state_kernel.enable_debug_log && !g_state_kernel.update_count)
		pr_err("NVTOUCH nvtouch_report_events x = %d, y = %d id = %d\n",
				event_x, event_y,
				event_data->tracking_id + delta_id);

	switch (event_data->tool) {
	case NVTOUCH_EVENT_TOOL_PEN:
		input_report_abs(g_state_kernel.input_device,
				ABS_MT_TOOL_TYPE,
				MT_TOOL_PEN);
		break;
	case NVTOUCH_EVENT_TOOL_FINGER:
		input_report_abs(g_state_kernel.input_device,
				ABS_MT_TOOL_TYPE,
				MT_TOOL_FINGER);
		break;
	case NVTOUCH_EVENT_TOOL_ERASER:
		input_report_key(g_state_kernel.input_device,
				BTN_TOUCH, 1);
		input_report_key(g_state_kernel.input_device,
				BTN_TOOL_RUBBER, 1);
		break;
	}
#ifdef PROTOCOL_A
	input_mt_sync(g_state_kernel.input_device);
#endif
}

static void reset_unused_mt(u32 prev_input_slot_bits)
{
	int i;
	/* reset empty protocol B slots */
	for (i = 0; i < NV_TOUCH_COUNT_MAX; i++) {
		if ((prev_input_slot_bits & (1 << i)) != 0 &&
			(g_state_kernel.input_slot_bits & (1 << i)) == 0) {
			input_mt_slot(g_state_kernel.input_device, i);
			input_mt_report_slot_state(g_state_kernel.input_device,
					MT_TOOL_FINGER,
					false);
			clear_mt_slot(i);
		}
	}
}

/* static int gCounter = 0; */
void nvtouch_report_events(struct nvtouch_events *touch_events)
{
	int i;
	int nvtouch_delta_x = 0;
	int nvtouch_delta_y = 0;
	int vendor_delta_x = 0;
	int vendor_delta_y = 0;
	int vendor_delta_id = 0;
	int all_event_count = touch_events->event_count;

	u32 prev_input_slot_bits = g_state_kernel.input_slot_bits;

	touch_events->timestamp = nvtouch_get_time_us();

	switch (g_state_kernel.driver_mode) {
	case NVTOUCH_DRIVER_CONFIG_MODE_NVTOUCH_ONLY:
		all_event_count = touch_events->event_count;
		if (touch_events->flags & NVTOUCH_REPORT_FLAG_SKIP) {
			/* skip reporting */
			return;
		}
		break;
	case NVTOUCH_DRIVER_CONFIG_MODE_VENDOR_ONLY:
		/* events are reported via vendor driver. */
		return;
	case NVTOUCH_DRIVER_CONFIG_MODE_DUAL:
		all_event_count = 0;

		if (g_state_kernel.dual_mode_enable_nvtouch_events)
			all_event_count += touch_events->event_count;

		if (g_state_kernel.dual_mode_enable_vendor_events)
			all_event_count +=
				g_state_kernel.vendor_events.event_count;

		nvtouch_delta_x = g_state_kernel.dual_mode_nvtouch_delta_x;
		nvtouch_delta_y = g_state_kernel.dual_mode_nvtouch_delta_y;
		vendor_delta_x = g_state_kernel.dual_mode_vendor_delta_x;
		vendor_delta_y = g_state_kernel.dual_mode_vendor_delta_y;
		/* need to separate nvtouch and vendor events */
		vendor_delta_id = 10;
		break;
	default:
		BUG_ON("Unsupported mode");
		break;
	}

	if (g_state_kernel.enable_debug_log) {
		u64 current_time = nvtouch_get_time_us();
		g_state_kernel.update_time_acc +=
				(u32)(current_time-touch_events->timestamp);
		g_state_kernel.update_count++;
		if (g_state_kernel.update_count > 250) {
			int fps = 0;
			if (g_state_kernel.last_print_timestamp) {
				fps = current_time -
					g_state_kernel.last_print_timestamp;
				fps = fps / g_state_kernel.update_count;
				fps = 1000000 / fps;
			}
#ifdef NVTOUCH_DEBUG
			if (g_state_kernel.enable_debug_log)
				pr_err(
			"Average processing time = %d us, avg fps = %d\n",
				g_state_kernel.update_time_acc /
					g_state_kernel.update_count, fps);
#endif
			g_state_kernel.last_print_timestamp = current_time;
			g_state_kernel.update_time_acc = 0;
			g_state_kernel.update_count = 0;
		}
	}

	g_state_kernel.input_slot_bits = 0;

	if (!all_event_count) {
		if (g_state_kernel.last_event_count) {
#ifdef PROTOCOL_A
			input_report_key(g_state_kernel.input_device,
					BTN_TOUCH, 0);
			input_mt_sync(g_state_kernel.input_device);
#else
			reset_unused_mt(prev_input_slot_bits);
#endif
			input_sync(g_state_kernel.input_device);
		}
		g_state_kernel.last_event_count = 0;
		return;
	}

	if (g_state_kernel.debug_blink_on_event) {
		g_state_kernel.backlight_device->props.brightness = 0;
		g_state_kernel.backlight_device->ops->update_status(
				g_state_kernel.backlight_device
		);
		g_state_kernel.debug_blink_on_event = 0;
	}


	if (g_state_kernel.driver_mode ==
			NVTOUCH_DRIVER_CONFIG_MODE_DUAL
			&& g_state_kernel.dual_mode_enable_vendor_events) {
		for (i = 0; i < g_state_kernel.vendor_events.event_count; i++) {
			struct nvtouch_event_data *event_data =
				&g_state_kernel.vendor_events.events[i];
			report_event(event_data, vendor_delta_id,
					vendor_delta_x, vendor_delta_y);
		}
	}

	if ((g_state_kernel.driver_mode ==
			NVTOUCH_DRIVER_CONFIG_MODE_NVTOUCH_ONLY)
		|| ((g_state_kernel.driver_mode ==
			NVTOUCH_DRIVER_CONFIG_MODE_DUAL)
		&& g_state_kernel.dual_mode_enable_nvtouch_events)) {
		for (i = 0; i < touch_events->event_count; i++) {
			struct nvtouch_event_data *event_data =
				&touch_events->events[i];
			report_event(event_data, 0, 0, 0);
		}
	}
#ifndef PROTOCOL_A
	reset_unused_mt(prev_input_slot_bits);
#endif
	input_sync(g_state_kernel.input_device);
	g_state_kernel.last_event_count = all_event_count;
}

u32 nvtouch_get_driver_mode(void)
{
		return g_state_kernel.driver_mode;
}
EXPORT_SYMBOL(nvtouch_get_driver_mode);

u64 nvtouch_get_time_us(void)
{
	struct timespec *pts;
#if 1
	/* High precision but slower timer */
	static struct timespec ts;
	getnstimeofday(&ts);
	pts = &ts;
#else
	/* Very fast but not precise (~1ms) time */
	pts = &xtime;
#endif
	 return pts->tv_sec * 1000000 + pts->tv_nsec / 1000;
}
EXPORT_SYMBOL(nvtouch_get_time_us);

void nvtouch_kernel_init(int sensor_w, int sensor_h, int screen_w,
		int screen_h, int invert_x, int invert_y,
		struct input_dev *input_device) {

	g_state_kernel.input_device = input_device;

	__set_bit(BTN_TOOL_RUBBER, input_device->keybit);
	input_set_abs_params(input_device, ABS_MT_TOOL_TYPE, 0,
			MT_TOOL_MAX, 0, 0);

	g_state_kernel.userspace_config.sensor_w = sensor_w;
	g_state_kernel.userspace_config.sensor_h = sensor_h;
	g_state_kernel.userspace_config.screen_w = screen_w;
	g_state_kernel.userspace_config.screen_h = screen_h;
	g_state_kernel.userspace_config.invert_x = invert_x;
	g_state_kernel.userspace_config.invert_y = invert_y;
	g_state_dta.ioctl_dta.sensor_img_width = sensor_w;
	g_state_dta.ioctl_dta.sensor_img_height = sensor_h;
	g_state_dta.ioctl_dta.invert_x = invert_x;
	g_state_dta.ioctl_dta.invert_y = invert_y;

	wmb();
	g_state_kernel.is_initialized = 1;
}
EXPORT_SYMBOL(nvtouch_kernel_init);

void nvtouch_debug_expose_u32(const char *name, u32 *var)
{
	debugfs_create_u32(name, 0600,
			g_state_kernel.debug_dfs, var);
}

/*
 * DTA device
 */
long nvtouch_dta_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long err = 0;
	if ((_IOC_TYPE(cmd) != NVTOUCH_DTA_IOCTL_MAGIC) ||
			(_IOC_NR(cmd) == 0) ||
			(_IOC_NR(cmd) > NVTOUCH_DTA_IOCTL_LAST)) {
		pr_err("DTA _IOC fail %d\n", _IOC_NR(cmd));
		return -EFAULT;
	}

	if (!g_state_kernel.is_initialized)
		return -EFAULT;

	spin_lock(&g_state_dta.slock);
	switch (cmd) {
	case NVTOUCH_DTA_IOCTL_CONFIG:
	{
		BUG_ON(sizeof(struct nvtouch_ioctl_dta_config) !=
				_IOC_SIZE(cmd));

		if (copy_from_user(&g_state_dta.ioctl_dta_config,
				(void __user *)arg,
					_IOC_SIZE(cmd))) {
			pr_err("NVTOUCH_DTA_IOCTL_CONFIG: _IOC IO fail\n");
			err = -EFAULT;
			break;
		}
		g_state_dta.ioctl_dta_config.sensor_w =
			g_state_kernel.userspace_config.sensor_w;
		g_state_dta.ioctl_dta_config.sensor_h =
			g_state_kernel.userspace_config.sensor_h;
		g_state_dta.ioctl_dta_config.screen_w =
			g_state_kernel.userspace_config.screen_w;
		g_state_dta.ioctl_dta_config.screen_h =
			g_state_kernel.userspace_config.screen_h;
		g_state_dta.ioctl_dta_config.invert_x =
			g_state_kernel.userspace_config.invert_x;
		g_state_dta.ioctl_dta_config.invert_y =
			g_state_kernel.userspace_config.invert_y;
		g_state_dta.ioctl_dta_config.driver_version_kernel =
				NVTOUCH_DRIVER_VERSION;

		err = copy_to_user((void __user *)arg,
				&g_state_dta.ioctl_dta_config,
				_IOC_SIZE(cmd));
		break;
	}
	case NVTOUCH_DTA_IOCTL_READ:
	case NVTOUCH_DTA_IOCTL_READ_OLD:
	{
		BUG_ON(sizeof(struct nvtouch_ioctl_dta) != _IOC_SIZE(cmd) &&
				/* WAR for compatibility with old apps*/
				sizeof(struct nvtouch_ioctl_dta_old) !=
				_IOC_SIZE(cmd));

		if (g_state_dta.authorized_pid == NVTOUCH_DTA_ADMIN_NULL_PID
			|| current->tgid != g_state_dta.authorized_pid) {
			err = -EFAULT;
			break;
		}

		if (g_state_dta.send_debug_touch) {
			memcpy(&g_state_dta.ioctl_dta.detected_events,
					&g_state_kernel.last_reported,
					sizeof(struct nvtouch_events));

			get_debug_touch(
				&g_state_dta.ioctl_dta.detected_events);

			g_state_kernel.last_debug_send_timestamp =
				nvtouch_get_time_us();

			g_state_dta.send_debug_touch = 0;
		} else {
		}

		err = copy_to_user((void __user *)arg,
				&g_state_dta.ioctl_dta,
				sizeof(struct nvtouch_ioctl_dta));
		break;
	}
	default:
		pr_err("NvtouchDTA: unrecognized ioctl cmd: 0x%x\n", cmd);
		err = -ENOTTY;
		break;
	}
	spin_unlock(&g_state_dta.slock);

	return err;
}

static int dta_device_open(struct inode *inode,
		struct file *file)
{
	spin_lock(&g_state_dta.slock);
	g_state_dta.client_refcount++;
	spin_unlock(&g_state_dta.slock);

	pr_err("dta device_open(%p)\n", file);
	return 0;
}

static int dta_device_release(struct inode *inode,
		struct file *file)
{
	spin_lock(&g_state_dta.slock);
	g_state_dta.client_refcount--;
	spin_unlock(&g_state_dta.slock);
	pr_err("dta device_release(%p,%p)\n", inode, file);
	return 0;
}

struct class *nvtouch_dta_cdev_class;
#define NVTOUCH_DTA_MAJOR 101
static const struct file_operations dta_fops = {
		.owner = THIS_MODULE,
		.open = dta_device_open,
		.release = dta_device_release,
		.unlocked_ioctl = nvtouch_dta_ioctl,
#ifdef CONFIG_COMPAT
		.compat_ioctl = nvtouch_dta_ioctl,
#endif

};

/*
 * DTA admin device
 */
long nvtouch_dta_admin_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	long err = 0;
	if ((_IOC_TYPE(cmd) != NVTOUCH_DTA_ADMIN_IOCTL_MAGIC) ||
		(_IOC_NR(cmd) == 0) ||
		(_IOC_NR(cmd) > NVTOUCH_DTA_ADMIN_IOCTL_LAST) ||
		(_IOC_SIZE(cmd) > NVTOUCH_DTA_ADMIN_IOCTL_MAX_ARG_SIZE)) {
		pr_err("DTA ADMIN _IOC fail\n");
		return -EFAULT;
	}

	if (!g_state_kernel.is_initialized)
		return -EFAULT;

	spin_lock(&g_state_dta.slock);

	switch (cmd) {
	case NVTOUCH_DTA_ADMIN_IOCTL_CONFIG:
	{
		if (copy_from_user(&g_state_dta.ioctl_dta_admin_config,
				(void __user *)arg,
					_IOC_SIZE(cmd))) {
			pr_err("NVTOUCH_DTA_ADMIN_IOCTL_CONFIG: _IOC IO fail\n");
			err = -EFAULT;
			break;
		}

		err = copy_to_user((void __user *)arg,
				&g_state_dta.ioctl_dta_admin_config,
				_IOC_SIZE(cmd));
		break;
	}
	case NVTOUCH_DTA_ADMIN_IOCTL_UPDATE:
	{
		int sensor_w = g_state_kernel.userspace_config.sensor_w;
		int sensor_h = g_state_kernel.userspace_config.sensor_h;

		if (copy_from_user(&g_state_dta.ioctl_dta_admin_update,
				(void __user *)arg,
					_IOC_SIZE(cmd))) {
			pr_err("NVTOUCH_DTA_ADMIN_IOCTL_UPDATE IO fail\n");
			err = -EFAULT;
			break;
		}
		g_state_dta.authorized_pid =
			g_state_dta.ioctl_dta_admin_update.pid;
		g_state_dta.authorized_win_device.left =
			g_state_dta.ioctl_dta_admin_update.left_pixels;
		g_state_dta.authorized_win_device.right =
			g_state_dta.ioctl_dta_admin_update.right_pixels;
		g_state_dta.authorized_win_device.bottom =
			g_state_dta.ioctl_dta_admin_update.bottom_pixels;
		g_state_dta.authorized_win_device.top =
			g_state_dta.ioctl_dta_admin_update.top_pixels;
		g_state_dta.device_width =
			g_state_dta.ioctl_dta_admin_update.device_width;
		g_state_dta.device_height =
			g_state_dta.ioctl_dta_admin_update.device_height;
		g_state_dta.orientation =
			g_state_dta.ioctl_dta_admin_update.orientation;

		switch (g_state_dta.orientation) {
		case NVTOUCH_DTA_ADMIN_ORIENTATION_90:
		case NVTOUCH_DTA_ADMIN_ORIENTATION_270:
			g_state_dta.ioctl_dta.sensor_img_width = sensor_h;
			g_state_dta.ioctl_dta.sensor_img_height = sensor_w;
			break;
		case NVTOUCH_DTA_ADMIN_ORIENTATION_180:
		default:
			g_state_dta.ioctl_dta.sensor_img_width = sensor_w;
			g_state_dta.ioctl_dta.sensor_img_height = sensor_h;
			break;
		}
		g_state_dta.ioctl_dta.orientation = g_state_dta.orientation;

		break;
	}
	case NVTOUCH_DTA_ADMIN_IOCTL_QUERY:
	{
		g_state_dta.ioctl_dta_admin_query.connected_clients =
				g_state_dta.client_refcount;
		err = copy_to_user((void __user *)arg,
				&g_state_dta.ioctl_dta_admin_query,
				_IOC_SIZE(cmd));
		break;
	}
	case NVTOUCH_DTA_ADMIN_IOCTL_SEND_DEBUG_TOUCH:
	{
		g_state_dta.send_debug_touch = 1;
		if (copy_from_user(&g_state_dta.ioctl_dta_debug_touch_send,
				(void __user *)arg,
					_IOC_SIZE(cmd))) {
			err = -EFAULT;
			break;
		}
		err = copy_to_user((void __user *)arg,
				&g_state_dta.ioctl_dta_debug_touch_send,
				_IOC_SIZE(cmd));
		break;
	}
	case NVTOUCH_DTA_ADMIN_IOCTL_RECV_DEBUG_TOUCH:
	{
		if (copy_from_user(&g_state_dta.ioctl_dta_debug_touch_recv,
				(void __user *)arg,
					_IOC_SIZE(cmd))) {
			err = -EFAULT;
			break;
		}
		/* copy back to userspace timestamp of when debug touch was
		 sent from kernel */
		g_state_dta.ioctl_dta_debug_touch_recv.timestamp =
				g_state_kernel.last_debug_send_timestamp;
		err = copy_to_user((void __user *)arg,
				&g_state_dta.ioctl_dta_debug_touch_recv,
				_IOC_SIZE(cmd));
		break;
	}
	default:
		pr_err("NvtouchDTAAdmin: unrecognized ioctl cmd: 0x%x\n", cmd);
		err = -ENOTTY;
	}
	spin_unlock(&g_state_dta.slock);
	return err;
}

static int dta_admin_device_open(struct inode *inode,
		struct file *file)
{
	pr_info("dta admin device_open(%p)\n", file);
	return 0;
}

static int dta_admin_device_release(struct inode *inode,
		struct file *file)
{
	pr_info("dta admin device_release(%p,%p)\n", inode, file);
	return 0;
}

struct class *nvtouch_dta_admin_cdev_class;
#define NVTOUCH_DTA_ADMIN_MAJOR 102
static const struct file_operations dta_admin_fops = {
		.owner = THIS_MODULE,
		.open = dta_admin_device_open,
		.release = dta_admin_device_release,
		.unlocked_ioctl = nvtouch_dta_admin_ioctl,
#ifdef CONFIG_COMPAT
		.compat_ioctl = nvtouch_dta_admin_ioctl,
#endif

};

int create_dta_dev_nodes(void)
{
	int err = 0;

	if (register_chrdev(NVTOUCH_DTA_ADMIN_MAJOR, DEVICE_FILE_NAME_DTA_ADMIN,
				&dta_admin_fops)) {
		pr_err("Nvtouch driver: Unable to register DTA driver\n");
		return -ENODEV;
	}
	nvtouch_dta_admin_cdev_class = class_create(THIS_MODULE,
			DEVICE_FILE_NAME_DTA_ADMIN);
	if (IS_ERR(nvtouch_dta_admin_cdev_class)) {
		err = PTR_ERR(nvtouch_dta_admin_cdev_class);
		goto out_dta_admin_chrdev;
	}
	device_create(nvtouch_dta_admin_cdev_class, NULL,
			MKDEV(NVTOUCH_DTA_ADMIN_MAJOR, 0),
			NULL, DEVICE_FILE_NAME_DTA_ADMIN);

	/* create dta node */
	if (register_chrdev(NVTOUCH_DTA_MAJOR, DEVICE_FILE_NAME_DTA,
				&dta_fops)) {
		pr_err("Nvtouch driver: Unable to register DTA driver\n");
		unregister_chrdev(NVTOUCH_DTA_ADMIN_MAJOR,
				DEVICE_FILE_NAME_DTA_ADMIN);
		return -ENODEV;
	}
	nvtouch_dta_cdev_class = class_create(THIS_MODULE,
			DEVICE_FILE_NAME_DTA);
	if (IS_ERR(nvtouch_dta_cdev_class)) {
		err = PTR_ERR(nvtouch_dta_cdev_class);
		goto out_dta_chrdev;
	}
	device_create(nvtouch_dta_cdev_class, NULL,
			MKDEV(NVTOUCH_DTA_MAJOR, 0),
			NULL, DEVICE_FILE_NAME_DTA);

	goto out_dta;

out_dta_chrdev:
	unregister_chrdev(NVTOUCH_DTA_MAJOR, DEVICE_FILE_NAME_DTA);
out_dta_admin_chrdev:
	unregister_chrdev(NVTOUCH_DTA_ADMIN_MAJOR, DEVICE_FILE_NAME_DTA_ADMIN);
out_dta:
	pr_err("XXX - registered NVTouch DTA driver\n");
	return err;
}

void unregister_dta_dev_nodes(void)
{
	unregister_chrdev(NVTOUCH_DTA_ADMIN_MAJOR, DEVICE_FILE_NAME_DTA_ADMIN);
	unregister_chrdev(NVTOUCH_DTA_MAJOR, DEVICE_FILE_NAME_DTA);
}

struct class *nvtouch_cdev_class;
struct device *nvtouch_cdev;
dev_t *nv_dev_first;
#define NVTOUCH_MAJOR 100

static int __init nvtouch_cdev_init(void)
{
	int err = 0;
	int i;

	atomic_set(&g_state_kernel.nvtouch_dev_ref_count, 0);

	if (register_chrdev(NVTOUCH_MAJOR, "nvtouch", &fops)) {
		pr_err("Nvtouch driver: Unable to register driver\n");
		return -ENODEV;
	}
	nvtouch_cdev_class = class_create(THIS_MODULE, "nvtouch");
	if (IS_ERR(nvtouch_cdev_class)) {
		err = PTR_ERR(nvtouch_cdev_class);
		goto out_chrdev;
	}

	g_state_kernel.nvtouch_dev =
			device_create(nvtouch_cdev_class, NULL,
			MKDEV(NVTOUCH_MAJOR, 0), NULL, "nvtouch");

	if (!g_state_kernel.nvtouch_dev) {
		class_destroy(nvtouch_cdev_class);
		err = -ENODEV;
		goto out_chrdev;
	}

	if (sysfs_create_file(&g_state_kernel.nvtouch_dev->kobj,
				&dev_attr_driver_mode.attr)) {
			WARN_ON("No sysfs attributes exposed for NvTouch!\n");
	}

	g_state_kernel.userspace_config.driver_version_kernel =
			NVTOUCH_DRIVER_VERSION;

	g_state_dta.authorized_pid = NVTOUCH_DTA_ADMIN_NULL_PID;
	memset(&g_state_dta.authorized_win_device, 0,
	       sizeof(struct nvtouch_rect));
	g_state_dta.device_width = 1920;
	g_state_dta.device_height = 1200;
	g_state_dta.client_refcount = 0;
	g_state_dta.send_debug_touch = 0;
	g_state_dta.authorized_pid = NVTOUCH_DTA_ADMIN_NULL_PID;

	g_state_kernel.time_of_last_touch = nvtouch_get_time_us();
	g_state_kernel.input_slot_bits = 0;

	g_state_kernel.driver_mode = NVTOUCH_DRIVER_CONFIG_MODE_VENDOR_ONLY;

	g_state_kernel.dual_mode_enable_nvtouch_events = 1;
	g_state_kernel.dual_mode_enable_vendor_events = 1;

	g_state_kernel.enable_calibration = 1;
	g_state_kernel.enable_calibration_debug = 1;

	g_state_kernel.tune_debug = 0;
	g_state_kernel.tune_param = 119;

#ifdef NVTOUCH_DEBUG
	g_state_kernel.debug_dfs = debugfs_create_dir("nvtouch", NULL);

	EXPOSE_VALUE_U32(g_state_kernel.enable_debug_log);
	EXPOSE_VALUE_U32(g_state_kernel.trace_mode);

	EXPOSE_VALUE_U32(g_state_kernel.dual_mode_vendor_delta_x);
	EXPOSE_VALUE_U32(g_state_kernel.dual_mode_vendor_delta_y);
	EXPOSE_VALUE_U32(g_state_kernel.dual_mode_nvtouch_delta_x);
	EXPOSE_VALUE_U32(g_state_kernel.dual_mode_nvtouch_delta_y);

	EXPOSE_VALUE_U32(g_state_kernel.dual_mode_enable_nvtouch_events);
	EXPOSE_VALUE_U32(g_state_kernel.dual_mode_enable_vendor_events);
	EXPOSE_VALUE_U32(g_state_kernel.pm_active_to_lp_timeout_ms);
	EXPOSE_VALUE_U32(g_state_kernel.pm_active_to_idle_timeout_ms);

	EXPOSE_VALUE_U32(g_state_kernel.enable_calibration);
	EXPOSE_VALUE_U32(g_state_kernel.enable_calibration_debug);

	EXPOSE_VALUE_U32(g_state_kernel.tune_debug);
	EXPOSE_VALUE_U32(g_state_kernel.tune_param);
#endif
	g_state_kernel.pm_active_to_lp_timeout_ms = 3000;
	g_state_kernel.pm_active_to_idle_timeout_ms = 20;

	for (i = 0; i < NVTOUCH_MT_SLOT_COUNT; i++)
		clear_mt_slot(i);

	init_debug_blink_on_touch();
	nvtouch_init_blobs(g_state_kernel.debug_dfs);
	spin_lock_init(&g_state_dta.slock);

	g_state_kernel.sample_gpfifo =
			nvtouch_gpfifo_create(GPFIFO_SIZE_SPI);

	BUG_ON(!g_state_kernel.sample_gpfifo);

	debugfs_create_file("replay_trace", 0600,
			g_state_kernel.debug_dfs, NULL,
			&trace_replay_ops);

	create_dta_dev_nodes();

	goto out;

out_chrdev:
	unregister_chrdev(NVTOUCH_MAJOR, "nvtouch");

out:
	g_state_kernel.is_initialized = 0;
	pr_err("XXX - registered NVTouch driver\n");
	return err;
}

 /* Cleanup - unregister the appropriate file from /proc */
static void cleanup_devnode(void)
{
	/* Unregister the device */
	unregister_chrdev(MAJOR_NUM, DEVICE_NAME);
	unregister_dta_dev_nodes();
}

module_init(nvtouch_cdev_init);
module_exit(cleanup_devnode);
