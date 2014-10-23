/*
 * Copyright (c) 2014,  Mellanox Technologies. All rights reserved.
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

#include <rdma/ib_peer_mem.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_umem.h>

static DEFINE_MUTEX(peer_memory_mutex);
static LIST_HEAD(peer_memory_list);

static int ib_invalidate_peer_memory(void *reg_handle, u64 core_context)
{
	return -ENOSYS;
}

static int ib_peer_insert_context(struct ib_peer_memory_client *ib_peer_client,
				  void *context,
				  u64 *context_ticket)
{
	struct core_ticket *core_ticket = kzalloc(sizeof(*core_ticket), GFP_KERNEL);

	if (!core_ticket)
		return -ENOMEM;

	mutex_lock(&ib_peer_client->lock);
	core_ticket->key = ib_peer_client->last_ticket++;
	core_ticket->context = context;
	list_add_tail(&core_ticket->ticket_list,
		      &ib_peer_client->core_ticket_list);
	*context_ticket = core_ticket->key;
	mutex_unlock(&ib_peer_client->lock);

	return 0;
}

/* Caller should be holding the peer client lock, specifically, the caller should hold ib_peer_client->lock */
static int ib_peer_remove_context(struct ib_peer_memory_client *ib_peer_client,
				  u64 key)
{
	struct core_ticket *core_ticket;

	list_for_each_entry(core_ticket, &ib_peer_client->core_ticket_list,
			    ticket_list) {
		if (core_ticket->key == key) {
			list_del(&core_ticket->ticket_list);
			kfree(core_ticket);
			return 0;
		}
	}

	return 1;
}

/**
** ib_peer_create_invalidation_ctx - creates invalidation context for a given umem
** @ib_peer_mem: peer client to be used
** @umem: umem struct belongs to that context
** @invalidation_ctx: output context
**/
int ib_peer_create_invalidation_ctx(struct ib_peer_memory_client *ib_peer_mem, struct ib_umem *umem,
				    struct invalidation_ctx **invalidation_ctx)
{
	int ret;
	struct invalidation_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ret = ib_peer_insert_context(ib_peer_mem, ctx,
				     &ctx->context_ticket);
	if (ret) {
		kfree(ctx);
		return ret;
	}

	ctx->umem = umem;
	umem->invalidation_ctx = ctx;
	*invalidation_ctx = ctx;
	return 0;
}

/**
 * ** ib_peer_destroy_invalidation_ctx - destroy a given invalidation context
 * ** @ib_peer_mem: peer client to be used
 * ** @invalidation_ctx: context to be invalidated
 * **/
void ib_peer_destroy_invalidation_ctx(struct ib_peer_memory_client *ib_peer_mem,
				      struct invalidation_ctx *invalidation_ctx)
{
	mutex_lock(&ib_peer_mem->lock);
	ib_peer_remove_context(ib_peer_mem, invalidation_ctx->context_ticket);
	mutex_unlock(&ib_peer_mem->lock);

	kfree(invalidation_ctx);
}
static int ib_memory_peer_check_mandatory(const struct peer_memory_client
						     *peer_client)
{
#define PEER_MEM_MANDATORY_FUNC(x) { offsetof(struct peer_memory_client, x), #x }
		static const struct {
			size_t offset;
			char  *name;
		} mandatory_table[] = {
			PEER_MEM_MANDATORY_FUNC(acquire),
			PEER_MEM_MANDATORY_FUNC(get_pages),
			PEER_MEM_MANDATORY_FUNC(put_pages),
			PEER_MEM_MANDATORY_FUNC(get_page_size),
			PEER_MEM_MANDATORY_FUNC(dma_map),
			PEER_MEM_MANDATORY_FUNC(dma_unmap)
		};
		int i;

		for (i = 0; i < ARRAY_SIZE(mandatory_table); ++i) {
			if (!*(void **)((void *)peer_client + mandatory_table[i].offset)) {
				pr_err("Peer memory %s is missing mandatory function %s\n",
				       peer_client->name, mandatory_table[i].name);
				return -EINVAL;
			}
		}

		return 0;
}

static void complete_peer(struct kref *kref)
{
	struct ib_peer_memory_client *ib_peer_client =
		container_of(kref, struct ib_peer_memory_client, ref);

	complete(&ib_peer_client->unload_comp);
}

void *ib_register_peer_memory_client(const struct peer_memory_client *peer_client,
				     invalidate_peer_memory *invalidate_callback)
{
	struct ib_peer_memory_client *ib_peer_client;

	if (ib_memory_peer_check_mandatory(peer_client))
		return NULL;

	ib_peer_client = kzalloc(sizeof(*ib_peer_client), GFP_KERNEL);
	if (!ib_peer_client)
		return NULL;

	INIT_LIST_HEAD(&ib_peer_client->core_ticket_list);
	mutex_init(&ib_peer_client->lock);
	init_completion(&ib_peer_client->unload_comp);
	kref_init(&ib_peer_client->ref);
	ib_peer_client->peer_mem = peer_client;
	ib_peer_client->last_ticket = 1;
	/* Once peer supplied a non NULL callback it's an indication that invalidation support is
	 * required for any memory owning.
	 */
	if (invalidate_callback) {
		*invalidate_callback = ib_invalidate_peer_memory;
		ib_peer_client->invalidation_required = 1;
	}

	mutex_lock(&peer_memory_mutex);
	list_add_tail(&ib_peer_client->core_peer_list, &peer_memory_list);
	mutex_unlock(&peer_memory_mutex);

	return ib_peer_client;
}
EXPORT_SYMBOL(ib_register_peer_memory_client);

void ib_unregister_peer_memory_client(void *reg_handle)
{
	struct ib_peer_memory_client *ib_peer_client = reg_handle;

	mutex_lock(&peer_memory_mutex);
	list_del(&ib_peer_client->core_peer_list);
	mutex_unlock(&peer_memory_mutex);

	kref_put(&ib_peer_client->ref, complete_peer);
	wait_for_completion(&ib_peer_client->unload_comp);
	kfree(ib_peer_client);
}
EXPORT_SYMBOL(ib_unregister_peer_memory_client);

struct ib_peer_memory_client *ib_get_peer_client(struct ib_ucontext *context, unsigned long addr,
						 size_t size, void **peer_client_context)
{
	struct ib_peer_memory_client *ib_peer_client;
	int ret;

	mutex_lock(&peer_memory_mutex);
	list_for_each_entry(ib_peer_client, &peer_memory_list, core_peer_list) {
		ret = ib_peer_client->peer_mem->acquire(addr, size,
						   context->peer_mem_private_data,
						   context->peer_mem_name,
						   peer_client_context);
		if (ret > 0)
			goto found;
	}

	ib_peer_client = NULL;

found:
	if (ib_peer_client)
		kref_get(&ib_peer_client->ref);

	mutex_unlock(&peer_memory_mutex);
	return ib_peer_client;
}
EXPORT_SYMBOL(ib_get_peer_client);

void ib_put_peer_client(struct ib_peer_memory_client *ib_peer_client,
			void *peer_client_context)
{
	if (ib_peer_client->peer_mem->release)
		ib_peer_client->peer_mem->release(peer_client_context);

	kref_put(&ib_peer_client->ref, complete_peer);
}
EXPORT_SYMBOL(ib_put_peer_client);
