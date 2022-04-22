#include <math.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <limits.h>
#include <ctype.h>
#include "fcntl.h"
#include <sys/types.h>
#include <unistd.h>
#include <math.h>

#include <base/latency.h>
#include <base/debug.h>
#include <base/time.h>

#define PERCENT_TO_CUT 0.00

int alloc_latency_dist(Latency_Dist_t *dist, size_t ct) {
    uint64_t *arr = (uint64_t *)(malloc(sizeof(uint64_t) * ct));
    if (arr == NULL) {
        return -ENOMEM;
    }
    dist->latencies = arr;
    dist->allocated = ct;
    return 0;
}

void free_latency_dist(Latency_Dist_t *dist) {
    free(dist->latencies);
}

int calculate_total_packets_required(uint32_t seg_size, uint32_t nb_segs) {
    if (seg_size > MAX_SEGMENT_SIZE) {
        if (nb_segs != 1) {
            printf("Cannot handle if segment size > MAX and nb_segs != 1.\n");
            exit(1);
        }
        int total_pkts_required = (int)(ceil((double)seg_size / (double)MAX_SEGMENT_SIZE));
        return total_pkts_required;
    } else {
        uint32_t pkts_per_segment = (uint32_t)MAX_SEGMENT_SIZE / seg_size;
        int total_pkts_required = (int)(ceil((double)nb_segs / (double)pkts_per_segment));
        return total_pkts_required;
    }
}

int calculate_and_dump_latencies(Packet_Map_t *packet_map,
                                    Latency_Dist_t *latency_dist,
                                    size_t num_sent,
                                    int num_packets_per_request,
                                    uint64_t total_time,
                                    size_t packet_size,
                                    size_t rate_pps,
                                    int has_latency_log,
                                    char *latency_log,
                                    int in_cycles)
{
    int ret = 0;
    float total_time_float = (float)total_time / ((float)ONE_SECOND * 1000);
    printf("Ran for %f seconds, sent %lu packets.\n", total_time_float, num_sent);
    ret = calculate_latencies(packet_map, 
                                latency_dist, 
                                num_sent, 
                                num_packets_per_request);
    if (ret) {
        NETPERF_WARN("Failed to calclate latencies");
        return ret;
    }

    ret = dump_latencies(latency_dist, 
                            total_time, 
                            packet_size, 
                            rate_gbps(rate_pps, packet_size),
                            has_latency_log,
                            latency_log,
                            in_cycles);
    if (ret) {
        NETPERF_WARN("Failed to dump latencies");
        return ret;
    }
    free_pktmap(packet_map);
    free_latency_dist(latency_dist);
    // flush all io
    fflush(stdout);
    return ret;
}

int add_latency_to_map(Packet_Map_t *map, uint64_t rtt, uint32_t id) {
    map->rtts[map->total_count] = rtt;
    map->ids[map->total_count] = id;
    map->total_count++;
    if (map->total_count > (MAX_ITERATIONS * 32)) {
        NETPERF_WARN("Overflow in packet map");
        return -ENOMEM;
    }
    return 0;
}

void free_pktmap(Packet_Map_t *map) {
    if (map->sent_ids != NULL) {
        free(map->sent_ids);
    }

    if (map->grouped_rtts != NULL) {
        free(map->grouped_rtts);
    }
}

int calculate_latencies(Packet_Map_t *map, Latency_Dist_t *dist, size_t num_sent, size_t num_per_bucket) {
    int ret = 0;
    ret = alloc_latency_dist(dist, num_sent);
    if (ret != 0) {
        return ret;
    }
    map->sent_ids = (uint32_t *)(malloc(sizeof(uint32_t) * num_sent));
    map->grouped_rtts = (uint64_t *)(malloc(sizeof(uint64_t) * num_sent * num_per_bucket));
    if (map->grouped_rtts == NULL || map->sent_ids == NULL) {
        NETPERF_WARN("Error allocating grouped rtts and sent_ids");
        return -ENOMEM;
    }

    for (size_t r = 0; r < num_sent; r++) {
        map->sent_ids[r] = 0;
        for (size_t c = 0; c < num_per_bucket; c++) {
            *(map->grouped_rtts + r*num_per_bucket + c) = 0;
        }
    }

    for (size_t i = 0;  i < map->total_count; i++) {
        uint32_t id = map->ids[i];
        uint64_t rtt = map->rtts[i];
        map->sent_ids[id] = 1;
        for (size_t c = 0; c < num_per_bucket; c++) {
            size_t r = (size_t)id;
            if (*(map->grouped_rtts + r*num_per_bucket + c) == 0) {
                *(map->grouped_rtts + r*num_per_bucket + c) = rtt;
                break;
            }
        }
    }

    int num_valid = 0;
    for (size_t r = 0; r < num_sent; r++) {
        if (map->sent_ids[r] == 0) {
            continue;
        }
        uint64_t max_rtt = 0;
        for (size_t c = 0; c < num_per_bucket; c++) {
            uint64_t rtt = *(map->grouped_rtts + r*num_per_bucket + c);
            if (rtt != 0) {
                if (rtt > max_rtt) {
                    max_rtt = rtt;
                }
            }
        }
        if (max_rtt > 0) {
            num_valid++;
            add_latency(dist, max_rtt);
        }
    }
    printf("Num fully received: %d, packets per bucket: %u, total_count: %u.\n", num_valid, (unsigned)num_per_bucket, (unsigned)map->total_count);
    return 0;
}

int add_latency(Latency_Dist_t *dist, uint64_t latency) {
    dist->latencies[dist->total_count] = latency;
    dist->total_count++;
    if (dist->total_count > dist->allocated) {
        NETPERF_WARN("Overflow in latency_dist");
        return -ENOMEM;
    }
    dist->latency_sum += latency;
    if (latency < dist->min) {
        dist->min = latency;
    }

    if (latency > dist->max) {
        dist->max = latency;
    }

    // calculate moving avg
    dist->moving_avg = dist->moving_avg * ((float)(dist->total_count - 1)/(float)dist->total_count) + ((float)(latency) / (float)(dist->total_count));
    return 0;
}

int cmpfunc(const void * a, const void *b) {
    const uint64_t *a_ptr = (const uint64_t *)a;
    const uint64_t *b_ptr = (const uint64_t *)b;
    return (int)(*a_ptr - *b_ptr);
}

uint64_t display(uint64_t num, int in_cycles) {
    if (in_cycles == 0) {
        return num;
    } else {
        return cycles_to_ns(num);
    }
}

int dump_debug_latencies(Latency_Dist_t *dist,
                            int in_cycles) {
    if (dist->total_count == 0) {
        return 0;
    }
    size_t num_sorted_latencies = (size_t)((1.0 - PERCENT_TO_CUT) * (float)dist->total_count);
    size_t cutoff = dist->total_count - num_sorted_latencies;
    uint64_t *arr = malloc(num_sorted_latencies * sizeof(uint64_t));
    size_t latency_sum = dist->latency_sum;
    if (arr == NULL) {
        NETPERF_DEBUG("Not able to allocate array to sort latencies\n");
        exit(1);
    }
    for (size_t i = 0; i < dist->total_count; i++) {
        if (i < cutoff) {
            latency_sum -= dist->latencies[i];
        } else {
            arr[i - cutoff] = dist->latencies[i];
        }
    }
    
    qsort(arr, num_sorted_latencies, sizeof(uint64_t), cmpfunc);
    uint64_t avg_latency = display((latency_sum) / (num_sorted_latencies), in_cycles);
    NETPERF_INFO("Median index: %lu, p99 index: %lu, p999: %lu, total: %lu", (size_t)((float)num_sorted_latencies * 0.50), (size_t)((float)num_sorted_latencies * 0.99), (size_t)((float)num_sorted_latencies * 0.999), num_sorted_latencies);
    uint64_t median = display(arr[(size_t)((float)num_sorted_latencies * 0.50)], in_cycles);
    uint64_t p99 = display(arr[(size_t)((float)num_sorted_latencies * 0.99)], in_cycles);
    uint64_t p999 = display(arr[(size_t)((float)num_sorted_latencies * 0.999)], in_cycles);
    printf("Stats:\n\t- Min latency: %lu ns\n\t- Max latency: %lu ns\n\t- Avg latency: %lu ns", display(dist->min, in_cycles), display(dist->max, in_cycles), avg_latency);
    printf("\n\t- Median latency: %lu ns\n\t- p99 latency: %lu ns\n\t- p999 latency: %lu ns\n", median, p99, p999);
    free(arr);
    return 0;
}

int dump_latencies(Latency_Dist_t *dist, 
                        uint64_t total_time, 
                        size_t message_size, 
                        float rate_gbps,
                        int has_latency_log,
                        char *latency_log,
                        int in_cycles) {
    NETPERF_DEBUG("Trying to dump latencies on client side with message size: %u\n", (unsigned)message_size);
    // sort the latencies
    if (dist->total_count == 0) {
        NETPERF_WARN("No latencies to dump");
        return -EINVAL;
    }
    size_t num_sorted_latencies = (size_t)((1.0 - PERCENT_TO_CUT) * (float)dist->total_count);
    size_t cutoff = dist->total_count - num_sorted_latencies;
    uint64_t *arr = malloc(num_sorted_latencies * sizeof(uint64_t));
    size_t latency_sum = dist->latency_sum;
    if (arr == NULL) {
        NETPERF_DEBUG("Not able to allocate array to sort latencies\n");
        exit(1);
    }
    for (size_t i = 0; i < dist->total_count; i++) {
        if (i < cutoff) {
            latency_sum -= dist->latencies[i];
        } else {
            arr[i - cutoff] = dist->latencies[i];
        }
    }
    
    qsort(arr, num_sorted_latencies, sizeof(uint64_t), cmpfunc);
    uint64_t avg_latency = display((latency_sum) / (num_sorted_latencies), in_cycles);
    NETPERF_INFO("Median index: %lu, p99 index: %lu, p999: %lu", (size_t)((float)num_sorted_latencies * 0.50), (size_t)((float)num_sorted_latencies * 0.99), (size_t)((float)num_sorted_latencies * 0.999));
    uint64_t median = display(arr[(size_t)((float)num_sorted_latencies * 0.50)], in_cycles);
    uint64_t p99 = display(arr[(size_t)((float)num_sorted_latencies * 0.99)], in_cycles);
    uint64_t p999 = display(arr[(size_t)((float)num_sorted_latencies * 0.999)], in_cycles);
    
    NETPERF_INFO("total ct: %lu, total_time: %lu, message_size: %lu", dist->total_count, total_time, message_size);
    float achieved_rate_pps = (float)(dist->total_count) / ((float)total_time / (float)1e9);
    float achieved_rate = rate_gbps(achieved_rate_pps, message_size);
    float percent_rate = achieved_rate / rate_gbps;
    printf("Stats:\n\t- Min latency: %lu ns\n\t- Max latency: %lu ns\n\t- Avg latency: %lu ns", display(dist->min, in_cycles), display(dist->max, in_cycles), avg_latency);
    printf("\n\t- Median latency: %lu ns\n\t- p99 latency: %lu ns\n\t- p999 latency: %lu ns", median, p99, p999);
    printf("\n\t- Achieved Goodput: %0.4f Gbps ( %0.4f %% ) \n", achieved_rate, percent_rate);
    
    if (has_latency_log) {
        FILE *fp = fopen(latency_log, "w");
        size_t ct = 0;
        for (int i = 0; i < dist->total_count; i++) {
            fprintf(fp, "%lu,%lu\n", ct, display(dist->latencies[i], in_cycles));
            ct++;
        }
        fclose(fp);
        free(latency_log);
    }
    free((void *)arr);
    NETPERF_INFO("Finished reporting function");
    return 0;
}
