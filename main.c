#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <net/ethernet.h>
#include <net/ip.h>
#include <net/udp.h>
#include <netinet/in.h>
#include <sys/mman.h>

#include <asm/ops.h>
#include <base/busy_work.h>
#include <base/debug.h>
#include <base/time.h>
#include <base/parse.h>
#include <base/mem.h>
#include <base/request.h>
#include <base/latency.h>
#include <base/compiler.h>
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
#include <mlx5_init.h>
#include <mlx5_rxtx.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <base/latency.h>

/**********************************************************************/
// CONSTANTS
/**********************************************************************/
#define FULL_PROTO_HEADER 42
#define PKT_ID_SIZE 0
#define FULL_HEADER_SIZE (FULL_PROTO_HEADER + PKT_ID_SIZE)

/**********************************************************************/
// STATIC STATE
static uint8_t num_cores = 4;

static uint64_t checksum = 0;
static int read_incoming_packet = 0;
static double busy_work_res;
static uint8_t mode;
static struct eth_addr server_mac;
static struct eth_addr client_mac;
static uint32_t server_ip;
static uint32_t client_ip;
static size_t num_segments = 1;
static size_t segment_size = 1024;
static size_t working_set_size = 16384;
static size_t busy_work_us = 0;
static size_t busy_iters = 0;
static int zero_copy = 1;
static int has_latency_log = 0;
static char *latency_log;

static uint64_t rate_pps;
static uint64_t total_time;

struct ibv_context *context;
struct ibv_pd *pd;
struct pci_addr nic_pci_addr;

// per-thread state
typedef struct CoreState {
  int done;
  
  uint32_t idx;
  uint32_t server_port;
  uint32_t client_port;
  
  RateDistribution rate_distribution;
  ClientRequest *client_requests;
  OutgoingHeader header;
  Latency_Dist_t latency_dist;
  Packet_Map_t packet_map;
  size_t inline_lengths[MAX_PACKETS];
  RequestHeader *request_headers[MAX_PACKETS];

#ifdef __TIMERS__
  Latency_Dist_t server_request_dist;
  Latency_Dist_t client_lateness_dist;
  Latency_Dist_t server_send_dist;
  Latency_Dist_t server_construction_dist;
  Latency_Dist_t busy_work_dist;
#endif

  void *server_working_set;
  struct mlx5_rxq rxqs[NUM_QUEUES]; // NUM_QUEUES = 1 by default (one queue per core)
  struct mlx5_txq txqs[NUM_QUEUES];
  struct ibv_mr *tx_mr;
  struct ibv_mr *rx_mr;

  struct mempool rx_buf_mempool;
  struct mempool tx_buf_mempool;
  struct mempool mbuf_mempool;
  uint32_t total_dropped;
} CoreState;

size_t max_inline_data = 256;

CoreState* per_core_state;

#define cpu_to_be32(x)	(__bswap32(x))
#define hton32(x) (cpu_to_be32(x))

/**
 * compute_flow_affinity - compute rss hash for incoming packets
 * @local_port: the local port number
 * @remote_port: the remote port
 * @local_ip: local ip (in host-order)
 * @remote_ip: remote ip (in host-order)
 * @num_queues: total number of queues
 *
 * Returns the 32 bit hash mod num_queues
 *
 * copied from dpdk/lib/librte_hash/rte_thash.h
 */
static uint32_t compute_flow_affinity(uint32_t local_ip,
				      uint32_t remote_ip,
				      uint16_t local_port,
				      uint16_t remote_port,
				      size_t num_queues)
{
    static __thread int thread_id;
    const uint8_t *rss_key = (uint8_t *)sym_rss_key;

    uint32_t i, j, map, ret = 0, input_tuple[] = {
      remote_ip, local_ip, local_port | remote_port << 16
    };

    /* From caladan - implementation of rte_softrss */
    for (j = 0; j < ARRAY_SIZE(input_tuple); j++) {
      for (map = input_tuple[j]; map;	map &= (map - 1)) {
	i = (uint32_t)__builtin_ctz(map);
	ret ^= hton32(((const uint32_t *)rss_key)[j]) << (31 - i) |
	  (uint32_t)((uint64_t)(hton32(((const uint32_t *)rss_key)[j + 1])) >>
		     (i + 1));
      }
    }
    
    return ret % (uint32_t)num_queues;
}

void find_ip_and_pair(uint16_t queue_id,
		      uint32_t remote_ip,
		      uint16_t remote_port,
		      uint32_t start_ip,
		      uint16_t start_port,
		      uint16_t *port) {
    while(true) {
      uint32_t queue =
	compute_flow_affinity(start_ip, 
			      remote_ip,
			      start_port,
			      remote_port,
			      num_cores);
      if (queue == queue_id) {
	break;
      }
      
      start_port += 1;
    }
    *port = start_port;
}

void init_state(CoreState* state, uint32_t idx) {
  state->done = 0;
  state->idx = idx;
  state->server_port = 50000;
  state->client_port = 50000;
  find_ip_and_pair(idx, server_ip, state->server_port,
		   client_ip, state->client_port,
		   &(state->client_port));

  state->rate_distribution = (struct RateDistribution)
    {.type = UNIFORM, .rate_pps = rate_pps, .total_time = total_time};
  state->client_requests = NULL;
  state->header = (struct OutgoingHeader) {};
  state->latency_dist = (struct Latency_Dist_t)
    { .min = LONG_MAX, .max = 0, .latency_sum = 0, .total_count = 0 };
  state->packet_map = (struct Packet_Map_t)
    {.total_count = 0, .grouped_rtts = NULL, .sent_ids = NULL };

#ifdef __TIMERS__
  state->server_request_dist = (struct Latency_Dist_t)
    { .min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0 };
  state->client_lateness_dist = (struct Latency_Dist_t)
    {.min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0 };
  state->server_send_dist = (struct Latency_Dist_t)
    {.min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0 };
  state->server_construction_dist = (struct Latency_Dist_t)
    {.min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0};
  state->busy_work_dist = {.min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0};
#endif

  state->rx_buf_mempool = (struct mempool) {};
  state->tx_buf_mempool = (struct mempool) {};
  state->mbuf_mempool = (struct mempool) {};
  state->total_dropped = 0;
}

int init_all_states(CoreState* states) {
  for ( int i = 0; i < num_cores; i++ ) {
    printf("Initializing state %d\n", i);
    init_state(&states[i], i);
  }

  return 0;
}

/**********************************************************************/
static int parse_args(int argc, char *argv[]) {
    // have mode and pci address
    int opt = 0;
    long tmp;

    static struct option long_options[] = {
        {"mode",      required_argument,       0,  'm' },
        {"pci_addr",  required_argument,       0, 'w'},
        {"client_mac", required_argument, 0, 'c'},
        {"server_mac", required_argument, 0, 'e'},
        {"client_ip", required_argument, 0, 'i'},
        {"server_ip", required_argument, 0, 's'},
        {"num_segments", optional_argument, 0, 'k'},
        {"segment_size", optional_argument, 0, 'q'},
        {"rate", optional_argument, 0, 'r'},
        {"time", optional_argument, 0, 't'},
        {"array_size", optional_argument, 0, 'a'},
        {"busy_work_us", optional_argument, 0, 'y'},
        {"latency_log", optional_argument, 0, 'l'},
        {"with_copy", no_argument, 0, 'z'},
        {"read_incoming_packet", no_argument, 0, 'd'},
        {"num_cores", optional_argument, 0, 'n'},
        {0,           0,                 0,  0   }
    };
    int long_index = 0;
    int ret;
    while ((opt = getopt_long(argc, argv, "m:w:c:e:i:s:k:q:a:z:r:t:l:d:n:",
                              long_options, &long_index )) != -1) {
        switch (opt) {
	case 'm':
                if (!strcmp(optarg, "CLIENT")) {
                    mode = UDP_CLIENT;
                } else if (!strcmp(optarg, "SERVER")) {
                    mode = UDP_SERVER;
                } else {
                    NETPERF_ERROR("Passed in invalid mode: %s", optarg);
                    return -EINVAL;
                }
                break;
            case 'w':
	      ret = pci_str_to_addr(optarg, &nic_pci_addr);
	      if (ret) {
		NETPERF_ERROR("Could not parse pci addr: %s", optarg);
		return -EINVAL;
	      }
	      break;
            case 'c':
                if (str_to_mac(optarg, &client_mac) != 0) {
                   NETPERF_ERROR("failed to convert %s to a mac address", optarg);
                   return -EINVAL;
                }
                NETPERF_INFO("Parsed client eth addr: %s", optarg);
                break;
            case 'e':
                if (str_to_mac(optarg, &server_mac) != 0) {
                   NETPERF_ERROR("failed to convert %s to a mac address", optarg);
                   return -EINVAL;
                }
                NETPERF_INFO("Parsed server eth addr: %s", optarg);
                break;
            case 'i':
                if (str_to_ip(optarg, &client_ip) != 0) {
                    NETPERF_ERROR("Failed to parse %s as an IP addr", optarg);
                    return -EINVAL;
                }
                break;
            case 's':
                if (str_to_ip(optarg, &server_ip) != 0) {
                    NETPERF_ERROR("Failed to parse %s as an IP addr", optarg);
                    return -EINVAL;
                }
                break;
            case 'k': // num_segments
                str_to_long(optarg, &tmp);
                num_segments = tmp;
                break;
            case 'n': // num_cores
                str_to_long(optarg, &tmp);
                num_cores = tmp;
                break;
            case 'q': // segment_size
                str_to_long(optarg, &tmp);
                segment_size = tmp;
                break;
            case 'a': // array_size
                str_to_long(optarg, &tmp);
                working_set_size = tmp;
                break;
            case 'y': // amount of cpu cycles to do math in us
                str_to_long(optarg, &tmp);
                busy_work_us = tmp;
                busy_iters = calibrate_busy_work(busy_work_us);
                break;
            case 'z': // with_copy
	        NETPERF_DEBUG("Setting zero copy 0");
                zero_copy = 0;
                break;
            case 'r': // rate
                str_to_long(optarg, &tmp);
		rate_pps = tmp;
                break;
            case 't': // total_time
                str_to_long(optarg, &tmp);
		total_time = tmp;
		break;
            case 'l':
                has_latency_log = 1;
                latency_log = (char *)malloc(strlen(optarg));
                strcpy(latency_log, optarg);
                break;
            case 'd':
                read_incoming_packet = 1;
                break;
            default:
                NETPERF_WARN("Invalid arguments");
                exit(EXIT_FAILURE);
        }
    }
    return 0;
}

/**
 * Initializes data for the actual workload.
 * On the client side: initializes the client requests and the outgoing header.
 * On the server side: fills in payload on the server side.
 */
int init_workload(CoreState* state) {
    int ret = 0;

    if (mode == UDP_CLIENT) {
        // 1 uint64_t per segment
        size_t client_payload_size = sizeof(uint64_t) * ( SEGLIST_OFFSET + num_segments );
        ret = initialize_outgoing_header(&(state->header),
					 &client_mac,
					 &server_mac,
					 client_ip,
					 server_ip,
					 state->client_port,
					 state->server_port,
					 client_payload_size);
        RETURN_ON_ERR(ret, "Failed to initialize outgoing header");
        ret = initialize_client_requests(&(state->client_requests),
					 &(state->rate_distribution),
                                            segment_size,
                                            num_segments,
                                            working_set_size);
        RETURN_ON_ERR(ret, "Failed to initialize client requests: %s", strerror(-errno));
    } else {
        // num_segments * segment_size
        ret = initialize_server_memory(state->server_working_set,
				       segment_size,
				       working_set_size);
        RETURN_ON_ERR(ret, "Failed to fill in server memory: %s", strerror(ret));
        // TODO: for now, initialize inline size to be size of request header
        for (size_t i = 0; i < MAX_PACKETS; i++) {
	  state->inline_lengths[i] = sizeof(RequestHeader);
        }

    }

    return 0;
}

int cleanup_mlx5(CoreState* state) {
    int ret = 0;
    // mbuf mempool
    NETPERF_DEBUG("cleanup 1");
    ret = munmap((state->mbuf_mempool).buf, (state->mbuf_mempool).len);
    RETURN_ON_ERR(ret, "Failed to unmap mbuf mempool: %s", strerror(errno));
    
    if (mode == UDP_CLIENT) {
        ret = memory_deregistration(state->tx_mr);
        RETURN_ON_ERR(ret, "Failed to dereg tx_mr: %s", strerror(errno));
        ret = munmap((state->tx_buf_mempool).buf, (state->tx_buf_mempool).len);
        RETURN_ON_ERR(ret, "Failed to munmap tx mempool for client: %s", strerror(errno));

        ret = memory_deregistration(state->rx_mr);
        RETURN_ON_ERR(ret, "Failed to dereg rx_mr: %s", strerror(errno));
        ret = munmap((state->rx_buf_mempool).buf, (state->rx_buf_mempool).len);
        RETURN_ON_ERR(ret, "Failed to munmap rx mempool for client: %s", strerror(errno));
    } else {
    NETPERF_DEBUG("cleanup 2");
    //ret = memory_deregistration(state->rx_mr);
    //  RETURN_ON_ERR(ret, "Failed to dereg rx_mr: %s", strerror(errno));
    NETPERF_DEBUG("cleanup 3");
        ret = munmap((state->rx_buf_mempool).buf, (state->rx_buf_mempool).len);
        RETURN_ON_ERR(ret, "Failed to munmap rx mempool for server: %s", strerror(errno));
            
        // for both the zero-copy and non-zero-copy, un register region for tx
    NETPERF_DEBUG("cleanup 4");
    //  ret = memory_deregistration(state->tx_mr);
    //  RETURN_ON_ERR(ret, "Failed to dereg tx_mr: %s", strerror(errno));
        if (!zero_copy) {
    NETPERF_DEBUG("cleanup 5");
	  ret = munmap((state->tx_buf_mempool).buf, (state->tx_buf_mempool).len);
	  RETURN_ON_ERR(ret, "Failed to munmap tx mempool for server: %s", strerror(errno));
        }
        // free the server memory
    NETPERF_DEBUG("cleanup 6");
        ret = munmap(state->server_working_set, align_up(working_set_size, PGSIZE_2MB));
        RETURN_ON_ERR(ret, "Failed to unmap server working set memory");
    }
    NETPERF_DEBUG("cleanup 7 - done");
    return 0;
}

int init_mlx5(CoreState* state) {
    int ret = 0;

    NETPERF_DEBUG("init 1");
    // Alloc memory pool for TX mbuf structs
    ret = mempool_memory_init(&(state->mbuf_mempool),
			      CONTROL_MBUFS_SIZE,
			      CONTROL_MBUFS_PER_PAGE,
			      REQ_MBUFS_PAGES);
    RETURN_ON_ERR(ret, "Failed to init mbuf mempool: %s", strerror(errno));
    NETPERF_DEBUG("init 2");

	NETPERF_DEBUG("rx args: %p %p %ld, tx args: %p %p %ld",
		      &(state->rx_mr), 
		      (state->rx_buf_mempool).buf, 
		      (state->rx_buf_mempool).len, 
		      &(state->tx_mr),
		      state->server_working_set,
		      working_set_size);
	
    
    if (mode == UDP_CLIENT) {
        // init rx and tx memory mempools
      ret = mempool_memory_init(&(state->tx_buf_mempool),
				REQ_MBUFS_SIZE,
				REQ_MBUFS_PER_PAGE,
				REQ_MBUFS_PAGES);
      RETURN_ON_ERR(ret, "Failed to init tx mempool for client: %s", strerror(errno));

      ret = memory_registration(pd,
				&(state->tx_mr), 
				(state->tx_buf_mempool).buf, 
				(state->tx_buf_mempool).len, 
				IBV_ACCESS_LOCAL_WRITE);
        RETURN_ON_ERR(ret, "Failed to run memory registration for tx buffer for client: %s", strerror(errno));

        ret = mempool_memory_init(&(state->rx_buf_mempool),
                                    DATA_MBUFS_SIZE,
                                    DATA_MBUFS_PER_PAGE,
                                    DATA_MBUFS_PAGES);
        RETURN_ON_ERR(ret, "Failed to int rx mempool for client: %s", strerror(errno));

        ret = memory_registration(pd,
				  &(state->rx_mr), 
				  (state->rx_buf_mempool).buf, 
				  (state->rx_buf_mempool).len, 
				  IBV_ACCESS_LOCAL_WRITE);
        RETURN_ON_ERR(ret, "Failed to run memory reg for client rx region: %s", strerror(errno));
    } else {
      ret = server_memory_init(&(state->server_working_set), working_set_size);
        RETURN_ON_ERR(ret, "Failed to init server working set memory");

        /* Recieve packets are request side on the server */
        ret = mempool_memory_init(&(state->rx_buf_mempool),
				  REQ_MBUFS_SIZE,
				  REQ_MBUFS_PER_PAGE,
				  REQ_MBUFS_PAGES);
	NETPERF_DEBUG("init 3");

        RETURN_ON_ERR(ret, "Failed to int rx mempool for server: %s", strerror(errno));

        ret = memory_registration(pd,
				  &(state->rx_mr), 
				  (state->rx_buf_mempool).buf, 
				  (state->rx_buf_mempool).len, 
				  IBV_ACCESS_LOCAL_WRITE);
        RETURN_ON_ERR(ret, "Failed to run memory reg for client rx region: %s", strerror(errno));

        if (!zero_copy) {
            // initialize tx buffer memory pool for network packets
	  ret = mempool_memory_init(&(state->tx_buf_mempool),
                                       DATA_MBUFS_SIZE,
                                       DATA_MBUFS_PER_PAGE,
                                       DATA_MBUFS_PAGES);
	  NETPERF_DEBUG("init 4 not zero copy");
            RETURN_ON_ERR(ret, "Failed to init tx buf mempool on server: %s", strerror(errno));

            ret = memory_registration(pd,
				      &(state->tx_mr), 
				      (state->tx_buf_mempool).buf, 
				      (state->tx_buf_mempool).len, 
				      IBV_ACCESS_LOCAL_WRITE);
	    
            RETURN_ON_ERR(ret, "Failed to register tx mempool on server: %s", strerror(errno));
        } else {
            // register the server memory region for zero-copy
	  NETPERF_DEBUG("init 4 zero copy");
	  ret = memory_registration(pd,
				    &(state->tx_mr),
				    state->server_working_set,
				    working_set_size,
				    IBV_ACCESS_LOCAL_WRITE);
	  NETPERF_DEBUG("init 4 zero copy done");
	  RETURN_ON_ERR(ret, "Failed to register memory for server working set: %s", strerror(errno)); 
        }
    }

    // Initialize single rxq attached to the rx mempool
    ret = mlx5_init_rxq(&(state->rxqs[0]), &(state->rx_buf_mempool),
			context, pd, state->rx_mr);
    NETPERF_DEBUG("init 5");
    
    RETURN_ON_ERR(ret, "Failed to create rxq: %s", strerror(-ret));
    
    // Initialize txq
    // TODO: for a fair comparison later, initialize the tx segments at runtime
    int init_each_tx_segment = 1;
    if (mode == UDP_SERVER && num_segments > 1 && zero_copy) {
        init_each_tx_segment = 0;
    }
    ret = mlx5_init_txq(&(state->txqs[0]), 
			pd, context,
			state->tx_mr, 
			max_inline_data, 
			init_each_tx_segment);

    NETPERF_DEBUG("init 6");
    RETURN_ON_ERR(ret, "Failed to initialize tx queue");

    NETPERF_INFO("Finished creating txq and rxq");
    return ret;
}

int init_mlx5_steering(CoreState* states, int num_states) {
    struct eth_addr *my_eth = &server_mac;
    struct eth_addr *other_eth = &client_mac;
    if (mode == UDP_CLIENT) {
        my_eth = &client_mac;
        other_eth = &server_mac;
    }


    // Initialize a single indirection table for all flows.
    struct mlx5_rxq** queues = (struct mlx5_rxq**)malloc(sizeof(struct mlx5_rxq*)*num_cores);

    for ( int i = 0; i < num_cores; i++ ) {
      queues[i] = &(states[i].rxqs[0]);
    }
    
    int ret = mlx5_qs_init_flows(queues,
				 num_cores,
				 pd, context,
				 my_eth, other_eth);
    RETURN_ON_ERR(ret, "Failed to install queue steering rules");
    return ret;
}

int parse_outgoing_request_header(RequestHeader *request_header, struct mbuf *mbuf, size_t payload_size) {
    unsigned char *ptr = mbuf->data;
    struct eth_hdr * const eth = (struct eth_hdr *)ptr;
    ptr += sizeof(struct eth_hdr);
    struct ip_hdr * const ipv4 = (struct ip_hdr *)ptr;
    ptr += sizeof(struct ip_hdr);
    struct udp_hdr *const udp = (struct udp_hdr *)ptr;
    ptr += sizeof(struct udp_hdr);
    uint64_t packet_id = *(uint64_t *)ptr;
    return initialize_reverse_request_header(request_header,
                                                eth,
                                                ipv4,
                                                udp,
                                                payload_size,
                                                packet_id);
}

int check_valid_packet(struct mbuf *mbuf, void **payload_out, uint32_t *payload_len, struct eth_addr *our_eth) {
    NETPERF_ASSERT(((char *)mbuf->data - (char *)mbuf) == RX_BUF_HEAD, "rx mbuf data pointer not set correctly");
    NETPERF_DEBUG("Mbuf addr: %p, mbuf data addr: %p, diff: %lu, mbuf len: %u", mbuf, mbuf->data, (char *)(mbuf->data) - (char *)mbuf, (unsigned)(mbuf_length(mbuf)));
    unsigned char *ptr = mbuf->data;
    struct eth_hdr * const eth = (struct eth_hdr *)ptr;
    ptr += sizeof(struct eth_hdr);
    struct ip_hdr * const ipv4 = (struct ip_hdr *)ptr;
    ptr += sizeof(struct ip_hdr);
    struct udp_hdr *const udp = (struct udp_hdr *)ptr;
    ptr += sizeof(struct udp_hdr);

    NETPERF_DEBUG("Fields: %d %d %d %d\n", ipv4->saddr, ipv4->daddr, udp->src_port, udp->dst_port);
    
    // check if the dest eth hdr is correct
    if (eth_addr_equal(our_eth, &eth->dhost) != 1) {
        NETPERF_DEBUG("Bad MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8,
            eth->dhost.addr[0], eth->dhost.addr[1],
		      eth->dhost.addr[2], eth->dhost.addr[3],
		      eth->dhost.addr[4], eth->dhost.addr[5]);
        return 0;
    }

    uint16_t eth_type = ntohs(eth->type);
    if (eth_type != ETHTYPE_IP) {
        NETPERF_DEBUG("Bad eth type: %x; returning", (unsigned)eth_type);
        return 0;
    }

    // check IP header
    if (ipv4->proto != IPPROTO_UDP) {
        NETPERF_DEBUG("Bad recv type: %u; returning", (unsigned)ipv4->proto);
        return 0;
    }

    //NETPERF_DEBUG("Ipv4 checksum: %u, Ipv4 ttl: %u, udp checksum: %u", (unsigned)(ntohs(ipv4->chksum)), (unsigned)ipv4->ttl, (unsigned)(ntohs(udp->chksum)));

    // TODO: finish checks
    *payload_out = (void *)ptr;
    *payload_len = mbuf_length(mbuf) - FULL_HEADER_SIZE;

    NETPERF_DEBUG("Received packet with size %lu", *payload_len);
    return 1;
}

uint64_t calculate_checksum(void *payload_ptr, size_t amt_to_add, size_t payload_len) {
    uint64_t ret = 0;
    // read something from each cacheline
    for (char *ptr = (char *)payload_ptr + amt_to_add; ptr < ((char *)payload_ptr + payload_len); ptr += 64) {
        // TODO: should we not always read from beginning?
        if (*ptr == 'a') {
            ret += 1;
        } else {
            ret += 2;
        }
    }
    return ret;
}
                            

int process_server_request(struct mbuf *request, 
			   void *payload, 
			   size_t payload_len, 
			   int total_packets_required,
			   uint64_t recv_timestamp,
			   CoreState* state)
{
#ifdef __TIMERS__
    uint64_t start_construct = cycletime();
#endif
    NETPERF_DEBUG("Total pkts required: %d", total_packets_required);
    uint64_t segments[MAX_SCATTERS];
    struct mbuf *send_mbufs[MAX_PACKETS][MAX_SCATTERS];
    
        
    for (size_t i = 0; i < num_segments; i++) {
        segments[i] = read_u64(payload, i + SEGLIST_OFFSET);
        NETPERF_DEBUG("Segment %u is %u", (unsigned)i, (unsigned)segments[i]);
    }
    RequestHeader request_header;
    if (read_incoming_packet == 1) {
        size_t amt_to_add = 64 - (sizeof(OutgoingHeader)); // gets the next cache line after header
        // reads all of the data in the packet and creates a check sum
        checksum = calculate_checksum(payload, amt_to_add, payload_len);
        request_header.checksum = checksum;
        // write it into the outgoing packet
    }

    size_t payload_left = num_segments * segment_size;
    size_t segment_array_idx = 0;
    
    // TODO: this technically isn't correct if each request actually sends more
    // than one packet
    // We are providing one request header, but in reality there should be n
    // with the payload size for that split up packet
    int ret = parse_outgoing_request_header(&request_header, request, payload_left);
    RETURN_ON_ERR(ret, "constructing outgoing header failed");

    for (int pkt_idx = 0; pkt_idx < total_packets_required; pkt_idx++) {
        state->request_headers[pkt_idx] = &request_header;
        size_t actual_segment_size = MIN(payload_left, MIN(segment_size, MAX_SEGMENT_SIZE));
        size_t nb_segs_per_packet = (MIN(payload_left, MAX_SEGMENT_SIZE)) / actual_segment_size;
        size_t pkt_len = actual_segment_size * nb_segs_per_packet;
        NETPERF_DEBUG("Pkt %d: seg size %u, nb_segs %u, pkt_len %u", pkt_idx,
                        (unsigned)actual_segment_size, (unsigned)nb_segs_per_packet, (unsigned)pkt_len);

        if (zero_copy) {
            struct mbuf *prev = NULL;
            for (int seg = 0; seg < nb_segs_per_packet; seg++) {
                void *server_memory = get_server_region(state->server_working_set,
							segments[segment_array_idx],
							segment_size);
                // allocate mbuf 
                send_mbufs[pkt_idx][seg] = (struct mbuf *)mempool_alloc(&(state->mbuf_mempool));
                if (send_mbufs[pkt_idx][seg] == NULL) {
                    // free all previous allocated mbufs
                    for (size_t pkt = 0; pkt < pkt_idx; pkt++) {
                        if (pkt == pkt_idx) {
                            break;
                        }
                        mbuf_free_to_mempool(&(state->mbuf_mempool), send_mbufs[pkt_idx][0]);
                    }
                    return ENOMEM;
                }
                send_mbufs[pkt_idx][seg]->lkey = state->tx_mr->lkey;

                // set the next buffer pointer for previous mbuf
                if (prev != NULL) {
                    prev->next = send_mbufs[pkt_idx][seg];
                }
                prev = send_mbufs[pkt_idx][seg];

		// set metadata for this mbuf
                send_mbufs[pkt_idx][seg]->release = NULL;
                send_mbufs[pkt_idx][seg]->release_to_mempool = zero_copy_tx_completion;
                send_mbufs[pkt_idx][seg]->release_to_mempools = NULL;
                mbuf_init(send_mbufs[pkt_idx][seg], 
                            (unsigned char *)server_memory,
                            actual_segment_size,
                            0);

                send_mbufs[pkt_idx][seg]->len = actual_segment_size;

                segment_array_idx += 1;
            }
            // for the first packet: set number of segments
            send_mbufs[pkt_idx][0]->nb_segs = nb_segs_per_packet;

            // for last packet: set next as null
            send_mbufs[pkt_idx][nb_segs_per_packet - 1]->next = NULL;
        } else {
            // single buffer for this packet
	  send_mbufs[pkt_idx][0] = (struct mbuf *)mempool_alloc(&(state->mbuf_mempool));
            //NETPERF_INFO("Allocatng mbuf %p", send_mbufs[pkt_idx][0]);
            // allocate backing buffer for this mbuf
            if (send_mbufs[pkt_idx][0] == NULL) {
                return ENOMEM;
            }
            send_mbufs[pkt_idx][0]->lkey = state->tx_mr->lkey;
            //NETPERF_INFO("Allocatng mbuf %p", send_mbufs[pkt_idx][0]);
            // allocate backing buffer for this mbuf
	    unsigned char *buffer = (unsigned char *)mempool_alloc(&(state->tx_buf_mempool));
            if (buffer == NULL) {
	      mbuf_free_to_mempools(&(state->tx_buf_mempool),
				    &(state->mbuf_mempool),
				    send_mbufs[pkt_idx][0]);
	      return ENOMEM;
            }
            //NETPERF_INFO("Allocating mbuf buffer %p", buffer);
            mbuf_init(send_mbufs[pkt_idx][0],
		      buffer,
		      DATA_MBUFS_SIZE,
		      0);
            // set release as non zero-copy release function
            send_mbufs[pkt_idx][0]->release_to_mempools = tx_completion;
            send_mbufs[pkt_idx][0]->next = NULL; // set the next packet as null

            for (int seg = 0; seg < nb_segs_per_packet; seg++) {
	      void *server_memory = get_server_region(state->server_working_set, 
							segments[segment_array_idx],
							actual_segment_size);

                // TODO: handle case where header is NOT initialized in memory
                NETPERF_DEBUG("Copying into offset %u of mbuf", (unsigned)(seg * segment_size));
                mbuf_copy(send_mbufs[pkt_idx][0], 
                            (char *)server_memory,
                            actual_segment_size,
                            seg * actual_segment_size);
                segment_array_idx += 1;

            }

            // for the first packet, set the number of segmens
            send_mbufs[pkt_idx][0]->nb_segs = 1;
        }

        send_mbufs[pkt_idx][0]->pkt_len = pkt_len;
    }

    // now actually transmit the packets
    int total_sent = 0;
#ifdef __TIMERS__
    uint64_t end_construct = cycletime();
    add_latency(&(state->server_construction_dist), end_construct - start_construct);
    uint64_t start_send = cycletime();
#endif
    size_t tries = 0;
    while (total_sent < total_packets_required) {
        int sent = mlx5_transmit_batch(send_mbufs, 
				       total_sent, // index to start on
				       total_packets_required - total_sent, // tota
				       &(state->txqs[0]),
				       state->request_headers,
				       state->inline_lengths,
				       &(state->tx_buf_mempool),
				       &(state->mbuf_mempool),
				       zero_copy);
        tries += 1;
        NETPERF_DEBUG("Successfully sent from %d, %d packets", total_sent, sent);
        if (sent < (total_packets_required - total_sent)) {
            NETPERF_DEBUG("Trying to send request again");
        }
        total_sent += sent;
        // TODO: It doesn't seem sensible to send "part" of one response if
        // more than one packet is required
	NETPERF_DEBUG("total %d %d", total_sent, total_packets_required);
        if (total_sent < total_packets_required) {
            // free all buffers used for this request, that weren't sent
            for (size_t pkt = total_sent; pkt < total_packets_required; pkt++) {
	      NETPERF_DEBUG("freeing %d", pkt);
	      if ( zero_copy ) {
		mbuf_free_to_mempool(&(state->mbuf_mempool), send_mbufs[pkt][0]);
	      } else {
		mbuf_free_to_mempools(&(state->tx_buf_mempool),
				      &(state->mbuf_mempool),
				      send_mbufs[pkt][0]);
	      }
            }
            return ENOMEM;
        }
    }
#ifdef __TIMERS__
    uint64_t end_send = cycletime();
    add_latency(&(state->server_send_dist), end_send - start_send);
#endif

    return 0;
}

void* do_server(CoreState* state) {
  NETPERF_DEBUG("Starting server program - mbuf size %ld", sizeof(struct mbuf));
    struct mbuf *recv_mbufs[BURST_SIZE];
    int total_packets_required = calculate_total_packets_required((size_t)segment_size, (size_t)num_segments);
    int num_received = 0;
    while (!(state->done)) {
      //NETPERF_DEBUG("Calling gather on core %d...", state->idx);
        num_received = mlx5_gather_rx((struct mbuf **)&recv_mbufs, 
				      BURST_SIZE,
				      &(state->rx_buf_mempool),
				      &(state->rxqs[0]));
        if (num_received > 0) {
	  NETPERF_DEBUG("Received packets %d on core %d\n", num_received, state->idx);
            uint64_t recv_time = nanotime();
            for (int  i = 0; i < num_received; i++) {
                struct mbuf *pkt = recv_mbufs[i];

                void *payload_out = NULL;
                uint32_t payload_len = 0;

                if (check_valid_packet(pkt, 
                                        &payload_out, 
                                        &payload_len, 
                                        &server_mac) != 1) {
                    NETPERF_DEBUG("Received invalid pkt");
                    mbuf_free_to_mempool(&(state->rx_buf_mempool), pkt);
                    continue;
                }

#ifdef __TIMERS__
                uint64_t cycles_start = cycletime();
#endif
                // do busy work
                if (busy_iters > 0)  {
                    busy_work_res = do_busy_work(busy_iters / 2);
                }
#ifdef __TIMERS__
                uint64_t end_busy = cycletime();
                add_latency(&(state->busy_work_dist), end_busy - cycles_start);

#endif
                int ret = process_server_request(pkt, 
						 payload_out, 
						 payload_len,
						 total_packets_required,
						 recv_time,
						 state);
		NETPERF_DEBUG("freeing pkt");
                mbuf_free_to_mempool(&(state->rx_buf_mempool), pkt);
		NETPERF_DEBUG("done.");
                if (ret == ENOMEM) {
                    NETPERF_DEBUG("Server overloaded, dropping request");
                } else {
#ifdef __TIMERS__
                uint64_t cycles_end = cycletime();
                add_latency(&(state->server_request_dist), cycles_end - cycles_start);
#endif
		//                mbuf_free(pkt);
                RETURN_ON_ERR(ret, "Error processing request");
                }
            }
        }
    }

    NETPERF_DEBUG("thread %d received done, exiting", state->idx);
    return (void*) 0;
}

// cleanup on the server-side
void sig_handler(int signo) {
    NETPERF_INFO("In request handler");
    // if debug timers were turned on, dump them
#ifdef __TIMERS__
    if (mode == UDP_SERVER) {
      for ( int i = 0; i < num_cores; i++ ) {
        NETPERF_INFO("----");
        NETPERF_INFO("server request processing timers: ");
        dump_debug_latencies(&per_core_state[i].server_request_dist, 1);
        NETPERF_INFO("----");
        NETPERF_INFO("server send processing timers: ");
        dump_debug_latencies(&per_core_state[i].server_send_dist, 1);
        NETPERF_INFO("----");
        NETPERF_INFO("----");
        NETPERF_INFO("server pkt construct processing timers: ");
        dump_debug_latencies(&per_core_state[i].server_construction_dist, 1);
        NETPERF_INFO("----");

        NETPERF_INFO("busy work timers: ");
        dump_debug_latencies(&(per_core_state[i].busy_work_dist), 1);
        NETPERF_INFO("----");
      }
    }
#endif

    NETPERF_INFO("setting done");
    for ( int i = 0; i < num_cores; i++ ) {
      NETPERF_INFO("setting done for thread %d", i);
      per_core_state[i].done = 1;
    }
    NETPERF_INFO("set done for all");
    fflush(stdout);
    NETPERF_INFO("flush stdout");
    fflush(stderr);

    //NETPERF_INFO("exit");
    //exit(0);
}


int main(int argc, char *argv[]) {
    int ret = 0;
    
    seed_rand();
    // Global time initialization
    ret = time_init();

    NETPERF_DEBUG("In netperf program");
    ret = parse_args(argc, argv);
    if (ret) {
        NETPERF_WARN("parse_args() failed.");
    }

    per_core_state = (CoreState*)malloc(sizeof(CoreState) * num_cores); // TODO free

    printf("Initializing states\n");
    ret = init_all_states(per_core_state);
    if (ret) {
        NETPERF_WARN("init_all_states() failed.");
        return ret;
    }

    if (ret) {
        NETPERF_WARN("Time initialization failed.");
        return ret;
    }

    ret = init_ibv_context(&context,
			   &pd, &nic_pci_addr);
    if ( ret ) {
      NETPERF_WARN("Failed to init ibv context: %s", strerror(errno));
      return ret;
    }

    
    // Mlx5 queue and flow initialization
    ret = 0;
    for ( int i = 0; i < num_cores; i++ ) {
      ret |= init_mlx5(&per_core_state[i]);
    }
    
    if (ret) {
        NETPERF_WARN("init_mlx5() failed.");
        return ret;
    }
    
    // initialize RSS indirection tables
    ret = init_mlx5_steering(per_core_state, num_cores);
    if ( ret ) {
      NETPERF_WARN("init_mlx5_steering() failed.");
      return ret;
    }

    // initialize the workload
    ret = 0;
    for ( int i = 0; i < num_cores; i++ ) {
      ret |= init_workload(&per_core_state[i]); 
    }
    if (ret) {
        NETPERF_WARN("Init of workload failed.");
        return ret;
    }

    if (mode == UDP_CLIENT) {
      ret = 1; 
        if (ret) {
            NETPERF_WARN("Client returned non-zero status.");
            return ret;
        }
    } else {
        // set up signal handler
        if (signal(SIGINT, sig_handler) == SIG_ERR)
            printf("\ncan't catch SIGINT\n");
	int results[num_cores];
	pthread_t threads[num_cores];
	for ( int i = 0; i < num_cores; i++ ) {
	  printf("Starting core %d\n", i);
	  results[i] = pthread_create(&threads[i], NULL, do_server, &per_core_state[i]);
	}

	for ( int i = 0; i < num_cores; i++ ) {
	  pthread_join(threads[i], NULL);
	}

	for ( int i = 0; i < num_cores; i++ ) { ret |= results[i]; }

	for ( int i = 0; i < num_cores; i++ ) {
	  NETPERF_DEBUG("cleaning up thread %d", i);
	  cleanup_mlx5(&per_core_state[i]);
	  NETPERF_DEBUG("done thread %d.", i);
	}

	NETPERF_DEBUG("done cleanup all threads.");
	return ret;
    }

    NETPERF_DEBUG("exiting.");

    return ret;
}


