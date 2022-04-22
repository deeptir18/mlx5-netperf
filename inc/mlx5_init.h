#pragma once
#include <base/mempool.h>
#include <base/mbuf.h>
#include <base/page.h>
#include <base/pci.h>
#include <base/stddef.h>
#include <base/atomic.h>
#include <util/udma_barrier.h>
#include <util/mmio.h>
#include <mlx5.h>
#include <mlx5_ifc.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
/**********************************************************************/
// CONSTANTS
/**********************************************************************/

#define PORT_NUM 1 // TODO: make this dynamic
#define NUM_QUEUES 1
#define RQ_NUM_DESC			1024
#define SQ_NUM_DESC			128
#define RUNTIME_RX_BATCH_SIZE		32
#define SQ_CLEAN_THRESH			RUNTIME_RX_BATCH_SIZE
#define SQ_CLEAN_MAX			SQ_CLEAN_THRESH
#define DEFAULT_CHAR 'c'
/* space for the mbuf struct */
#define RX_BUF_HEAD \
 (align_up(sizeof(struct mbuf), 2 * CACHE_LINE_SIZE))
/* some NICs expect enough padding for CRC etc.*/
#define RX_BUF_TAIL			64
#define NET_MTU 9216 // jumbo frames is turned on in this interface

// for TX on the client size
// for RX on the server size
#define REQ_MBUFS_SIZE 16384
#define REQ_MBUFS_PER_PAGE 256
#define REQ_MBUFS_PAGES 100

#define DATA_MBUFS_SIZE 16384
#define DATA_MBUFS_PER_PAGE 256
#define DATA_MBUFS_PAGES 40

// for zero-copy on the server, still need to have "control" mbufs to store
// pointers
#define CONTROL_MBUFS_SIZE (align_up(sizeof(struct mbuf), 2 * CACHE_LINE_SIZE))
#define CONTROL_MBUFS_PER_PAGE 4096
#define CONTROL_MBUFS_PAGES 10

/* Initialize the server memory */
int server_memory_init(void **addr, size_t region_len);

/* Initialize memory in a mempool and initialize the mempool*/
int mempool_memory_init(struct mempool *mempool,
                        size_t mbuf_size,
                        size_t mbufs_per_page, 
                        size_t num_pages);

/* Get the ibv device info for the NIC */
int ibv_device_to_pci_addr(const struct ibv_device *device,
                           struct pci_addr *pci_addr);

/* Init the ibv context */
int init_ibv_context(struct ibv_context **ibv_context,
                        struct ibv_pd **pd, 
                        struct pci_addr *nic_pci_addr);

/* Do the memory registration */
int memory_registration(struct ibv_pd *pd,
                        struct ibv_mr **mr, 
                        void *buf, 
                        size_t len, 
                        int flags);
int memory_deregistration(struct ibv_mr *mr);

/* Initialize a single rxq */
int mlx5_init_rxq(struct mlx5_rxq *v,
                    struct mempool *rx_mempool,
                    struct ibv_context *ibv_context,
                    struct ibv_pd *ibv_pd,
                    struct ibv_mr *mr);

/* Initialize queue steering */
int mlx5_qs_init_flows(struct mlx5_rxq *v, 
                        struct ibv_pd *ibv_pd,
                        struct ibv_context *ibv_context,
                        struct eth_addr *my_eth, 
                        struct eth_addr *other_eth);

/* Initialize txq */
int mlx5_init_txq(struct mlx5_txq *v, 
                    struct ibv_pd *ibv_pd,
                    struct ibv_context *ibv_context,
                    struct ibv_mr *mr_tx,
                    size_t max_inline_data,
                    int init_each_tx_segment);
