/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Cisco Systems.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies. All rights reserved.
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
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/hugetlb.h>
#include <linux/dma-attrs.h>
#include <linux/slab.h>

#include "uverbs.h"

static struct ib_umem *peer_umem_get(struct ib_peer_memory_client *ib_peer_mem,
				     struct ib_umem *umem, unsigned long addr,
				     int dmasync, unsigned long peer_mem_flags)
{
	int ret;
	const struct peer_memory_client *peer_mem = ib_peer_mem->peer_mem;
	struct invalidation_ctx *invalidation_ctx = NULL;

	umem->ib_peer_mem = ib_peer_mem;
	if (peer_mem_flags & IB_PEER_MEM_INVAL_SUPP) {
		ret = ib_peer_create_invalidation_ctx(ib_peer_mem, umem, &invalidation_ctx);
		if (ret)
			goto end;
	}

	/*
	 * We always request write permissions to the pages, to force breaking of any CoW
	 * during the registration of the MR. For read-only MRs we use the "force" flag to
	 * indicate that CoW breaking is required but the registration should not fail if
	 * referencing read-only areas.
	 */
	ret = peer_mem->get_pages(addr, umem->length,
				  1, !umem->writable,
				  &umem->sg_head,
				  umem->peer_mem_client_context,
				  invalidation_ctx ?
				  invalidation_ctx->context_ticket : 0);
	if (ret)
		goto out;

	umem->page_size = peer_mem->get_page_size
					(umem->peer_mem_client_context);
	if (umem->page_size <= 0)
		goto put_pages;

	umem->offset = addr & ((unsigned long)umem->page_size - 1);
	ret = peer_mem->dma_map(&umem->sg_head,
				umem->peer_mem_client_context,
				umem->context->device->dma_device,
				dmasync,
				&umem->nmap);
	if (ret)
		goto put_pages;

	return umem;

put_pages:
	peer_mem->put_pages(umem->peer_mem_client_context,
					&umem->sg_head);
out:
	if (invalidation_ctx)
		ib_peer_destroy_invalidation_ctx(ib_peer_mem, invalidation_ctx);
end:
	ib_put_peer_client(ib_peer_mem, umem->peer_mem_client_context);
	kfree(umem);
	return ERR_PTR(ret);
}

static void peer_umem_release(struct ib_umem *umem)
{
	struct ib_peer_memory_client *ib_peer_mem = umem->ib_peer_mem;
	const struct peer_memory_client *peer_mem = ib_peer_mem->peer_mem;
	struct invalidation_ctx *invalidation_ctx = umem->invalidation_ctx;

	if (invalidation_ctx)
		ib_peer_destroy_invalidation_ctx(ib_peer_mem, invalidation_ctx);

	peer_mem->dma_unmap(&umem->sg_head,
			    umem->peer_mem_client_context,
			    umem->context->device->dma_device);
	peer_mem->put_pages(&umem->sg_head,
			    umem->peer_mem_client_context);
	ib_put_peer_client(ib_peer_mem, umem->peer_mem_client_context);
	kfree(umem);
}

static void __ib_umem_release(struct ib_device *dev, struct ib_umem *umem, int dirty)
{
	struct scatterlist *sg;
	struct page *page;
	int i;

	if (umem->nmap > 0)
		ib_dma_unmap_sg(dev, umem->sg_head.sgl,
				umem->nmap,
				DMA_BIDIRECTIONAL);

	for_each_sg(umem->sg_head.sgl, sg, umem->npages, i) {

		page = sg_page(sg);
		if (umem->writable && dirty)
			set_page_dirty_lock(page);
		put_page(page);
	}

	sg_free_table(&umem->sg_head);
	return;

}

static int handle_pfn_pages(struct mm_struct *mm, struct scatterlist *sg,
			    struct ib_umem *umem,
			    unsigned long start, unsigned long length)
{
	int i = 0;
	struct vm_area_struct *vma = NULL;
	unsigned long pfn;

	umem->nmap = (length + PAGE_SIZE - 1) / PAGE_SIZE;

	do {
		vma = find_vma(mm, start);
		if (!vma || !(vma->vm_flags & VM_PFNMAP))
			return -EFAULT;

		sg[i].page_link = 0;

		if (follow_pfn(vma, start, &pfn))
			return -EINVAL;

		sg[i].dma_address = pfn << PAGE_SHIFT;
		sg[i].length = min_t(unsigned, length, vma->vm_end - start);
		sg[i].dma_length = sg[i].length;
		sg[i].offset = 0;

		length -= sg[i].dma_length;
		start += sg[i].dma_length;
		i++;

	} while (length);

	return 0;
}

static int check_lock_limit(int npages)
{
	unsigned long locked     = npages + current->mm->pinned_vm;
	unsigned long lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	if ((locked > lock_limit) && !capable(CAP_IPC_LOCK))
		return ENOMEM;

	return locked;
}

int ib_umem_activate_invalidation_notifier(struct ib_umem *umem,
					   umem_invalidate_func_t func,
					   void *cookie)
{
	struct invalidation_ctx *invalidation_ctx = umem->invalidation_ctx;
	int ret = 0;

	mutex_lock(&umem->ib_peer_mem->lock);
	if (invalidation_ctx->peer_invalidated) {
		pr_err("ib_umem_activate_invalidation_notifier: pages were invalidated by peer\n");
		ret = -EINVAL;
		goto end;
	}
	invalidation_ctx->func = func;
	invalidation_ctx->cookie = cookie;
	/* from that point any pending invalidations can be called */
end:
	mutex_unlock(&umem->ib_peer_mem->lock);
	return ret;
}
EXPORT_SYMBOL(ib_umem_activate_invalidation_notifier);
/**
 * ib_umem_get - Pin and DMA map userspace memory.
 * @context: userspace context to pin memory for
 * @addr: userspace virtual address to start at
 * @size: length of region to pin
 * @access: IB_ACCESS_xxx flags for memory being pinned
 * @dmasync: flush in-flight DMA when the memory region is written
 * @peer_mem_flags: IB_PEER_MEM_xxx flags for memory being used
 */
struct ib_umem *ib_umem_get(struct ib_ucontext *context, unsigned long addr,
			       size_t size, int access, int dmasync,
			       unsigned long peer_mem_flags)
{
	struct ib_umem *umem;
	struct page **page_list;
	struct vm_area_struct **vma_list;
	unsigned long locked = 0;
	unsigned long cur_base;
	unsigned long npages;
	int ret;
	int i;
	DEFINE_DMA_ATTRS(attrs);
	struct scatterlist *sg, *sg_list_start;
	int need_release = 0;

	if (dmasync)
		dma_set_attr(DMA_ATTR_WRITE_BARRIER, &attrs);

	if (!can_do_mlock())
		return ERR_PTR(-EPERM);

	umem = kzalloc(sizeof *umem, GFP_KERNEL);
	if (!umem)
		return ERR_PTR(-ENOMEM);

	umem->context   = context;
	umem->length    = size;
	umem->offset    = addr & ~PAGE_MASK;
	umem->page_size = PAGE_SIZE;
	/*
	 * We ask for writable memory if any access flags other than
	 * "remote read" are set.  "Local write" and "remote write"
	 * obviously require write access.  "Remote atomic" can do
	 * things like fetch and add, which will modify memory, and
	 * "MW bind" can change permissions by binding a window.
	 */
	umem->writable  = !!(access & ~IB_ACCESS_REMOTE_READ);
	if (peer_mem_flags & IB_PEER_MEM_ALLOW) {
		struct ib_peer_memory_client *peer_mem_client;

		peer_mem_client =  ib_get_peer_client(context, addr, size, peer_mem_flags,
						      &umem->peer_mem_client_context);
		if (peer_mem_client)
			return peer_umem_get(peer_mem_client, umem, addr,
					dmasync, peer_mem_flags);
	}

	/* We assume the memory is from hugetlb until proved otherwise */
	umem->hugetlb   = 1;

	page_list = (struct page **) __get_free_page(GFP_KERNEL);
	if (!page_list) {
		kfree(umem);
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * if we can't alloc the vma_list, it's not so bad;
	 * just assume the memory is not hugetlb memory
	 */
	vma_list = (struct vm_area_struct **) __get_free_page(GFP_KERNEL);
	if (!vma_list)
		umem->hugetlb = 0;

	npages = PAGE_ALIGN(size + umem->offset) >> PAGE_SHIFT;

	down_write(&current->mm->mmap_sem);

	cur_base = addr & PAGE_MASK;

	if (npages == 0) {
		ret = -EINVAL;
		goto out;
	}

	ret = sg_alloc_table(&umem->sg_head, npages, GFP_KERNEL);
	if (ret)
		goto out;

	need_release = 1;
	sg_list_start = umem->sg_head.sgl;

	ret = handle_pfn_pages(current->mm, sg_list_start, umem,
			       cur_base, size);

	if (ret == 0 || ret != -EFAULT)
		goto out;

	locked = check_lock_limit(npages);
	if (locked < 0) {
		ret = locked;
		goto out;
	}

	while (npages) {
		ret = get_user_pages(current, current->mm, cur_base,
				     min_t(unsigned long, npages,
					   PAGE_SIZE / sizeof (struct page *)),
				     1, !umem->writable, page_list, vma_list);
		if (ret < 0)
			goto out;

		umem->npages += ret;
		cur_base += ret * PAGE_SIZE;
		npages   -= ret;

		for_each_sg(sg_list_start, sg, ret, i) {
			if (vma_list && !is_vm_hugetlb_page(vma_list[i]))
				umem->hugetlb = 0;

			sg_set_page(sg, page_list[i], PAGE_SIZE, 0);
		}

		/* preparing for next loop */
		sg_list_start = sg;
	}

	umem->nmap = ib_dma_map_sg_attrs(context->device,
				  umem->sg_head.sgl,
				  umem->npages,
				  DMA_BIDIRECTIONAL,
				  &attrs);

	if (umem->nmap <= 0) {
		ret = -ENOMEM;
		goto out;
	}

	ret = 0;

out:
	if (ret < 0) {
		if (need_release)
			__ib_umem_release(context->device, umem, 0);
		kfree(umem);
	} else
		current->mm->pinned_vm = locked;

	up_write(&current->mm->mmap_sem);
	if (vma_list)
		free_page((unsigned long) vma_list);
	free_page((unsigned long) page_list);

	return ret < 0 ? ERR_PTR(ret) : umem;
}
EXPORT_SYMBOL(ib_umem_get);

static void ib_umem_account(struct work_struct *work)
{
	struct ib_umem *umem = container_of(work, struct ib_umem, work);

	down_write(&umem->mm->mmap_sem);
	umem->mm->pinned_vm -= umem->diff;
	up_write(&umem->mm->mmap_sem);
	mmput(umem->mm);
	kfree(umem);
}

/**
 * ib_umem_release - release memory pinned with ib_umem_get
 * @umem: umem struct to release
 */
void ib_umem_release(struct ib_umem *umem)
{
	struct ib_ucontext *context = umem->context;
	struct mm_struct *mm;
	unsigned long diff;
	if (umem->ib_peer_mem) {
		peer_umem_release(umem);
		return;
	}

	__ib_umem_release(umem->context->device, umem, 1);

	mm = get_task_mm(current);
	if (!mm) {
		kfree(umem);
		return;
	}

	diff = PAGE_ALIGN(umem->length + umem->offset) >> PAGE_SHIFT;

	/*
	 * We may be called with the mm's mmap_sem already held.  This
	 * can happen when a userspace munmap() is the call that drops
	 * the last reference to our file and calls our release
	 * method.  If there are memory regions to destroy, we'll end
	 * up here and not be able to take the mmap_sem.  In that case
	 * we defer the vm_locked accounting to the system workqueue.
	 */
	if (context->closing) {
		if (!down_write_trylock(&mm->mmap_sem)) {
			INIT_WORK(&umem->work, ib_umem_account);
			umem->mm   = mm;
			umem->diff = diff;

			queue_work(ib_wq, &umem->work);
			return;
		}
	} else
		down_write(&mm->mmap_sem);

	current->mm->pinned_vm -= diff;
	up_write(&mm->mmap_sem);
	mmput(mm);
	kfree(umem);
}
EXPORT_SYMBOL(ib_umem_release);

int ib_umem_page_count(struct ib_umem *umem)
{
	int shift;
	int i;
	int n;
	struct scatterlist *sg;

	shift = ilog2(umem->page_size);

	n = 0;
	for_each_sg(umem->sg_head.sgl, sg, umem->nmap, i)
		n += sg_dma_len(sg) >> shift;

	return n;
}
EXPORT_SYMBOL(ib_umem_page_count);
