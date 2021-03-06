
#pragma once

#include <base/pci.h>
#include <base/tcache.h>
#include <base/thread.h>
//#include <iokernel/queue.h>

//#include "../defs.h"


#define RQ_NUM_DESC			1024
#define SQ_NUM_DESC			128

#define SQ_CLEAN_THRESH			1
#define SQ_CLEAN_MAX			SQ_CLEAN_THRESH

struct trans_ops {
	/* receive an ingress packet */
	void (*recv) (struct trans_entry *e, struct mbuf *m);
	/* propagate a network error */
	void (*err) (struct trans_entry *e, int err);
};

struct trans_entry {
	int	                    match;
	uint8_t			        proto;
	struct netaddr		    laddr;
	struct netaddr		    raddr;
	struct rcu_hlist_node	link;
	struct rcu_head		    rcu;
	const struct trans_ops	*ops;
};

/* space for the mbuf struct */
#define RX_BUF_HEAD \
 (align_up(sizeof(struct mbuf), 2 * CACHE_LINE_SIZE))
/* some NICs expect enough padding for CRC etc., even if they strip it */
#define RX_BUF_TAIL			64

static inline unsigned int directpath_get_buf_size(void)
{
	return align_up(net_get_mtu() + RX_BUF_HEAD + RX_BUF_TAIL,
			2 * CACHE_LINE_SIZE);
}

extern struct pci_addr nic_pci_addr;
extern bool cfg_pci_addr_specified;

extern struct mempool directpath_buf_mp;
extern struct tcache *directpath_buf_tcache;
extern DEFINE_PERTHREAD(struct tcache_perthread, directpath_buf_pt);
extern void directpath_rx_completion(struct mbuf *m);
extern int mlx5_init(struct hardware_q **rxq_out,
	    struct direct_txq **txq_out, unsigned int nr_rxq,
	    unsigned int nr_txq);

struct ibv_device;
extern int ibv_device_to_pci_addr(const struct ibv_device *device, struct pci_addr *pci_addr);
