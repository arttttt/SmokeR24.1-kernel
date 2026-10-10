/*
 * VDSO implementation for AArch32. Derived from arm64/kernel/vdso.c
 *
 * Copyright (c) 2009-2014, NVIDIA CORPORATION.  All rights reserved.
 * Original Copyright (C) 2012 ARM Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#include <linux/kernel.h>
#include <linux/clocksource.h>
#include <linux/elf.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/timekeeper_internal.h>
#include <linux/vmalloc.h>

#include <asm/cacheflush.h>
#include <asm/signal.h>
#include <asm/vdso.h>
#include <asm/vdso_datapage.h>

extern char vdso32_start, vdso32_end;
static unsigned long vdso32_pages;
static struct page **vdso32_pagelist;


/*
 * The vDSO data page.
 */
static union {
	struct vdso_data	data;
	u8			page[PAGE_SIZE];
} vdso_data_store __page_aligned_data;
struct vdso_data *vdso_data = &vdso_data_store.data;


static int __init vdso_init(void)
{
	struct page *pg;
	char *vbase32;
	int i, ret = 0;

	vdso32_pages = (&vdso32_end - &vdso32_start) >> PAGE_SHIFT;
	pr_info("vdso32: %ld pages (%ld code, %ld data) at base %p\n",
		vdso32_pages + 1, vdso32_pages, 1L, &vdso32_start);

	vdso32_pagelist = kzalloc(sizeof(struct page *) * (vdso32_pages + 1),
				   GFP_KERNEL);
	if (vdso32_pagelist == NULL) {
		pr_err("Failed to allocate vDSO32 pagelist!\n");
		return -ENOMEM;
	}

	/* Grab the vDSO code pages. */
	for (i = 0; i < vdso32_pages; i++) {
		pg = virt_to_page(&vdso32_start + i*PAGE_SIZE);
		ClearPageReserved(pg);
		get_page(pg);
		vdso32_pagelist[i] = pg;
	}

	/* Sanity check the shared object header. */
	vbase32 = vmap(vdso32_pagelist, 1, 0, PAGE_KERNEL);
	if (vbase32 == NULL) {
		pr_err("Failed to map vDSO pagelist!\n");
		return -ENOMEM;
	} else if (memcmp(vbase32, "\177ELF", 4)) {
		pr_err("vDSO 32 is not a valid ELF object!\n");
		ret = -EINVAL;
		goto unmap32;
	}

	/* Grab the vDSO data page. */
	pg = virt_to_page(vdso_data);
	get_page(pg);
	vdso32_pagelist[i] = pg;

unmap32:
	vunmap(vbase32);
	return ret;
}
arch_initcall(vdso_init);

int vdso_setup_additional_pages(struct linux_binprm *bprm,
				int uses_interp)
{
	struct mm_struct *mm = current->mm;
	unsigned long vdso_base, vdso_mapping_len;
	struct page **local_vdso_pagelist;
	unsigned long local_vdso_pages;
	int ret;

	local_vdso_pagelist = vdso32_pagelist;
	local_vdso_pages = vdso32_pages;

	/* Be sure to map the data page */
	vdso_mapping_len = (local_vdso_pages + 1) << PAGE_SHIFT;

	vdso_base = get_unmapped_area(NULL, 0, vdso_mapping_len, 0, 0);
	if (IS_ERR_VALUE(vdso_base)) {
		ret = vdso_base;
		goto up_fail;
	}
	mm->context.vdso = (void *)vdso_base;

	ret = install_special_mapping(mm, vdso_base, vdso_mapping_len,
				      VM_READ|VM_EXEC|
				      VM_MAYREAD|VM_MAYWRITE|VM_MAYEXEC,
				      local_vdso_pagelist);
	if (ret) {
		mm->context.vdso = NULL;
		goto up_fail;
	}

up_fail:
	return ret;
}

/*
 * Update the vDSO data page to keep in sync with kernel timekeeping.
 */
void update_vsyscall(struct timekeeper *tk)
{
	struct timespec xtime_coarse;
	u32 use_syscall = strcmp(tk->tkr.clock->name, "arch_sys_counter");

	++vdso_data->tb_seq_count;
	smp_wmb();

	xtime_coarse = __current_kernel_time();
	vdso_data->use_syscall			= use_syscall;
	vdso_data->xtime_coarse_sec		= xtime_coarse.tv_sec;
	vdso_data->xtime_coarse_nsec		= xtime_coarse.tv_nsec;
	vdso_data->wtm_clock_sec		= tk->wall_to_monotonic.tv_sec;
	vdso_data->wtm_clock_nsec		= tk->wall_to_monotonic.tv_nsec;

	if (!use_syscall) {
		vdso_data->cs_cycle_last	= tk->tkr.cycle_last;
		vdso_data->xtime_clock_sec	= tk->xtime_sec;
		vdso_data->xtime_clock_nsec	= tk->tkr.xtime_nsec;
		vdso_data->cs_mult		= tk->tkr.mult;
		vdso_data->cs_shift		= tk->tkr.shift;
	}

	smp_wmb();
	++vdso_data->tb_seq_count;
}

void update_vsyscall_tz(void)
{
	vdso_data->tz_minuteswest	= sys_tz.tz_minuteswest;
	vdso_data->tz_dsttime		= sys_tz.tz_dsttime;
}
