/*
 * drivers/staging/android/ion/ion_dummy_driver.c
 *
 * Copyright (C) 2013 Linaro, Inc
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/sizes.h>
#include "ion.h"
#include "ion_priv.h"

struct ion_device *idev;
struct ion_heap **heaps;

/*
 * The system heap alone, and deliberately.
 *
 * ION exists on this board for one client: Codec2 opens /dev/ion and needs
 * somewhere to put its buffers. It asks with heapMask ~0 -- any heap will
 * do -- and ion_alloc walks the list by descending id, so whatever sits
 * highest answers first and the system heap, id 0, answers last.
 *
 * With the sample driver's other three in place, that meant the DMA heap
 * took every allocation. It is declared with no size, so it comes out of
 * the generic coherent pool, which on Tegra is a few hundred kilobytes:
 *
 *     W CCodecBufferChannel: [c2.android.raw.decoder] start: cannot
 *                            allocate memory at all
 *
 * and with no buffers there was no decoded audio, no AudioTrack, and
 * silence -- while the system heap, which allocates pages and has the whole
 * of memory behind it, sat at zero bytes used.
 *
 * The carveout and chunk heaps were no better placed and reserved four
 * megabytes each on a two-gigabyte tablet to be equally unused. Nothing
 * here asks for a heap by id: the real allocator on this hardware is nvmap,
 * and ION is only the doorway Codec2 insists on.
 */
struct ion_platform_heap dummy_heaps[] = {
		{
			.id	= ION_HEAP_TYPE_SYSTEM,
			.type	= ION_HEAP_TYPE_SYSTEM,
			.name	= "system",
		},
};

struct ion_platform_data dummy_ion_pdata = {
	.nr = ARRAY_SIZE(dummy_heaps),
	.heaps = dummy_heaps,
};

static int __init ion_dummy_init(void)
{
	int i, err;

	idev = ion_device_create(NULL);
	heaps = kzalloc(sizeof(struct ion_heap *) * dummy_ion_pdata.nr,
			GFP_KERNEL);
	if (!heaps)
		return PTR_ERR(heaps);

	for (i = 0; i < dummy_ion_pdata.nr; i++) {
		struct ion_platform_heap *heap_data = &dummy_ion_pdata.heaps[i];

		heaps[i] = ion_heap_create(heap_data);
		if (IS_ERR_OR_NULL(heaps[i])) {
			err = PTR_ERR(heaps[i]);
			goto err;
		}
		ion_device_add_heap(idev, heaps[i]);
	}
	return 0;
err:
	for (i = 0; i < dummy_ion_pdata.nr; i++) {
		if (heaps[i])
			ion_heap_destroy(heaps[i]);
	}
	kfree(heaps);

	return err;
}

static void __exit ion_dummy_exit(void)
{
	int i;

	ion_device_destroy(idev);

	for (i = 0; i < dummy_ion_pdata.nr; i++)
		ion_heap_destroy(heaps[i]);
	kfree(heaps);
}

module_init(ion_dummy_init);
module_exit(ion_dummy_exit);

