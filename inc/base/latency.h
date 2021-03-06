/* 
 * Netperf Utilities for calculating packet latencies on the client side.
 * */
#pragma once
/****************************************************************/
// CONSTANTS
#define MAX_ITERATIONS 1000000
#define BATCH_SIZE 32
#define BURST_SIZE 32
#define MAX_SEGMENT_SIZE 8192
#define LATENCY_DIST_CT 720000000

#define rate_gbps(rate_pps, size) ((float)rate_pps * (float)(size) * 8.0 / (float)1e9)
/****************************************************************/

typedef struct Packet_Map_t
{
    uint64_t rtts[MAX_ITERATIONS];
    uint64_t ids[MAX_ITERATIONS]; // unique IDs sent in each packet
    size_t total_count;
    uint32_t *sent_ids;
    uint64_t *grouped_rtts;
} Packet_Map_t;

typedef struct Latency_Dist_t
{
    uint64_t min, max;
    uint64_t latency_sum;
    uint64_t total_count;
    float moving_avg;
    uint64_t *latencies;
    size_t allocated;
} Latency_Dist_t;

int alloc_latency_dist(Latency_Dist_t *dist, size_t ct);

void free_latency_dist(Latency_Dist_t *dist);

int calculate_total_packets_required(uint32_t seg_size, uint32_t nb_segs);

int calculate_and_dump_latencies(Packet_Map_t *packet_map, 
                                    Latency_Dist_t *latency_dist, 
                                    size_t num_sent, 
                                    int num_packets_per_request,
                                    uint64_t total_time,
                                    size_t packet_size,
                                    size_t rate_pps,
                                    int has_latency_log,
                                    char *latency_log,
                                    int in_cycles);

int cmpfunc(const void *a, const void *b);

int add_latency_to_map(Packet_Map_t *map, uint64_t rtt, uint32_t id);

void free_pktmap(Packet_Map_t *map);

int calculate_latencies(Packet_Map_t *map, 
                            Latency_Dist_t *dist, 
                            size_t num_sent, 
                            size_t num_per_bucket);

int add_latency(Latency_Dist_t *dist, uint64_t latency);

uint64_t display(uint64_t num, int in_cycles);

int dump_debug_latencies(Latency_Dist_t *dist,
                        int in_cycles);

int dump_latencies(Latency_Dist_t *dist,
                        uint64_t total_time,
                        size_t message_size,
                        float rate_gbps,
                        int has_latency_log,
                        char *latency_log,
                        int in_cycles);

