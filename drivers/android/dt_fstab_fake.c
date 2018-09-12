/*
 * Copyright (C) 2018, Paul Keith <javelinanddart@gmail.com>
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define DT_FILE_INIT(_name, _contents) \
static int dt_show_##_name (struct seq_file *m, void *v) \
{ \
	seq_printf(m, "%s\n", _contents); \
	return 0; \
} \
\
static int dt_open_##_name (struct inode *inode, struct file *file) \
{ \
	return single_open(file, dt_show_##_name , NULL); \
} \
\
static struct file_operations dt_file_ops_##_name = { \
	.owner = THIS_MODULE, \
	.open = dt_open_##_name, \
	.read = seq_read, \
	.llseek = seq_lseek,\
	.release = single_release \
};

#define DT_PARTITION_INIT(_name, _block, _fsmgr, _mnt, _type) \
DT_FILE_INIT(compatible_##_name, "android,"#_name)  \
DT_FILE_INIT(dev_##_name, _block)  \
DT_FILE_INIT(fsmgr_##_name, _fsmgr) \
DT_FILE_INIT(mnt_##_name, _mnt) \
DT_FILE_INIT(name_##_name, #_name) \
DT_FILE_INIT(status_##_name, "okay") \
DT_FILE_INIT(type_##_name, _type)

#define DT_DIR_CREATE(_path) \
if (proc_mkdir(_path, NULL) == NULL) { \
	pr_err("%s: Failed to create proc/"_path"\n", __func__); \
	return 1; \
}

#define DT_FILE_CREATE(_name, _path) \
if (proc_create(_path, 0, NULL, &dt_file_ops_##_name ) == NULL) { \
	pr_err("%s: Failed to create proc/"_path"\n", __func__); \
	return 1; \
}

#define DT_PARTITION_CREATE(_name) \
DT_DIR_CREATE("device-tree/firmware/android/fstab/"#_name) \
DT_FILE_CREATE(compatible_##_name, "device-tree/firmware/android/fstab/"#_name"/compatible") \
DT_FILE_CREATE(dev_##_name, "device-tree/firmware/android/fstab/"#_name"/dev") \
DT_FILE_CREATE(fsmgr_##_name, "device-tree/firmware/android/fstab/"#_name"/fsmgr_flags") \
DT_FILE_CREATE(mnt_##_name, "device-tree/firmware/android/fstab/"#_name"/mnt_flags") \
DT_FILE_CREATE(name_##_name, "device-tree/firmware/android/fstab/"#_name"/name") \
DT_FILE_CREATE(status_##_name, "device-tree/firmware/android/fstab/"#_name"/status") \
DT_FILE_CREATE(type_##_name, "device-tree/firmware/android/fstab/"#_name"/type")

#define DT_REMOVE(_path) \
remove_proc_entry(_path, NULL);

#define DT_PARTITION_REMOVE(_name) \
DT_REMOVE("device-tree/firmware/android/fstab/"#_name"/type") \
DT_REMOVE("device-tree/firmware/android/fstab/"#_name"/status") \
DT_REMOVE("device-tree/firmware/android/fstab/"#_name"/name") \
DT_REMOVE("device-tree/firmware/android/fstab/"#_name"/mnt_flags") \
DT_REMOVE("device-tree/firmware/android/fstab/"#_name"/fsmgr_flags") \
DT_REMOVE("device-tree/firmware/android/fstab/"#_name"/dev") \
DT_REMOVE("device-tree/firmware/android/fstab/"#_name"/compatible") \
DT_REMOVE("device-tree/firmware/android/fstab/"#_name)

// Setup basic hierarchy
DT_FILE_INIT(fstab_compatible, "android,fstab")
DT_FILE_INIT(fstab_name, "fstab")

static int __init dt_fstab_proc_init(void)
{
	// Setup basic hierarchy
	DT_DIR_CREATE("device-tree/firmware/android/fstab")
	DT_FILE_CREATE(fstab_compatible, "device-tree/firmware/android/fstab/compatible")
	DT_FILE_CREATE(fstab_name, "device-tree/firmware/android/fstab/name")

	return 0;
}

static void __exit dt_fstab_proc_exit(void)
{
	// Cleanup basic hierarchy
	DT_REMOVE("device-tree/firmware/android/fstab/name")
	DT_REMOVE("device-tree/firmware/android/fstab/compatible")
	DT_REMOVE("device-tree/firmware/android/fstab")
}

module_init(dt_fstab_proc_init);
module_exit(dt_fstab_proc_exit);

MODULE_DESCRIPTION("Android fstab driver");
MODULE_AUTHOR("Paul Keith <javelinanddart@gmail.com>");
MODULE_LICENSE("GPL");
