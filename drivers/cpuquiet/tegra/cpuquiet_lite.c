/*
 * Copyright (C) 2018 Artyom Bambalov <artem-bambalov@yandex.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/cpu.h>
#include <linux/fb.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define VERSION "1.3.0"

#define CPUS_DOWN_DELAY_MS	5000

static struct cpuquiet_lite_data {
	struct work_struct 		cpuquiet_lite_work;
	struct workqueue_struct *cpuquiet_lite_wq;
	struct notifier_block 	notif;
	unsigned int 			prev_online_cpus;
	bool					suspended;
	struct delayed_work 	panel_work;
} cpuquiet_l_data;

static void __cpuinit tegra_cpuquiet_lite_work_func(struct work_struct *work)
{
	unsigned int cpunumber;
	unsigned int online_cpus = cpuquiet_l_data.prev_online_cpus;

	pr_info("%s: suspended = %d, last_online_cpus = %d\n", __func__,
			cpuquiet_l_data.suspended,
			cpuquiet_l_data.prev_online_cpus);

	for (cpunumber = 1; cpunumber < online_cpus; cpunumber++) {
		if (cpuquiet_l_data.suspended) {
			cpu_down(cpunumber);
		} else {
			cpu_up(cpunumber);
		}
	}
}

static void panel_state_work__func(struct work_struct *work) {
	queue_work(cpuquiet_l_data.cpuquiet_lite_wq, &cpuquiet_l_data.cpuquiet_lite_work);
}

static int fb_notifier_callback_func(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	switch (*blank) {
	case FB_BLANK_UNBLANK:
		if (delayed_work_pending(&cpuquiet_l_data.panel_work))
			cancel_delayed_work_sync(&cpuquiet_l_data.panel_work);

		cpuquiet_l_data.suspended = false;
		queue_work(cpuquiet_l_data.cpuquiet_lite_wq, &cpuquiet_l_data.cpuquiet_lite_work);
		break;
	default:
		if (delayed_work_pending(&cpuquiet_l_data.panel_work))
			cancel_delayed_work_sync(&cpuquiet_l_data.panel_work);
		else
			cpuquiet_l_data.prev_online_cpus = num_online_cpus();

		cpuquiet_l_data.suspended = true;
		queue_delayed_work_on(0, cpuquiet_l_data.cpuquiet_lite_wq, 
				&cpuquiet_l_data.panel_work,
				msecs_to_jiffies(CPUS_DOWN_DELAY_MS));
	}

	return NOTIFY_OK;
}

static struct notifier_block fb_notifier_callback = {
	.notifier_call = fb_notifier_callback_func,
};

static int cpuquiet_lite_init(void)
{
	pr_info("%s: cpuquiet lite version %s\n", __func__, VERSION);
	cpuquiet_l_data.cpuquiet_lite_wq = alloc_workqueue(
		"cpuquiet_lite", WQ_NON_REENTRANT | WQ_FREEZABLE, 1);

	if (!cpuquiet_l_data.cpuquiet_lite_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&cpuquiet_l_data.panel_work, panel_state_work__func);
	INIT_WORK(&cpuquiet_l_data.cpuquiet_lite_work, tegra_cpuquiet_lite_work_func);
	fb_register_client(&fb_notifier_callback);
	cpuquiet_l_data.prev_online_cpus = num_online_cpus();

	return 0;
}
late_initcall(cpuquiet_lite_init);
