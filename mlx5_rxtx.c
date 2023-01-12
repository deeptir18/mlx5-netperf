#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <net/ip.h>
#include <net/udp.h>
#include <net/ethernet.h>

#include <asm/ops.h>
#include <base/debug.h>
#include <base/time.h>
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

/* Atomically change the reference count */
uint16_t server_change_refcnt(unsigned long index, int16_t change) {
    // index = "virtual segment"
    // gets broken down into refcnt_array + physical segment offset
    size_t physical_segment = index % num_total_segments;
    size_t refcnt_array = index / num_total_segments;
    //NETPERF_DEBUG("Virtual seg %lu, physical seg %lu, refcnt array %lu, num total segments %lu\n", index, physical_segment, refcnt_array, num_total_segments);
    if (change > 0) {
        uint16_t cur = 0;
        cur +=  __atomic_add_fetch(&working_set_refcnts[refcnt_array][physical_segment], (uint16_t)change, __ATOMIC_ACQ_REL);
        return cur;
    } else {
        uint16_t cur = 0;
        cur -= __atomic_sub_fetch(&working_set_refcnts[refcnt_array][physical_segment], (uint16_t)(change * -1), __ATOMIC_ACQ_REL);
        return cur;
    }
}



int mlx5_gather_completions(struct mbuf **mbufs, 
                            struct mlx5_txq *v, 
                            unsigned int budget)
{
    NETPERF_DEBUG("Calling gather completions");
	struct mlx5dv_cq *cq = &v->tx_cq_dv;
	struct mlx5_cqe64 *cqe, *cqes = cq->buf;

	unsigned int compl_cnt;
	uint8_t opcode;
	uint16_t wqe_idx;

	for (compl_cnt = 0; compl_cnt < budget; compl_cnt++, v->cq_head++) {
        NETPERF_DEBUG("Completion on cqe: %u, true_cq_head: %u, true idx: %u", v->cq_head, v->true_cq_head, v->cq_head & (cq->cqe_cnt - 1));
		cqe = &cqes[v->cq_head & (cq->cqe_cnt - 1)];
		opcode = cqe_status(cqe, cq->cqe_cnt, v->cq_head);
        NETPERF_DEBUG("parity: %u", v->cq_head & cq->cqe_cnt);


		if (opcode == MLX5_CQE_INVALID) {
            NETPERF_DEBUG("Invalid cqe for cqe %u", v->cq_head);
			break;
        }

		PANIC_ON_TRUE(opcode != MLX5_CQE_REQ, "wrong opcode, cqe format: %d, equals 0x3: %d, opcode: %d, wqe counter: %d, syndrome: %d", mlx5_get_cqe_format(cqe), mlx5_get_cqe_format(cqe) == 0x3, mlx5_get_cqe_opcode(cqe), be16toh(cqe->wqe_counter), get_error_syndrome(cqe));
		PANIC_ON_TRUE(mlx5_get_cqe_format(cqe) == 0x3, "cq->cqe_cnt wrong cqe format");

		wqe_idx = be16toh(cqe->wqe_counter) & (v->tx_qp_dv.sq.wqe_cnt - 1);
        NETPERF_DEBUG("Completion on wqe idx: %u; unwrapped cqe wqe idx: %u, wqe ct: %u, cqe cnt: %u", wqe_idx, be16toh(cqe->wqe_counter), v->tx_qp_dv.sq.wqe_cnt, cq->cqe_cnt);
		mbufs[compl_cnt] = load_acquire(&v->buffers[wqe_idx]);
        v->true_cq_head += mbufs[compl_cnt]->num_wqes;
        // pad by actual number of wqes taken by this send
	}

    NETPERF_DEBUG("after processing completions, cq_head is : %u, sq_head is: %u, true cq head: %u", v->cq_head, v->sq_head, v->true_cq_head);
	cq->dbrec[0] = htobe32(v->cq_head & 0xffffff);

	return compl_cnt;
}

int mlx5_inline_data(struct mlx5_txq *v,
                        size_t offset,
                        char *data,
                        size_t inline_len) {
    uint32_t current_idx = POW2MOD(v->sq_head, v->tx_qp_dv.sq.wqe_cnt);
    NETPERF_DEBUG("Current post number: %u, size: %u, wrapped: %u, and: %u", v->sq_head, v->tx_qp_dv.sq.wqe_cnt, current_idx, v->sq_head & (v->tx_qp_dv.sq.wqe_cnt - 1));
    char *current_segment_ptr  = get_segment(v, current_idx);
    char *inline_start = current_segment_ptr + sizeof(struct mlx5_wqe_ctrl_seg) + offsetof(struct mlx5_wqe_eth_seg, inline_hdr_start);
    char *end_ptr = v->tx_qp_dv.sq.buf + v->tx_qp_dv.sq.wqe_cnt * v->tx_qp_dv.sq.stride;
    char *inline_off_start = inline_start + offset;
    if (inline_off_start >= end_ptr) {
        size_t end_chunk = inline_len - (end_ptr - inline_start);
        inline_off_start = v->tx_qp_dv.sq.buf + end_chunk;
    }
    NETPERF_DEBUG("Inlining at offset %lu, inline len %lu; inline off start is %p, inline_off_start", offset, inline_len, inline_off_start);

    // do memcpy with wrap around if necessary
    if ((end_ptr - inline_off_start) < inline_len) {
        size_t first_chunk = end_ptr - inline_off_start;
        size_t second_chunk = inline_len - first_chunk;
        memcpy(inline_off_start, data, first_chunk);
        memcpy(v->tx_qp_dv.sq.buf, data + first_chunk, second_chunk);
    } else {
        memcpy(inline_off_start, data, inline_len);
    }
    return 0;
}



int mlx5_fill_tx_segment(struct mlx5_txq *v,
                            struct mbuf *m,
                            RequestHeader *request_header,
                            size_t inline_len) {
    
    int i, compl = 0;
	struct mbuf *mbs[SQ_CLEAN_MAX];
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_eth_seg *eseg;
	struct mlx5_wqe_data_seg *dpseg;
    void *current_segment_ptr;
    void *end_ptr = v->tx_qp_dv.sq.buf + v->tx_qp_dv.sq.wqe_cnt * v->tx_qp_dv.sq.stride;
    
    // for now: we only allow inlining the entire request header, or nothing
    //NETPERF_ASSERT((inline_len == sizeof(RequestHeader) && request_header != NULL) || (request_header == NULL && inline_len == 0), "Provided inline_len not large enough: %lu, expected >= %lu", inline_len, sizeof(RequestHeader));

    // calculate the number of 16 byte segments required for the work request
    // (qpn_ds)
    // 1 for control segment
    // 1 for ethernet segment until inline header
    // (inline_len + 15) / 16 pads inline_len to the next multiple of 16 before
    // dividing
    // inline data starts two bytes before `eth_seg->inline_hdr` (at
    // inline_hdr_start)
    NETPERF_DEBUG("Ctrl size: %lu, eth size: %lu, inline size: %lu, num mbufs: %d", sizeof(*ctrl), offsetof(struct mlx5_wqe_eth_seg, inline_hdr), inline_len, m->nb_segs);
    int num_hdr_segs = 2;
    if (inline_len > 0) {
        num_hdr_segs  = sizeof(*ctrl) / 16 +
                        (offsetof(struct mlx5_wqe_eth_seg, inline_hdr)) / 16 +
                        ((inline_len - 2) + 15) / 16;
    }
    int num_dpsegs = (sizeof(*dpseg) * m->nb_segs) / 16;
    // number of work requests must be padded to 4 16-byte segments
    int num_wqes = (num_hdr_segs + num_dpsegs + 3) / 4;

    NETPERF_DEBUG("Num octowords for req: %d, num wqes: %d, in flight: %u, total: %u, free: %u", num_hdr_segs + num_dpsegs, num_wqes, nr_inflight_tx(v), v->tx_qp_dv.sq.wqe_cnt, v->tx_qp_dv.sq.wqe_cnt - nr_inflight_tx(v));

    // check if there are enough wqes: if not: check for completions
    if (unlikely((v->tx_qp_dv.sq.wqe_cnt - nr_inflight_tx(v)) < num_wqes)) {
		compl = mlx5_gather_completions(mbs, v, SQ_CLEAN_MAX);
		for (i = 0; i < compl; i++) {
			mbuf_free(mbs[i]);
        }
		if (unlikely((v->tx_qp_dv.sq.wqe_cnt - nr_inflight_tx(v)) < num_wqes)) {
            NETPERF_DEBUG("txq still full after checking for completions: inflight %u, ct %u, sq: %u, true cq: %u", nr_inflight_tx(v), v->tx_qp_dv.sq.wqe_cnt, v->sq_head, v->true_cq_head);
			return ENOMEM;
		}
    }

    // calculate number of contiguous wqes (before the end of the ringe buffer)
    uint32_t current_idx = POW2MOD(v->sq_head, v->tx_qp_dv.sq.wqe_cnt);
    NETPERF_DEBUG("Current post number: %u, size: %u, wrapped: %u, and: %u", v->sq_head, v->tx_qp_dv.sq.wqe_cnt, current_idx, v->sq_head & (v->tx_qp_dv.sq.wqe_cnt - 1));
    current_segment_ptr  = get_segment(v, current_idx);
    NETPERF_DEBUG("Ctrl seg addr: %p", current_segment_ptr);
    
    // fill in the control segment
    ctrl = current_segment_ptr;
    *(uint32_t *)(current_segment_ptr + 8) = 0;
    ctrl->imm = 0;
    ctrl->fm_ce_se = MLX5_WQE_CTRL_CQ_UPDATE;
    ctrl->qpn_ds = htobe32((num_hdr_segs + num_dpsegs) | (v->tx_qp->qp_num << 8) );
	ctrl->opmod_idx_opcode = htobe32(((v->sq_head & 0xffff) << 8) |
					       MLX5_OPCODE_SEND);
    current_segment_ptr += sizeof(*ctrl);


    // fill in the ethernet segment
    eseg = current_segment_ptr;
    memset(eseg, 0, sizeof(struct mlx5_wqe_eth_seg));
    eseg->cs_flags |= MLX5_ETH_WQE_L3_CSUM | MLX5_ETH_WQE_L4_CSUM;
    eseg->inline_hdr_sz = htobe16(inline_len);
    if (inline_len > 0) {
        current_segment_ptr += offsetof(struct mlx5_wqe_eth_seg, inline_hdr_start);
        NETPERF_DEBUG("Inline addr: %p; inline len: %lu", current_segment_ptr, inline_len);
        size_t offset = 0;
        mlx5_inline_data(v, offset, (char *)(&(request_header->packet_header.eth)), sizeof(struct eth_hdr));
        offset += sizeof(struct eth_hdr);
        mlx5_inline_data(v, offset, (char *)(&(request_header->packet_header.ipv4)), sizeof(struct ip_hdr));
        offset += sizeof(struct ip_hdr);
        mlx5_inline_data(v, offset, (char *)(&(request_header->packet_header.udp)), sizeof(struct udp_hdr));
        offset += sizeof(struct udp_hdr);
        mlx5_inline_data(v, offset, (char *)(&(request_header->packet_id)), sizeof(uint64_t));
        offset += sizeof(uint64_t);
        mlx5_inline_data(v, offset, (char *)(&(request_header->checksum)), sizeof(uint64_t));
        // Pad current segment pointer based on wrap around behavior
        if ((end_ptr - current_segment_ptr) < inline_len) {
            NETPERF_DEBUG("In case where inline_len %lu greater than space left %lu", inline_len, (end_ptr - current_segment_ptr));
            size_t first_chunk_size = end_ptr - current_segment_ptr;
            current_segment_ptr = v->tx_qp_dv.sq.buf;
            current_segment_ptr += (inline_len - first_chunk_size + 15) & ~0xf;
        } else {
            current_segment_ptr += 2;
            current_segment_ptr += (inline_len - 2 + 15) & ~0xf;
            NETPERF_DEBUG("Dpseg addr: %p", current_segment_ptr);
        }
    } else {
        current_segment_ptr += 16;
        if (current_segment_ptr == end_ptr) {
            NETPERF_DEBUG("Reaching case where next dpseg wraps around");
            current_segment_ptr = v->tx_qp_dv.sq.buf;
        }
#ifdef __DEBUG__
        unsigned char *data_addr = mbuf_data(m);
        struct eth_hdr *eth = (struct eth_hdr *)data_addr;
        struct ip_hdr *ipv4 = (struct ip_hdr *)(data_addr + sizeof(struct eth_hdr));
        struct udp_hdr *udp = (struct udp_hdr *)(data_addr + sizeof(struct eth_hdr) + sizeof(struct ip_hdr));
        //uint64_t *id_ptr = (uint64_t *)(data_addr + sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr));
        //uint64_t *ts_ptr = (uint64_t *)(data_addr + sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + sizeof(uint64_t));
        print_individual_headers(eth, ipv4, udp);
#endif
    }
    struct mbuf *curr = m;
    m->num_wqes = num_wqes;
    uint32_t dpseg_ct = 0;
    while (curr != NULL) {
        dpseg = current_segment_ptr;
        // lkey already set during initialization
        NETPERF_DEBUG("Adding dpseg with length: %u, addr %p, lkey %u", mbuf_length(curr), mbuf_data(curr), curr->lkey);
	    dpseg->byte_count = htobe32(mbuf_length(curr));
	    dpseg->addr = htobe64((uint64_t)mbuf_data(curr));
        dpseg->lkey = htobe32(curr->lkey);
        curr = curr->next;
        // go to next segment ptr and roll over
        current_segment_ptr += sizeof(*dpseg);
        if (current_segment_ptr == end_ptr) {
            NETPERF_DEBUG("Reaching case where next dpseg wraps around");
            current_segment_ptr = v->tx_qp_dv.sq.buf;
        }
        dpseg_ct++;
    }

    /* record buffer for completion queue */
    store_release(&v->buffers[current_idx], m);
    v->sq_head += num_wqes;
    NETPERF_DEBUG("Incrementing sq_head to %u, wrapped: %u", v->sq_head, POW2MOD(v->sq_head, v->tx_qp_dv.sq.wqe_cnt));

    return 0; 
}

void mlx5_ring_doorbell(struct mlx5_txq *v, struct mlx5_wqe_ctrl_seg *ctrl) {
    NETPERF_DEBUG("Ringing doorbell for ctrl %p", ctrl);
	/* write doorbell record */
	udma_to_device_barrier();
    // post the next new wqe to doorbell
	v->tx_qp_dv.dbrec[MLX5_SND_DBR] = htobe32(v->sq_head & 0xffff);

    NETPERF_DEBUG("blueflame register size: %u", v->tx_qp_dv.bf.size);
	/* ring bf doorbell */
	mmio_wc_start();
	mmio_write64_be(v->tx_qp_dv.bf.reg, *(__be64 *)ctrl);
	mmio_flush_writes();
}

struct mlx5_wqe_ctrl_seg  *mlx5_post_transmission(struct mbuf *m,
                                                    struct mlx5_txq *v,
                                                    RequestHeader *request_header,
                                                    size_t inline_len) {
	uint32_t idx = v->sq_head & (v->tx_qp_dv.sq.wqe_cnt - 1);
	struct mlx5_wqe_ctrl_seg *ctrl =  get_segment(v, idx);

    int ret = mlx5_fill_tx_segment(v, m, request_header, inline_len);
    if (ret == ENOMEM) {
        NETPERF_DEBUG("from filling in segment: could not construct segment: txq full");
        return NULL;
    }

    return ctrl;
}

int mlx5_transmit_one(struct mbuf *m, struct mlx5_txq *v, RequestHeader *request_header, size_t inline_len)
{
    struct mlx5_wqe_ctrl_seg *ctrl = mlx5_post_transmission(m, v, request_header, inline_len);
    if (ctrl == NULL) {
        return 0;
    }
    
    mlx5_ring_doorbell(v, ctrl);
    
    /* check for completions */
	if (nr_inflight_tx(v) >= SQ_CLEAN_THRESH) {
	    int i, compl = 0;
	    struct mbuf *mbs[SQ_CLEAN_MAX];
		compl = mlx5_gather_completions(mbs, v, SQ_CLEAN_MAX);
		for (i = 0; i < compl; i++)
			mbuf_free(mbs[i]);
	}

    return 1;
}

int mlx5_gather_rx(struct mbuf **ms, 
                    unsigned int budget, 
                    struct mempool *rx_buf_mempool,
                    struct mlx5_rxq *v)
{
	uint8_t opcode;
	uint16_t wqe_idx;
	int rx_cnt;

	struct mlx5dv_rwq *wq = &v->rx_wq_dv;
	struct mlx5dv_cq *cq = &v->rx_cq_dv;

	struct mlx5_cqe64 *cqe, *cqes = cq->buf;
	struct mbuf *m;

	for (rx_cnt = 0; rx_cnt < budget; rx_cnt++, v->consumer_idx++) {
		cqe = &cqes[v->consumer_idx & (cq->cqe_cnt - 1)];
		opcode = cqe_status(cqe, cq->cqe_cnt, v->consumer_idx);

		if (opcode == MLX5_CQE_INVALID) {
			break;
        }

		if (unlikely(opcode != MLX5_CQE_RESP_SEND)) {
            NETPERF_PANIC("got opcode %02X", opcode);
            exit(1);
		}

        // TODO: some statistics thing we should add in later
		total_dropped += be32toh(cqe->sop_drop_qpn) >> 24;

		PANIC_ON_TRUE(mlx5_get_cqe_format(cqe) == 0x3, "not compressed"); // not compressed
		wqe_idx = be16toh(cqe->wqe_counter) & (wq->wqe_cnt - 1);
		m = v->buffers[wqe_idx];
		mbuf_fill_cqe(m, cqe);
		ms[rx_cnt] = m;
	}

	if (unlikely(!rx_cnt))
		return rx_cnt;

	cq->dbrec[0] = htobe32(v->consumer_idx & 0xffffff);
	PANIC_ON_TRUE(mlx5_refill_rxqueue(v, rx_cnt, rx_buf_mempool), "failed to refill rx queue");
	return rx_cnt;
}

int mlx5_transmit_batch(struct mbuf *mbufs[BATCH_SIZE][MAX_SCATTERS],
                        size_t start_index,
                        size_t burst_size,
                        struct mlx5_txq *v,
                        RequestHeader *request_headers[BATCH_SIZE],
                        size_t inline_len[BATCH_SIZE])
{
    for (size_t i = start_index; i < burst_size; i++) {
        struct mbuf *mbuf = mbufs[i][0];
        int ret = mlx5_transmit_one(mbuf, v, request_headers[i], inline_len[i]);
        if (ret != 1) {
            NETPERF_DEBUG("Could not transmit one mbuf");
            return i - start_index;
        }
    }
    return burst_size - start_index;

    struct mlx5_wqe_ctrl_seg *curr_ctrl = NULL;

    for (size_t i = start_index; i < burst_size; i++) {
        struct mbuf *mbuf = mbufs[i][0];
        // post this to the queue
        struct mlx5_wqe_ctrl_seg *ctrl = mlx5_post_transmission(mbuf, v, request_headers[i], inline_len[i]);
        if (ctrl == NULL) {
            // if there is something to be posted, post, then check for
            // completions
            if (curr_ctrl != NULL) {
                mlx5_ring_doorbell(v, curr_ctrl);
                curr_ctrl = NULL;
            }
	        /*if (nr_inflight_tx(v) >= SQ_CLEAN_THRESH) {
	            int j, compl = 0;
	            struct mbuf *mbs[SQ_CLEAN_MAX];
		        compl = mlx5_gather_completions(mbs, v, SQ_CLEAN_MAX);
		        for (j = 0; j < compl; j++)
			        mbuf_free(mbs[j]);
	        }*/
            return (i - start_index);
        }
        if (curr_ctrl == NULL) {
            curr_ctrl = ctrl;
        }
    }

    // TODO: shouldn't this check not be necessary?
    if (curr_ctrl != NULL) {
        mlx5_ring_doorbell(v, curr_ctrl);
    } else {
        NETPERF_WARN("In processing, curr ctrl was none");
    }

    /* check for completions */
	if (nr_inflight_tx(v) >= SQ_CLEAN_THRESH) {
	    int j, compl = 0;
	    struct mbuf *mbs[SQ_CLEAN_MAX];
		compl = mlx5_gather_completions(mbs, v, SQ_CLEAN_MAX);
		for (j = 0; j < compl; j++)
			mbuf_free(mbs[j]);
	}
    return (burst_size - start_index);
}
