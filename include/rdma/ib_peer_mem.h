#if !defined(IB_PEER_MEM_H)
#define IB_PEER_MEM_H

#include <rdma/peer_mem.h>

struct ib_ucontext;
struct ib_umem;
struct invalidation_ctx;

struct ib_peer_memory_client {
	const struct peer_memory_client *peer_mem;
	struct list_head	core_peer_list;
	int invalidation_required;
	struct kref ref;
	struct completion unload_comp;
	/* lock is used via the invalidation flow */
	struct mutex lock;
	struct list_head   core_ticket_list;
	u64	last_ticket;
};

enum ib_peer_mem_flags {
	IB_PEER_MEM_ALLOW	= 1,
};

struct core_ticket {
	unsigned long key;
	void *context;
	struct list_head   ticket_list;
};

struct ib_peer_memory_client *ib_get_peer_client(struct ib_ucontext *context, unsigned long addr,
						 size_t size, void **peer_client_context);

void ib_put_peer_client(struct ib_peer_memory_client *ib_peer_client,
			void *peer_client_context);

int ib_peer_create_invalidation_ctx(struct ib_peer_memory_client *ib_peer_mem, struct ib_umem *umem,
				    struct invalidation_ctx **invalidation_ctx);

void ib_peer_destroy_invalidation_ctx(struct ib_peer_memory_client *ib_peer_mem,
				      struct invalidation_ctx *invalidation_ctx);

#endif
