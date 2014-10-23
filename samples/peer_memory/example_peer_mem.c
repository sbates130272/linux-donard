/*
 * Copyright (c) 2014, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <rdma/peer_mem.h>

#define DRV_NAME	"example_peer_mem"
#define DRV_VERSION	"1.0"
#define DRV_RELDATE	__DATE__

MODULE_AUTHOR("Yishai Hadas");
MODULE_DESCRIPTION("Example peer memory");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRV_VERSION);
static unsigned long example_mem_start_range;
static unsigned long example_mem_end_range;

module_param(example_mem_start_range, ulong, 0444);
MODULE_PARM_DESC(example_mem_start_range, "peer example start memory range");
module_param(example_mem_end_range, ulong, 0444);
MODULE_PARM_DESC(example_mem_end_range, "peer example end memory range");

static void *reg_handle;

struct example_mem_context {
	u64 core_context;
	u64 page_virt_start;
	u64 page_virt_end;
	size_t mapped_size;
	unsigned long npages;
	int	      nmap;
	unsigned long page_size;
	int	      writable;
	int dirty;
};

static void example_mem_put_pages(struct sg_table *sg_head, void *context);

/* acquire return code: 1 mine, 0 - not mine */
static int example_mem_acquire(unsigned long addr, size_t size, void *peer_mem_private_data,
			       char *peer_mem_name, void **client_context)
{
	struct example_mem_context *example_mem_context;

	if (!(addr >= example_mem_start_range) ||
	    !(addr + size < example_mem_end_range))
		/* peer is not the owner */
		return 0;

	example_mem_context = kzalloc(sizeof(*example_mem_context), GFP_KERNEL);
	if (!example_mem_context)
		/* Error case handled as not mine */
		return 0;

	example_mem_context->page_virt_start = addr & PAGE_MASK;
	example_mem_context->page_virt_end   = (addr + size + PAGE_SIZE - 1) & PAGE_MASK;
	example_mem_context->mapped_size  = example_mem_context->page_virt_end - example_mem_context->page_virt_start;

	/* 1 means mine */
	*client_context = example_mem_context;
	__module_get(THIS_MODULE);
	return 1;
}

static int example_mem_get_pages(unsigned long addr, size_t size, int write, int force,
				 struct sg_table *sg_head, void *client_context, u64 core_context)
{
	int ret;
	unsigned long npages;
	unsigned long cur_base;
	struct page **page_list;
	struct scatterlist *sg, *sg_list_start;
	int i;
	struct example_mem_context *example_mem_context;

	example_mem_context = (struct example_mem_context *)client_context;
	example_mem_context->core_context = core_context;
	example_mem_context->page_size = PAGE_SIZE;
	example_mem_context->writable = write;
	npages = example_mem_context->mapped_size >> PAGE_SHIFT;

	if (npages == 0)
		return -EINVAL;

	ret = sg_alloc_table(sg_head, npages, GFP_KERNEL);
	if (ret)
		return ret;

	page_list = (struct page **)__get_free_page(GFP_KERNEL);
	if (!page_list) {
		ret = -ENOMEM;
		goto out;
	}

	sg_list_start = sg_head->sgl;
	cur_base = addr & PAGE_MASK;

	while (npages) {
		ret = get_user_pages(current, current->mm, cur_base,
				     min_t(unsigned long, npages, PAGE_SIZE / sizeof(struct page *)),
				     write, force, page_list, NULL);

		if (ret < 0)
			goto out;

		example_mem_context->npages += ret;
		cur_base += ret * PAGE_SIZE;
		npages   -= ret;

		for_each_sg(sg_list_start, sg, ret, i)
				sg_set_page(sg, page_list[i], PAGE_SIZE, 0);

		/* preparing for next loop */
		sg_list_start = sg;
	}

out:
	if (page_list)
		free_page((unsigned long)page_list);

	if (ret < 0) {
		example_mem_put_pages(sg_head, client_context);
		return ret;
	}
	/* mark that pages were exposed from the peer memory */
	example_mem_context->dirty = 1;
	return 0;
}

static int example_mem_dma_map(struct sg_table *sg_head, void *context,
			       struct device *dma_device, int dmasync,
			       int *nmap)
{
	DEFINE_DMA_ATTRS(attrs);
	struct example_mem_context *example_mem_context =
		(struct example_mem_context *)context;

	if (dmasync)
		dma_set_attr(DMA_ATTR_WRITE_BARRIER, &attrs);
	 example_mem_context->nmap = dma_map_sg_attrs(dma_device, sg_head->sgl,
						      example_mem_context->npages,
						      DMA_BIDIRECTIONAL, &attrs);
	if (example_mem_context->nmap <= 0)
		return -ENOMEM;

	*nmap = example_mem_context->nmap;
	return 0;
}

static int example_mem_dma_unmap(struct sg_table *sg_head, void *context,
				 struct device  *dma_device)
{
	struct example_mem_context *example_mem_context =
		(struct example_mem_context *)context;

	dma_unmap_sg(dma_device, sg_head->sgl,
		     example_mem_context->nmap,
		     DMA_BIDIRECTIONAL);
	return 0;
}

static void example_mem_put_pages(struct sg_table *sg_head, void *context)
{
	struct scatterlist *sg;
	struct page *page;
	int i;

	struct example_mem_context *example_mem_context =
		(struct example_mem_context *)context;

	for_each_sg(sg_head->sgl, sg, example_mem_context->npages, i) {
		page = sg_page(sg);
		if (example_mem_context->writable && example_mem_context->dirty)
			set_page_dirty_lock(page);
		put_page(page);
	}

	sg_free_table(sg_head);
}

static void example_mem_release(void *context)
{
	struct example_mem_context *example_mem_context =
		(struct example_mem_context *)context;

	kfree(example_mem_context);
	module_put(THIS_MODULE);
}

static unsigned long example_mem_get_page_size(void *context)
{
	struct example_mem_context *example_mem_context =
				(struct example_mem_context *)context;

	return example_mem_context->page_size;
}

static const struct peer_memory_client example_mem_client = {
	.name			= DRV_NAME,
	.version		= DRV_VERSION,
	.acquire		= example_mem_acquire,
	.get_pages	= example_mem_get_pages,
	.dma_map	= example_mem_dma_map,
	.dma_unmap	= example_mem_dma_unmap,
	.put_pages	= example_mem_put_pages,
	.get_page_size	= example_mem_get_page_size,
	.release		= example_mem_release,
};

static int __init example_mem_client_init(void)
{
	reg_handle = ib_register_peer_memory_client(&example_mem_client, NULL);
	if (!reg_handle)
		return -EINVAL;

	return 0;
}

static void __exit example_mem_client_cleanup(void)
{
	ib_unregister_peer_memory_client(reg_handle);
}

module_init(example_mem_client_init);
module_exit(example_mem_client_cleanup);
