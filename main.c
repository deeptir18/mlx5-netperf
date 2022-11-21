#include <inttypes.h>
#include <math.h>
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
static uint64_t checksum = 0;
static int read_incoming_packet = 0;
static double busy_work_res;
static uint8_t mode;
static int echo_mode = 0;
static struct eth_addr server_mac;
static struct eth_addr client_mac;
static uint32_t server_ip;
static uint32_t client_ip;
static uint32_t server_port = 54321; 
static uint32_t client_port = 54321;
static size_t num_segments = 1;
static size_t segment_size = 1024;
static size_t working_set_size = 16384;
static size_t busy_work_us = 0;
static size_t busy_iters = 0;
static int zero_copy = 1;
static int has_latency_log = 0;
static char *latency_log;
static int has_ready_file = 0;
static char *ready_file;
static RateDistribution rate_distribution = {.type = UNIFORM, .rate_pps = 5000, .total_time = 2};
static ClientRequest *client_requests = NULL; // pointer to client request information
static OutgoingHeader header = {}; // header to copy in to the request
static Latency_Dist_t latency_dist = { .min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0 };
static Packet_Map_t packet_map = {.total_count = 0, .grouped_rtts = NULL, .sent_ids = NULL };
static RequestHeader *request_header_ptrs[BATCH_SIZE];
static size_t inline_lengths[BATCH_SIZE];
static RequestHeader request_headers[BATCH_SIZE];

#ifdef __TIMERS__
static Latency_Dist_t server_request_dist = { .min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0, .allocated = 0 };
static Latency_Dist_t server_send_dist = {.min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0, .allocated = 0 };
static Latency_Dist_t server_construction_dist = {.min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0, .allocated = 0};
static Latency_Dist_t busy_work_dist = {.min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0, .allocated = 0};
static Latency_Dist_t server_tries_dist = {.min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0, .allocated = 0};
static Latency_Dist_t server_burst_ct_dist = {.min = LONG_MAX, .max = 0, .total_count = 0, .latency_sum = 0, .allocated = 0};
#endif


static void *server_working_set;
static struct mlx5_rxq rxqs[NUM_QUEUES];
static struct mlx5_txq txqs[NUM_QUEUES];
static struct ibv_context *context;
static struct ibv_pd *pd;
static struct ibv_mr *tx_mr;
static struct ibv_mr *rx_mr;
static struct pci_addr nic_pci_addr;
static size_t max_inline_data = 64;

// extern (declared in mlx5_rxtx.h)
struct mempool rx_buf_mempool = {};
struct mempool tx_buf_mempool = {};
struct mempool mbuf_mempool = {};
uint32_t total_dropped = 0;

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
        {"ready_file",optional_argument, 0, 'f'},
        {"with_copy", no_argument, 0, 'z'},
        {"read_incoming_packet", no_argument, 0, 'd'},
        {"echo_mode", no_argument, 0, 'b'},
        {0,           0,                 0,  0   }
    };
    int long_index = 0;
    int ret;
    while ((opt = getopt_long(argc, argv, "m:w:c:e:i:s:k:q:a:z:r:t:l:d:b:f:",
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
                zero_copy = 0;
                break;
            case 'r': // rate
                str_to_long(optarg, &tmp);
                rate_distribution.rate_pps = tmp;
                break;
            case 't': // total_time
                str_to_long(optarg, &tmp);
                rate_distribution.total_time = tmp;
                break;
            case 'l':
                has_latency_log = 1;
                latency_log = strdup(optarg);
                break;
            case 'd':
                read_incoming_packet = 1;
                break;
            case 'b':
                echo_mode = 1;
                break;
            case 'f':
                has_ready_file = 1;
                ready_file = strdup(optarg);
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
int init_workload() {
    int ret = 0;

    if (mode == UDP_CLIENT) {
        // 1 uint64_t per segment
        size_t client_payload_size = sizeof(uint64_t) * ( SEGLIST_OFFSET + num_segments );
        ret = initialize_outgoing_header(&header,
                                    &client_mac,
                                    &server_mac,
                                    client_ip,
                                    server_ip,
                                    client_port,
                                    server_port,
                                    client_payload_size);
        RETURN_ON_ERR(ret, "Failed to initialize outgoing header");
        ret = initialize_client_requests(&client_requests,
                                            &rate_distribution,
                                            segment_size,
                                            num_segments,
                                            working_set_size);
        RETURN_ON_ERR(ret, "Failed to initialize client requests: %s", strerror(-errno));
    } else {
        // num_segments * segment_size
        ret = initialize_server_memory(server_working_set,
                                        segment_size,
                                        working_set_size);
        RETURN_ON_ERR(ret, "Failed to fill in server memory: %s", strerror(ret));
        for (size_t i = 0; i < BATCH_SIZE; i++) {
            if (echo_mode == 1) {
                // header inlined into packet data
                inline_lengths[i] = 0;
                request_header_ptrs[i] = NULL;
            } else {
                inline_lengths[i] = sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + sizeof(uint64_t) + sizeof(uint64_t);
                request_header_ptrs[i] = &request_headers[i];
            }
        }

    }

    return 0;
}

int cleanup_mlx5() {
    int ret = 0;
    // mbuf mempool
    ret = munmap(mbuf_mempool.buf, mbuf_mempool.len);
    RETURN_ON_ERR(ret, "Failed to unmap mbuf mempool: %s", strerror(errno));
    
    if (mode == UDP_CLIENT) {
        ret = memory_deregistration(tx_mr);
        RETURN_ON_ERR(ret, "Failed to dereg tx_mr: %s", strerror(errno));
        ret = munmap(tx_buf_mempool.buf, tx_buf_mempool.len);
        RETURN_ON_ERR(ret, "Failed to munmap tx mempool for client: %s", strerror(errno));

        ret = memory_deregistration(rx_mr);
        RETURN_ON_ERR(ret, "Failed to dereg rx_mr: %s", strerror(errno));
        ret = munmap(rx_buf_mempool.buf, rx_buf_mempool.len);
        RETURN_ON_ERR(ret, "Failed to munmap rx mempool for client: %s", strerror(errno));
    } else {
        ret = memory_deregistration(rx_mr);
        RETURN_ON_ERR(ret, "Failed to dereg rx_mr: %s", strerror(errno));
        ret = munmap(rx_buf_mempool.buf, rx_buf_mempool.len);
        RETURN_ON_ERR(ret, "Failed to munmap rx mempool for server: %s", strerror(errno));
            
        // for both the zero-copy and non-zero-copy, un register region for tx
        ret = memory_deregistration(tx_mr);
        RETURN_ON_ERR(ret, "Failed to dereg tx_mr: %s", strerror(errno));
        if (!zero_copy) {
            ret = munmap(tx_buf_mempool.buf, tx_buf_mempool.len);
            RETURN_ON_ERR(ret, "Failed to munmap tx mempool for server: %s", strerror(errno));
        }
        // free the server memory
        ret = munmap(server_working_set, align_up(working_set_size, PGSIZE_2MB));
        RETURN_ON_ERR(ret, "Failed to unmap server working set memory");
    }
    return 0;
}

int init_mlx5() {
    int ret = 0;
    
    ret = init_ibv_context(&context, &pd, &nic_pci_addr);
    RETURN_ON_ERR(ret, "Failed to init ibv context: %s", strerror(errno));

    // Alloc memory pool for TX mbuf structs
    ret = mempool_memory_init(&mbuf_mempool,
                                CONTROL_MBUFS_SIZE,
                                CONTROL_MBUFS_PER_PAGE,
                                REQ_MBUFS_PAGES);
    RETURN_ON_ERR(ret, "Failed to init mbuf mempool: %s", strerror(errno));

    if (mode == UDP_CLIENT) {
        // init rx and tx memory mempools
        ret = mempool_memory_init(&tx_buf_mempool,
                                    REQ_MBUFS_SIZE,
                                    REQ_MBUFS_PER_PAGE,
                                    REQ_MBUFS_PAGES);
        RETURN_ON_ERR(ret, "Failed to init tx mempool for client: %s", strerror(errno));

        ret = memory_registration(pd, 
                                    &tx_mr, 
                                    tx_buf_mempool.buf, 
                                    tx_buf_mempool.len, 
                                    IBV_ACCESS_LOCAL_WRITE);
        RETURN_ON_ERR(ret, "Failed to run memory registration for tx buffer for client: %s", strerror(errno));

        ret = mempool_memory_init(&rx_buf_mempool,
                                    DATA_MBUFS_SIZE,
                                    DATA_MBUFS_PER_PAGE,
                                    DATA_MBUFS_PAGES);
        RETURN_ON_ERR(ret, "Failed to int rx mempool for client: %s", strerror(errno));

        ret = memory_registration(pd, 
                                    &rx_mr, 
                                    rx_buf_mempool.buf, 
                                    rx_buf_mempool.len, 
                                    IBV_ACCESS_LOCAL_WRITE);
        RETURN_ON_ERR(ret, "Failed to run memory reg for client rx region: %s", strerror(errno));
    } else {
        ret = server_memory_init(&server_working_set, working_set_size);
        RETURN_ON_ERR(ret, "Failed to init server working set memory");

        /* Recieve packets are request side on the server */
        ret = mempool_memory_init(&rx_buf_mempool,
                                    REQ_MBUFS_SIZE,
                                    REQ_MBUFS_PER_PAGE,
                                    REQ_MBUFS_PAGES);
        RETURN_ON_ERR(ret, "Failed to init rx mempool for server: %s", strerror(errno));

        ret = memory_registration(pd, 
                                    &rx_mr, 
                                    rx_buf_mempool.buf, 
                                    rx_buf_mempool.len, 
                                    IBV_ACCESS_LOCAL_WRITE);
        RETURN_ON_ERR(ret, "Failed to run memory reg for client rx region: %s", strerror(errno));
        if (!zero_copy) {
            // initialize tx buffer memory pool for network packets
            ret = mempool_memory_init(&tx_buf_mempool,
                                       DATA_MBUFS_SIZE,
                                       DATA_MBUFS_PER_PAGE,
                                       DATA_MBUFS_PAGES);
            RETURN_ON_ERR(ret, "Failed to init tx buf mempool on server: %s", strerror(errno));

            ret = memory_registration(pd, 
                                        &tx_mr, 
                                        tx_buf_mempool.buf, 
                                        tx_buf_mempool.len, 
                                        IBV_ACCESS_LOCAL_WRITE);

            RETURN_ON_ERR(ret, "Failed to register tx mempool on server: %s", strerror(errno));
        } else {
            // register the server memory region for zero-copy
            ret = memory_registration(pd, 
                                        &tx_mr,
                                        server_working_set,
                                        working_set_size,
                                        IBV_ACCESS_LOCAL_WRITE);
            RETURN_ON_ERR(ret, "Failed to register memory for server working set: %s", strerror(errno)); 
        }
    }

    // Initialize single rxq attached to the rx mempool
    ret = mlx5_init_rxq(&rxqs[0], &rx_buf_mempool, context, pd, rx_mr);
    RETURN_ON_ERR(ret, "Failed to create rxq: %s", strerror(-ret));

    struct eth_addr *my_eth = &server_mac;
    struct eth_addr *other_eth = &client_mac;
    if (mode == UDP_CLIENT) {
        my_eth = &client_mac;
        other_eth = &server_mac;
    }

    ret = mlx5_qs_init_flows(&rxqs[0], pd, context, my_eth, other_eth);
    RETURN_ON_ERR(ret, "Failed to install queue steering rules");

    // TODO: for a fair comparison later, initialize the tx segments at runtime
    int init_each_tx_segment = 1;
    if (mode == UDP_SERVER && num_segments > 1 && zero_copy) {
        init_each_tx_segment = 0;
    }
    size_t expected_inline_length = sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + sizeof(uint64_t) * 2;
    size_t expected_segs = num_segments;
    if (echo_mode == 1) {
        expected_inline_length = 0;
    }
    if (zero_copy != 1) {
        expected_segs = 1;
    }
    ret = mlx5_init_txq(&txqs[0], 
                            pd, 
                            context, 
                            tx_mr, 
                            max_inline_data, 
                            init_each_tx_segment,
                            expected_segs,
                            expected_inline_length);
    RETURN_ON_ERR(ret, "Failed to initialize tx queue");

    NETPERF_INFO("Finished creating txq and rxq");
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
    uint32_t packet_id = *(uint32_t *)ptr;
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
    //struct udp_hdr *const udp = (struct udp_hdr *)ptr;
    ptr += sizeof(struct udp_hdr);

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
        NETPERF_DEBUG("Bad eth type: %u; returning", (unsigned)eth_type);
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
    NETPERF_DEBUG("Received packet with size %u", *payload_len);
    return 1;

}

int do_client() {
    NETPERF_DEBUG("Starting client");
    struct mbuf *recv_pkts[BURST_SIZE];
    struct mbuf *pkt;
    size_t client_payload_size = sizeof(uint64_t) * (num_segments + SEGLIST_OFFSET);
    int total_packets_per_request = calculate_total_packets_required((uint32_t)segment_size, (uint32_t)num_segments);
    
    size_t num_received = 0;
    size_t outstanding = 0;
    RequestHeader request_header;

    uint64_t total_time = rate_distribution.total_time * 1e9;
    uint64_t start_time_offset = nanotime();
    uint64_t start_cycle_offset = ns_to_cycles(start_time_offset);
    
    // first client request (first packet sent at time 0)
    ClientRequest *current_request = get_client_req(client_requests, 0);

    uint64_t last_sent_cycle = start_cycle_offset;
    uint64_t lateness_budget = 0;
    // main processing loop
    while (nanotime() < (start_time_offset + total_time)) {
        // allocate actual mbuf struct
        pkt = (struct mbuf *)mempool_alloc(&mbuf_mempool);
        if (pkt == NULL) {
            NETPERF_WARN("Error allocating mbuf for req # %u; recved %u", (unsigned)current_request->packet_id, (unsigned)num_received);
            return -ENOMEM;
        }

        // allocate buffer backing the mbuf
        unsigned char *buffer = (unsigned char *)mempool_alloc(&tx_buf_mempool);
        mbuf_init(pkt, buffer, REQ_MBUFS_SIZE, 0);
        
        // frees the backing store back to tx_buf_mempool and the actual struct
        // back to the mbuf_mempool
        pkt->release = tx_completion;
        pkt->lkey = tx_mr->lkey;

        // copy data into the mbuf
        mbuf_copy(pkt, 
                    (char *)&header, 
                    sizeof(OutgoingHeader), 
                    0);
        mbuf_copy(pkt, 
                    (char *)current_request + sizeof(uint64_t), 
                    client_payload_size, 
                    sizeof(OutgoingHeader));
        int sent = 0;
        // TODO: add a timer here, to measure, in nanoseconds, how late each
        // transmission is, compared to the actual intersend time
        uint64_t cur_send = cycles_offset(start_cycle_offset);
        //lateness_budget += cur_send - (last_sent_cycle + current_request->timestamp_offset);
        current_request->timestamp_offset = cur_send;
        NETPERF_DEBUG("Attempting to transmit packet %u (send cycles %lu, time since last %lu), recved %u", (unsigned)current_request->packet_id, current_request->timestamp_offset, cycles_to_ns(current_request->timestamp_offset -last_sent_cycle), (unsigned)num_received);
        last_sent_cycle = current_request->timestamp_offset;
        
        while (sent != 1) {
            sent = mlx5_transmit_one(pkt, &txqs[0], &request_header, 0);
        }
        
        current_request += 1;

        while (true) {
            // if time to send the next packet, break
            if (lateness_budget > current_request->timestamp_offset) {
                lateness_budget -= current_request->timestamp_offset;
                break;
            }
            if (cycles_offset(start_cycle_offset) >= (current_request->timestamp_offset - lateness_budget) + last_sent_cycle) {
                lateness_budget = 0;
                break;
            }
            int nb_rx = mlx5_gather_rx((struct mbuf **)&recv_pkts, 
                                        32,
                                        &rx_buf_mempool,
                                        &rxqs[0]);
            
            // process any received packets
            for (int i = 0; i < nb_rx; i++) {
                void *payload;
                uint32_t payload_len;
                if (check_valid_packet(recv_pkts[i], &payload, &payload_len, &client_mac) != 1) {
                    NETPERF_DEBUG("Received invalid packet back");
                    mbuf_free(recv_pkts[i]);
                    continue;
                }
                NETPERF_ASSERT((payload_len) == 
                                    num_segments * segment_size, 
                                    "Expected size: %u; actual size: %u", 
                                    (unsigned)(num_segments * segment_size),
                                    (unsigned)(payload_len));

                // read the id recorded in this packet
                uint64_t id = read_u64(payload, ID_OFF);

                // query the timestamp based on the ID
                uint64_t timestamp = (get_client_req(client_requests, id))->timestamp_offset;

                uint64_t rtt = (cycles_offset(start_cycle_offset) - timestamp);
                add_latency_to_map(&packet_map, rtt, id);

                // free back the received packet
                mbuf_free(recv_pkts[i]);
                num_received += 1;
                outstanding--;
            }
        }
    }

    // dump the packets
    uint64_t total_exp_time = nanotime() - start_time_offset;
    size_t total_sent = (size_t)current_request->packet_id - 1;
    free(client_requests);
    calculate_and_dump_latencies(&packet_map,
                                    &latency_dist,
                                    total_sent,
                                    total_packets_per_request,
                                    total_exp_time,
                                    num_segments * segment_size,
                                    rate_distribution.rate_pps,
                                    has_latency_log,
                                    latency_log,
                                    1);

    cleanup_mlx5();
    return 0;
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

int flip_headers(struct mbuf *request, size_t outgoing_size) {
    unsigned char *ptr = request->data;
    struct eth_addr tmp_ether;
    uint32_t tmp = 0;
    uint16_t tmp_udp = 0;

    struct eth_hdr *eth  = (struct eth_hdr *)ptr;
    ether_addr_copy(&eth->dhost, &tmp_ether);
    ether_addr_copy(&eth->shost, &eth->dhost);
    ether_addr_copy(&tmp_ether, &eth->shost);

    struct ip_hdr *ipv4 = (struct ip_hdr *)(ptr + sizeof(struct eth_hdr));
    ipv4->chksum = 0;
    ipv4->len = htons(sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + outgoing_size);
    tmp = ipv4->daddr;
    ipv4->daddr = ipv4->saddr;
    ipv4->saddr = tmp;
    ipv4->chksum = get_chksum(ipv4);

    struct udp_hdr *udp = (struct udp_hdr *)(ptr + sizeof(struct ip_hdr) + sizeof(struct eth_hdr));
    tmp_udp = udp->dst_port;
    udp->dst_port = udp->src_port;
    udp->src_port = tmp_udp;
    udp->len = htons(sizeof(struct udp_hdr) + outgoing_size);
    udp->chksum = get_chksum(udp);

    if (request->head_len < (outgoing_size + (sizeof(RequestHeader)))) {
        // cannot set mbuf's outgoing size as new value
        NETPERF_WARN("Cannot set outgoing size to %lu, limit of %u", outgoing_size + sizeof(RequestHeader), request->head_len);
        return -EINVAL;
    }

    request->len = outgoing_size + sizeof(RequestHeader);
    return 0;
}

int post_mbufs(struct mbuf *send_mbufs[BATCH_SIZE][MAX_SCATTERS], size_t ct) {
    size_t total_sent = 0;
    uint64_t tries = 0;
    while (total_sent < ct) {
        size_t sent = mlx5_transmit_batch(send_mbufs,
                                        total_sent,
                                        ct,
                                        &txqs[0],
                                        request_header_ptrs,
                                        inline_lengths);
        tries += 1;
        total_sent += sent;
    }
#ifdef __TIMERS__
    add_latency(&server_tries_dist, tries);
#endif
    return 0;
}

int process_echo_requests(struct mbuf *requests[BATCH_SIZE], size_t ct) {
#ifdef __TIMERS__
    uint64_t start_construct = cycletime();
#endif
    struct mbuf *send_mbufs[BATCH_SIZE][MAX_SCATTERS];
    for (size_t pkt_idx = 0; pkt_idx < ct; pkt_idx++) {
        struct mbuf *request = requests[pkt_idx];
        request->nb_segs = 1;
        request->next = NULL;
        request->lkey = rx_mr->lkey;
        int ret = 0;
        ret = flip_headers(request, num_segments * segment_size);
        RETURN_ON_ERR(ret, "Failed to flip headers on mbuf");
        send_mbufs[pkt_idx][0] = request;
    }
#ifdef __TIMERS__
    uint64_t end_construct = cycletime();
    add_latency(&server_construction_dist, end_construct - start_construct);
    uint64_t start_send = cycletime();
#endif
    post_mbufs(send_mbufs, ct);
#ifdef __TIMERS__
    uint64_t end_send = cycletime();
    add_latency(&server_send_dist, end_send - start_send);
#endif
    return 0;
}

int process_server_requests(struct mbuf *requests[BATCH_SIZE], size_t ct) {
    assert(ct <= BATCH_SIZE);
#ifdef __TIMERS__
    uint64_t start_construct = cycletime();
#endif
    if (echo_mode == 1) {
        return process_echo_requests(requests, ct);
    }

    struct mbuf *send_mbufs[BATCH_SIZE][MAX_SCATTERS];
    uint64_t segments[MAX_SCATTERS];
    for (size_t pkt_idx = 0; pkt_idx < ct; pkt_idx++) {
        struct mbuf *request = requests[pkt_idx];
        char *payload = (char *)(mbuf_data(request)) + FULL_HEADER_SIZE;
        size_t payload_len = mbuf_length(request) - FULL_HEADER_SIZE;
        NETPERF_DEBUG("Payload len: %lu", payload_len);
        for (size_t seg = 0; seg < num_segments; seg++) {
            segments[seg] = read_u64(payload, seg + SEGLIST_OFFSET);
            NETPERF_DEBUG("Seg %lu: %lu", seg, segments[seg]);
        }

#ifdef __TIMERS__
        uint64_t start_busy = cycletime();
#endif
        // do busy work
        if (busy_iters > 0)  {
            busy_work_res = do_busy_work(busy_iters / 2);
        }
#ifdef __TIMERS__
        if (busy_iters > 0) {
            uint64_t end_busy = cycletime();
            add_latency(&busy_work_dist, end_busy - start_busy);
        }
#endif

        if (read_incoming_packet == 1) {
            size_t amt_to_add = 64 - (sizeof(OutgoingHeader)); // gets the next cache line after header
            // reads all of the data in the packet and creates a check sum
            checksum = calculate_checksum(payload, amt_to_add, payload_len);
            request_headers[pkt_idx].checksum = checksum;
        }
        // add 16 for (packet_id (4), id_padding (4), checksum (8))
        int ret = parse_outgoing_request_header(&request_headers[pkt_idx], request, num_segments * segment_size + 16);
        RETURN_ON_ERR(ret, "constructing outgoing header failed");

        if (zero_copy) {
            struct mbuf *prev = NULL;
            for (int seg = 0; seg < num_segments; seg++) {
                void *server_memory = get_server_region(server_working_set,
                                                            segments[seg],
                                                            segment_size);
                // allocate mbuf 
                send_mbufs[pkt_idx][seg] = (struct mbuf *)mempool_alloc(&mbuf_mempool);
                if (send_mbufs[pkt_idx][seg] == NULL) {
                    // free all previous allocated mbufs
                    for (size_t pkt = 0; pkt < pkt_idx - 1; pkt++) {
                        mbuf_free(send_mbufs[pkt][0]);
                    }
                    NETPERF_WARN("No buffers to send outgoing zero-copy packet");
                    return ENOMEM;
                }

                // set the next buffer pointer for previous mbuf
                if (prev != NULL) {
                    prev->next = send_mbufs[pkt_idx][seg];
                }
                prev = send_mbufs[pkt_idx][seg];

                // set metadata for this mbuf
                send_mbufs[pkt_idx][seg]->lkey = tx_mr->lkey;
                send_mbufs[pkt_idx][seg]->release = zero_copy_tx_completion;
                mbuf_init(send_mbufs[pkt_idx][seg], 
                            (unsigned char *)server_memory,
                            segment_size,
                            0);
                send_mbufs[pkt_idx][seg]->len = segment_size;
            }
            // for the first packet: set number of segments
            send_mbufs[pkt_idx][0]->nb_segs = num_segments;

            // for last packet: set next as null
            send_mbufs[pkt_idx][num_segments - 1]->next = NULL;
        } else {
            // single buffer for this packet
            send_mbufs[pkt_idx][0] = (struct mbuf *)mempool_alloc(&mbuf_mempool);
            if (send_mbufs[pkt_idx][0] == NULL) {
                // free all previous allocated mbufs
                for (size_t pkt = 0; pkt < pkt_idx - 1; pkt++) {
                    mbuf_free(send_mbufs[pkt][0]);
                }
                return ENOMEM;
            }
            send_mbufs[pkt_idx][0]->lkey = tx_mr->lkey;
            //NETPERF_INFO("Allocating mbuf %p", send_mbufs[pkt_idx][0]);
            // allocate backing buffer for this mbuf
            unsigned char *buffer = (unsigned char *)mempool_alloc(&tx_buf_mempool);
            if (buffer == NULL) {
                mbuf_free(send_mbufs[pkt_idx][0]);
                return ENOMEM;
            }
            //NETPERF_INFO("Allocating mbuf buffer %p", buffer);
            mbuf_init(send_mbufs[pkt_idx][0],
                        buffer,
                        DATA_MBUFS_SIZE,
                        0);
            // set release as non zero-copy release function
            send_mbufs[pkt_idx][0]->release = tx_completion;
            send_mbufs[pkt_idx][0]->next = NULL; // set the next packet as null

            for (int seg = 0; seg < num_segments; seg++) {
                void *server_memory = get_server_region(server_working_set, 
                                                            segments[seg],
                                                            segment_size);

                // TODO: handle case where header is NOT initialized in memory
                NETPERF_DEBUG("Copying into offset %u of mbuf", (unsigned)(seg * segment_size));
                mbuf_copy(send_mbufs[pkt_idx][0], 
                            (char *)server_memory,
                            segment_size,
                            seg * segment_size);
            }
            // for the first packet, set the number of segmens
            send_mbufs[pkt_idx][0]->nb_segs = 1;
        }

        // for the first packet, record the packet len
        send_mbufs[pkt_idx][0]->pkt_len = segment_size * num_segments;
    }

#ifdef __TIMERS__
    uint64_t end_construct = cycletime();
    add_latency(&server_construction_dist, end_construct - start_construct);
    uint64_t start_send = cycletime();
#endif

    // now send all these mbufs
    post_mbufs(send_mbufs, ct);
#ifdef __TIMERS__
    uint64_t end_send = cycletime();
    add_latency(&server_send_dist, end_send - start_send);
#endif
    return 0;
}
                            
int do_server() {
    NETPERF_DEBUG("Starting server program");
    struct mbuf *recv_mbufs[BURST_SIZE];
    struct mbuf *mbufs_to_process[BATCH_SIZE];
    size_t num_received = 0;
    while (1) {
        num_received = mlx5_gather_rx((struct mbuf **)&recv_mbufs, 
                                        BURST_SIZE,
                                        &rx_buf_mempool,
                                        &rxqs[0]);
        if (num_received > 0) {
#ifdef __TIMERS__
            add_latency(&server_burst_ct_dist, num_received);
#endif
            size_t rx_idx = 0;
            size_t batch_idx = 0;
            while (rx_idx < num_received) {
                struct mbuf *pkt = recv_mbufs[rx_idx];
                void *payload_out = NULL;
                uint32_t payload_len = 0;
                if (check_valid_packet(pkt, &payload_out, &payload_len, &server_mac) != 1) {
                    NETPERF_DEBUG("Received invalid pkt");
                    mbuf_free(pkt);
                    recv_mbufs[rx_idx] = NULL;
                    rx_idx += 1;
                    continue;
                }
                mbufs_to_process[batch_idx] = pkt;
                batch_idx += 1;
                rx_idx += 1;

                if (batch_idx == BATCH_SIZE || rx_idx == num_received) {
#ifdef __TIMERS__
                    uint64_t process_start = cycletime();
#endif
                    int ret = process_server_requests(mbufs_to_process, batch_idx);
                    if (ret != 0) {
                        // free the packets
                        for (size_t pkt_idx = 0; pkt_idx < batch_idx; pkt_idx++) {
                            mbuf_free(mbufs_to_process[pkt_idx]);
                            mbufs_to_process[pkt_idx] = NULL;
                        }
                        NETPERF_WARN("Error processing batch of packets with ct %lu", batch_idx);
                    }
                    if (echo_mode == 0) {
                        // if not echo mode, free processed packets
                        for (size_t pkt_idx = 0; pkt_idx < batch_idx; pkt_idx++) {
                            mbuf_free(mbufs_to_process[pkt_idx]);
                            mbufs_to_process[pkt_idx] = NULL;
                        }
                    }
#ifdef __TIMERS__
                    uint64_t process_end = cycletime();
                    add_latency(&server_request_dist, process_end - process_start);
#endif
                    batch_idx = 0;
                }
            }
        }
    }
    return 0;
}

// cleanup on the server-side
void sig_handler(int signo) {
    NETPERF_INFO("In request handler");
    // if debug timers were turned on, dump them
#ifdef __TIMERS__
    if (mode == UDP_SERVER) {
        NETPERF_INFO("----");
        NETPERF_INFO("server request processing timers: ");
        dump_debug_latencies(&server_request_dist, 1);
        NETPERF_INFO("----");
        NETPERF_INFO("server send processing timers: ");
        dump_debug_latencies(&server_send_dist, 1);
        NETPERF_INFO("----");
        NETPERF_INFO("----");
        NETPERF_INFO("server pkt construct processing timers: ");
        dump_debug_latencies(&server_construction_dist, 1);
        NETPERF_INFO("----");
        NETPERF_INFO("busy work timers: ");
        dump_debug_latencies(&busy_work_dist, 1);
        NETPERF_INFO("----");
        NETPERF_INFO("Server tries counts: ");
        dump_debug_latencies(&server_tries_dist, 0);
        NETPERF_INFO("----");
        NETPERF_INFO("Server receive burst ct: ");
        dump_debug_latencies(&server_burst_ct_dist, 0);
        NETPERF_INFO("----");
        free_latency_dist(&server_request_dist);
        free_latency_dist(&server_send_dist);
        free_latency_dist(&server_construction_dist);
        free_latency_dist(&busy_work_dist);
        free_latency_dist(&server_tries_dist);
        free_latency_dist(&server_burst_ct_dist);
    }
#endif
    cleanup_mlx5();
    fflush(stdout);
    fflush(stderr);
    exit(0);
    
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

    if (ret) {
        NETPERF_WARN("Time initialization failed.");
        return ret;
    }

    // Mlx5 queue and flow initialization
    ret = init_mlx5();
    if (ret) {
        NETPERF_WARN("init_mlx5() failed.");
        return ret;
    }
    NETPERF_INFO("Finished init_mlx5()");

    // initialize the workload
    ret = init_workload();
    if (ret) {
        NETPERF_WARN("Init of workload failed.");
        return ret;
    }

    if (mode == UDP_CLIENT) {
        ret = do_client();
        if (ret) {
            NETPERF_WARN("Client returned non-zero status.");
            return ret;
        }
    } else {
    // allocate all the latency dists
#ifdef __TIMERS__
        int alloc_ret = 0;
        alloc_ret = alloc_latency_dist(&server_request_dist, LATENCY_DIST_CT);
        if (alloc_ret != 0) {
            NETPERF_WARN("Tried to allocate too large of a latency dist: %lu", (size_t)LATENCY_DIST_CT);
        } 
        alloc_ret = alloc_latency_dist(&server_send_dist, LATENCY_DIST_CT);
        if (alloc_ret != 0) {
            NETPERF_WARN("Tried to allocate too large of a latency dist: %lu", (size_t)LATENCY_DIST_CT);
        } 
        alloc_ret = alloc_latency_dist(&server_construction_dist, LATENCY_DIST_CT);
        if (alloc_ret != 0) {
            NETPERF_WARN("Tried to allocate too large of a latency dist: %lu", (size_t)LATENCY_DIST_CT);
        } 
        alloc_ret = alloc_latency_dist(&busy_work_dist, LATENCY_DIST_CT);
        if (alloc_ret != 0) {
            NETPERF_WARN("Tried to allocate too large of a latency dist: %lu", (size_t)LATENCY_DIST_CT);
        } 
        alloc_ret = alloc_latency_dist(&server_tries_dist, LATENCY_DIST_CT);
        if (alloc_ret != 0) {
            NETPERF_WARN("Tried to allocate too large of a latency dist: %lu", (size_t)LATENCY_DIST_CT);
        } 
        alloc_ret = alloc_latency_dist(&server_burst_ct_dist, LATENCY_DIST_CT);
        if (alloc_ret != 0) {
            NETPERF_WARN("Tried to allocate too large of a latency dist: %lu", (size_t)LATENCY_DIST_CT);
        } 
#endif
        // set up signal handler
        if (signal(SIGINT, sig_handler) == SIG_ERR)
            printf("\ncan't catch SIGINT\n");

        // write ready file
        if (has_ready_file) {
            FILE *fptr;
            fptr = fopen(ready_file,"w");
            if (fptr == NULL) {
                printf("Error!");
                exit(1);    
            }
            fprintf(fptr,"ready\n");
            fclose(fptr);
        }
        return do_server();
    }

    return ret;
}


