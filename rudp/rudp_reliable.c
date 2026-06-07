#include "rudp_reliable.h"
#include "fec.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <poll.h>

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void rudp_sender_init(struct rudp_sender *s, int sockfd)
{
    memset(s, 0, sizeof(*s));
    s->sockfd = sockfd;
    s->next_seq = 1;
    s->rto_ms = INITIAL_RTO_MS;
}

void rudp_receiver_init(struct rudp_receiver *r, int sockfd)
{
    memset(r, 0, sizeof(*r));
    r->sockfd = sockfd;
    r->next_expected = 1;
}

void rudp_sender_set_fec(struct rudp_sender *s, int k, int p)
{
    if (k < 1 || k > FEC_MAX_K) return;
    s->fec_k = k;
    s->fec_p = p;
    s->use_fec = 1;
    s->fec_block_pos = 0;
    s->fec_block_start = 0;
    s->fec_block_max_len = 0;
    s->fec_block_parity_pending = 0;
    s->fec_block_parity_pkt_len = 0;
    s->fec_block_parity_retx = 0;
}

void rudp_receiver_set_fec(struct rudp_receiver *r, int k, int p)
{
    if (k < 1 || k > FEC_MAX_K) return;
    r->fec_k = k;
    r->fec_p = p;
    r->use_fec = 1;
    r->fec_block_start = 0;
    r->fec_block_parity_present = 0;
    r->fec_block_parity_len = 0;
}

void rudp_sender_set_fec_v2(struct rudp_sender *s, int k, int p)
{
    if (k < 1 || k > FEC_MAX_K) return;
    s->fec_k = k;
    s->fec_p = p;
    s->use_fec = 1;
    s->use_fec_v2 = 1;
}

void rudp_receiver_set_fec_v2(struct rudp_receiver *r, int k, int p)
{
    if (k < 1 || k > FEC_MAX_K) return;
    r->fec_k = k;
    r->fec_p = p;
    r->use_fec = 1;
    r->use_fec_v2 = 1;
}

static void update_rtt(struct rudp_sender *s, int64_t sample_ms)
{
    if (!s->rtt_initialized) {
        s->srtt = (int)(sample_ms << RTO_SHIFT);
        s->rttvar = (int)(sample_ms << (RTO_SHIFT - 1));
        s->rtt_initialized = 1;
    } else {
        int diff = s->srtt - (int)(sample_ms << RTO_SHIFT);
        if (diff < 0) diff = -diff;
        s->rttvar += (diff - s->rttvar) >> RTO_BETA_SHIFT;
        s->srtt += ((int)(sample_ms << RTO_SHIFT) - s->srtt) >> RTO_ALPHA_SHIFT;
    }
    s->rto_ms = (s->srtt + 4 * s->rttvar + (1 << (RTO_SHIFT - 1))) >> RTO_SHIFT;
    if (s->rto_ms < MIN_RTO_MS) s->rto_ms = MIN_RTO_MS;
    if (s->rto_ms > MAX_RTO_MS) s->rto_ms = MAX_RTO_MS;
}

/* ---- Single-packet reliable (Phase 3) ---- */

int rudp_send_reliable(struct rudp_sender *s,
                       const void *payload, int payload_len,
                       const struct sockaddr *dest, socklen_t dest_len)
{
    struct rudp_header h;
    memset(&h, 0, sizeof(h));
    h.type = RUDP_DATA;
    h.seq_num = s->next_seq;
    h.window = 1;

    s->send_base = s->next_seq;
    s->pkt_seq = s->next_seq;
    s->next_seq++;
    s->pkt_len = payload_len;
    s->retransmitted = 0;
    s->retx_count = 0;

    int total_len = rudp_build_packet(s->pkt_buf, &h, payload, payload_len);
    if (total_len < 0) return -1;

    struct pollfd pfd;
    pfd.fd = s->sockfd;
    pfd.events = POLLIN;

    for (;;) {
        s->send_ts_ms = now_ms();

        if (sendto(s->sockfd, s->pkt_buf, total_len, 0, dest, dest_len) < 0)
            return -1;

        if (s->retx_count > 0)
            s->retransmitted = 1;

        int remaining = s->rto_ms;
        while (remaining > 0) {
            int64_t before = now_ms();
            int ret = poll(&pfd, 1, remaining);
            if (ret < 0) return -1;
            if (ret > 0 && (pfd.revents & POLLIN)) {
                struct rudp_header ack_h;
                int n = rudp_recvfrom(s->sockfd, &ack_h, NULL, 0, NULL, NULL);
                if (n >= 0 && ack_h.type == RUDP_ACK && ack_h.ack_num == s->pkt_seq) {
                    int64_t rtt = now_ms() - s->send_ts_ms;
                    if (!s->retransmitted)
                        update_rtt(s, rtt);
                    return payload_len;
                }
                remaining -= (int)(now_ms() - before);
            } else {
                break;
            }
        }

        s->retx_count++;
        if (s->retx_count > MAX_RETRANSMITS)
            return -1;
    }
}

int rudp_recv_reliable(struct rudp_receiver *r,
                       void *payload_buf, int buf_size,
                       struct sockaddr *src, socklen_t *src_len,
                       float drop_rate)
{
    for (;;) {
        uint8_t raw[MAX_PACKET_SIZE];
        struct sockaddr_in sender_addr;
        socklen_t addrlen = sizeof(sender_addr);

        int n = (int)recvfrom(r->sockfd, raw, sizeof(raw), 0,
                              (struct sockaddr *)&sender_addr, &addrlen);
        if (n < 0) return -1;

        if (drop_rate > 0.0f &&
            (float)rand() / (float)RAND_MAX < drop_rate)
            continue;

        struct rudp_header h;
        const void *payload_ptr;
        int pay_len = rudp_parse_packet(raw, n, &h, &payload_ptr);
        if (pay_len < 0) continue;
        if (h.type != RUDP_DATA) continue;

        struct rudp_header ack_h;
        memset(&ack_h, 0, sizeof(ack_h));
        ack_h.type = RUDP_ACK;
        ack_h.ack_num = h.seq_num;

        rudp_sendto(r->sockfd, &ack_h, NULL, 0,
                    (struct sockaddr *)&sender_addr, addrlen);

        if (h.seq_num == r->next_expected) {
            r->next_expected++;
            if (pay_len > 0 && payload_buf && buf_size > 0) {
                int copy = pay_len < buf_size ? pay_len : buf_size;
                memcpy(payload_buf, payload_ptr, copy);
            }
            if (src && src_len)
                memcpy(src, &sender_addr,
                       *src_len < addrlen ? *src_len : addrlen);
            return pay_len;
        }
    }
}

/* ---- Sliding window + SACK (Phase 4) ---- */

static void send_sack(int sockfd, struct rudp_receiver *r,
                      struct sockaddr_in *dest, socklen_t dest_len)
{
    struct rudp_header h;
    memset(&h, 0, sizeof(h));
    h.type = RUDP_SACK;
    h.ack_num = r->next_expected - 1;
    h.window = WINDOW_SIZE;

    uint32_t bitmap = r->recv_bitmap;
    rudp_sendto(sockfd, &h, &bitmap, 4,
                (struct sockaddr *)dest, dest_len);
}

int rudp_send_sliding(struct rudp_sender *s,
                      const void *data, int data_len,
                      const struct sockaddr *dest, socklen_t dest_len)
{
    const uint8_t *ptr = (const uint8_t *)data;
    int remaining = data_len;

    struct pollfd pfd;
    pfd.fd = s->sockfd;
    pfd.events = POLLIN;

    while (remaining > 0 || (s->next_seq - s->send_base) > 0) {

        while (remaining > 0 && (s->next_seq - s->send_base) < WINDOW_SIZE) {
            uint32_t seq = s->next_seq;
            int idx = seq & (WINDOW_SIZE - 1);

            int chunk = remaining;
            if (chunk > MAX_PAYLOAD_SIZE) chunk = MAX_PAYLOAD_SIZE;

            struct rudp_header h;
            memset(&h, 0, sizeof(h));
            h.type = RUDP_DATA;
            h.seq_num = seq;
            h.window = WINDOW_SIZE;

            int total = rudp_build_packet(s->slots[idx].pkt, &h, ptr, chunk);
            if (total < 0) return -1;
            s->slots[idx].total_len = total;
            s->slots[idx].seq = seq;
            s->slots[idx].send_ts_ms = now_ms();
            s->slots[idx].retx_count = 0;
            s->slots[idx].retransmitted = 0;

            sendto(s->sockfd, s->slots[idx].pkt, total, 0, dest, dest_len);

            s->next_seq++;
            ptr += chunk;
            remaining -= chunk;
        }

        if (s->send_base == s->next_seq)
            return data_len;

        int timeout = s->rto_ms;
        int ret = poll(&pfd, 1, timeout);

        if (ret < 0) return -1;

        if (ret > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                uint8_t raw[MAX_PACKET_SIZE];
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);

                int n = (int)recvfrom(s->sockfd, raw, sizeof(raw),
                                      MSG_DONTWAIT,
                                      (struct sockaddr *)&from, &from_len);
                if (n < 0) break;

                struct rudp_header h;
                const void *sack_pl;
                int pay_len = rudp_parse_packet(raw, n, &h, &sack_pl);
                if (pay_len < 0) continue;

                if (h.type == RUDP_ACK || h.type == RUDP_SACK) {
                    uint32_t ca = h.ack_num;
                    if (ca >= s->send_base) {
                        int sampled = 0;
                        for (uint32_t seq = s->send_base; seq <= ca && seq < s->next_seq; seq++) {
                            int i = seq & (WINDOW_SIZE - 1);
                            if (s->slots[i].seq == seq) {
                                if (!sampled && s->slots[i].total_len > 0
                                    && !s->slots[i].retransmitted) {
                                    int64_t sample = now_ms() - s->slots[i].send_ts_ms;
                                    if (sample >= 0) update_rtt(s, sample);
                                    sampled = 1;
                                }
                                s->slots[i].total_len = 0;
                            }
                        }
                        s->send_base = ca + 1;
                    }

                    if (h.type == RUDP_SACK && pay_len >= 4) {
                        uint32_t bitmap;
                        memcpy(&bitmap, sack_pl, 4);
                        int sampled = 0;
                        for (int i = 0; i < WINDOW_SIZE; i++) {
                            if (bitmap & (1u << i)) {
                                uint32_t sack_seq = ca + 1 + i;
                                if (sack_seq >= s->send_base && sack_seq < s->next_seq) {
                                    int idx = sack_seq & (WINDOW_SIZE - 1);
                                    if (s->slots[idx].seq == sack_seq
                                        && s->slots[idx].total_len > 0) {
                                        if (!sampled && !s->slots[idx].retransmitted) {
                                            int64_t sample = now_ms() - s->slots[idx].send_ts_ms;
                                            if (sample >= 0) update_rtt(s, sample);
                                            sampled = 1;
                                        }
                                        s->slots[idx].total_len = 0;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            while (s->send_base < s->next_seq) {
                int idx = s->send_base & (WINDOW_SIZE - 1);
                if (s->slots[idx].seq == s->send_base && s->slots[idx].total_len > 0)
                    break;
                s->send_base++;
            }
        }

        if (ret == 0) {
            uint32_t oldest = s->send_base;
            int idx = oldest & (WINDOW_SIZE - 1);
            if (s->slots[idx].seq == oldest && s->slots[idx].total_len > 0) {
                sendto(s->sockfd, s->slots[idx].pkt,
                       s->slots[idx].total_len, 0, dest, dest_len);
                s->slots[idx].send_ts_ms = now_ms();
                s->slots[idx].retx_count++;
                s->slots[idx].retransmitted = 1;
                if (s->slots[idx].retx_count > MAX_RETRANSMITS)
                    return -1;
            }
        }
    }

    return data_len;
}

int rudp_recv_sliding(struct rudp_receiver *r,
                      void *buf, int buf_size,
                      struct sockaddr *src, socklen_t *src_len,
                      float drop_rate)
{
    uint8_t *out = (uint8_t *)buf;
    int total_recv = 0;

    while (1) {
        uint8_t raw[MAX_PACKET_SIZE];
        struct sockaddr_in sender_addr;
        socklen_t addrlen = sizeof(sender_addr);

        int n = (int)recvfrom(r->sockfd, raw, sizeof(raw), 0,
                              (struct sockaddr *)&sender_addr, &addrlen);
        if (n < 0) return -1;

        if (drop_rate > 0.0f &&
            (float)rand() / (float)RAND_MAX < drop_rate)
            continue;

        struct rudp_header h;
        const void *payload_ptr;
        int pay_len = rudp_parse_packet(raw, n, &h, &payload_ptr);
        if (pay_len < 0) continue;
        if (h.type != RUDP_DATA) continue;

        uint32_t seq = h.seq_num;

        if (seq < r->next_expected) {
            send_sack(r->sockfd, r, &sender_addr, addrlen);
            continue;
        }

        uint32_t offset = seq - r->next_expected;
        if (offset >= WINDOW_SIZE) {
            send_sack(r->sockfd, r, &sender_addr, addrlen);
            continue;
        }

        r->recv_bitmap |= (1u << offset);
        if (pay_len > 0) {
            int copy = pay_len;
            if (copy > MAX_PAYLOAD_SIZE) copy = MAX_PAYLOAD_SIZE;
            memcpy(r->packet_bufs[offset], payload_ptr, copy);
            r->packet_lens[offset] = copy;
        } else {
            r->packet_lens[offset] = 0;
        }

        send_sack(r->sockfd, r, &sender_addr, addrlen);

        while (r->recv_bitmap & 1) {
            int copy = r->packet_lens[0];
            if (buf_size > 0 && total_recv + copy <= buf_size) {
                memcpy(out + total_recv, r->packet_bufs[0], copy);
                total_recv += copy;
            }

            r->recv_bitmap >>= 1;
            for (int i = 0; i < WINDOW_SIZE - 1; i++) {
                r->packet_lens[i] = r->packet_lens[i + 1];
                memcpy(r->packet_bufs[i], r->packet_bufs[i + 1], MAX_PAYLOAD_SIZE);
            }
            r->packet_lens[WINDOW_SIZE - 1] = 0;

            r->next_expected++;

            if (buf_size > 0 && total_recv >= buf_size) {
                if (src && src_len)
                    memcpy(src, &sender_addr,
                           *src_len < addrlen ? *src_len : addrlen);
                return total_recv;
            }
        }
    }
}

/* ---- FEC sliding window (Phase 7) ---- */

static int sender_build_data_pkt(uint8_t *pkt_buf, uint32_t seq,
                                 const void *payload, int payload_len)
{
    struct rudp_header h;
    memset(&h, 0, sizeof(h));
    h.type = RUDP_DATA;
    h.seq_num = seq;
    h.window = WINDOW_SIZE;
    return rudp_build_packet(pkt_buf, &h, payload, payload_len);
}

static int sender_build_fec_pkt(uint8_t *pkt_buf, uint32_t block_start,
                                const uint8_t *parity, int parity_len,
                                int K, int P, int parity_index)
{
    uint8_t payload[MAX_PAYLOAD_SIZE + 1];
    if (parity_len > MAX_PAYLOAD_SIZE) parity_len = MAX_PAYLOAD_SIZE;
    payload[0] = (uint8_t)(((K - 1) << 5) | ((P - 1) << 3) | (parity_index & 0x07));
    memcpy(&payload[1], parity, (size_t)parity_len);

    struct rudp_header h;
    memset(&h, 0, sizeof(h));
    h.type = RUDP_FEC;
    h.seq_num = block_start;
    h.window = WINDOW_SIZE;
    h.length = (uint16_t)(parity_len + 1);
    return rudp_build_packet(pkt_buf, &h, payload, parity_len + 1);
}

static int sender_emit_fec_parity(struct rudp_sender *s,
                                  const struct sockaddr *dest, socklen_t dest_len)
{
    int K = s->fec_k;
    int pos = s->fec_block_pos;
    if (pos <= 0) return 0;

    int max_len = s->fec_block_max_len;
    if (max_len <= 0) return 0;

    const uint8_t *ptrs[FEC_MAX_K];
    for (int i = 0; i < pos; i++) ptrs[i] = s->fec_block_pkts[i];

    uint8_t parity[MAX_PAYLOAD_SIZE];
    fec_xor_parity(ptrs, pos, max_len, parity);

    int pkt_len = sender_build_fec_pkt(s->fec_block_parity_pkt,
                                       s->fec_block_start,
                                       parity, max_len, K, s->fec_p, 0);
    if (pkt_len < 0) return -1;
    s->fec_block_parity_pkt_len = pkt_len;
    s->fec_block_parity_seq = s->fec_block_start;
    s->fec_block_parity_len = max_len;
    s->fec_block_parity_retx = 0;
    s->fec_block_parity_pending = 1;

    if (sendto(s->sockfd, s->fec_block_parity_pkt, pkt_len, 0, dest, dest_len) < 0)
        return -1;
    return 0;
}

static void sender_buffer_data(struct rudp_sender *s, uint32_t seq,
                               const void *payload, int payload_len)
{
    int K = s->fec_k;
    if (s->fec_block_pos == 0) s->fec_block_start = seq;
    int idx = s->fec_block_pos;
    if (idx >= K) return;
    if (payload_len > MAX_PAYLOAD_SIZE) payload_len = MAX_PAYLOAD_SIZE;
    memcpy(s->fec_block_pkts[idx], payload, (size_t)payload_len);
    s->fec_block_pkt_lens[idx] = payload_len;
    if (payload_len > s->fec_block_max_len) s->fec_block_max_len = payload_len;
    s->fec_block_pos++;
}

int rudp_send_fec_sliding(struct rudp_sender *s,
                          const void *data, int data_len,
                          const struct sockaddr *dest, socklen_t dest_len)
{
    const uint8_t *ptr = (const uint8_t *)data;
    int remaining = data_len;
    int K = s->fec_k > 0 ? s->fec_k : FEC_K_DEFAULT;

    struct pollfd pfd;
    pfd.fd = s->sockfd;
    pfd.events = POLLIN;

    while (remaining > 0 || (s->next_seq - s->send_base) > 0
           || s->fec_block_parity_pending) {

        if (s->fec_block_parity_pending) {
            uint32_t acked_parity = 0;
            if (s->fec_block_parity_pkt_len > 0
                && s->send_base > s->fec_block_parity_seq + (uint32_t)K - 1) {
                s->fec_block_parity_pending = 0;
            } else {
                acked_parity = 1;
            }
            (void)acked_parity;
        }

        while (remaining > 0 && (s->next_seq - s->send_base) < WINDOW_SIZE) {
            uint32_t seq = s->next_seq;
            int idx = seq & (WINDOW_SIZE - 1);

            int chunk = remaining;
            if (chunk > MAX_PAYLOAD_SIZE) chunk = MAX_PAYLOAD_SIZE;

            uint8_t data_pkt[MAX_PACKET_SIZE];
            int total = sender_build_data_pkt(data_pkt, seq, ptr, chunk);
            if (total < 0) return -1;

            s->slots[idx].total_len = total;
            s->slots[idx].seq = seq;
            s->slots[idx].send_ts_ms = now_ms();
            s->slots[idx].retx_count = 0;
            s->slots[idx].retransmitted = 0;
            memcpy(s->slots[idx].pkt, data_pkt, (size_t)total);

            sender_buffer_data(s, seq, ptr, chunk);

            if (sendto(s->sockfd, data_pkt, total, 0, dest, dest_len) < 0)
                return -1;

            if (s->fec_block_pos == K) {
                if (sender_emit_fec_parity(s, dest, dest_len) < 0)
                    return -1;
                s->fec_block_pos = 0;
                s->fec_block_max_len = 0;
            }

            s->next_seq++;
            ptr += chunk;
            remaining -= chunk;
        }

        if (s->send_base == s->next_seq && remaining == 0) {
            if (s->fec_block_parity_pending) {
                int timeout = s->rto_ms;
                int ret = poll(&pfd, 1, timeout);
                if (ret < 0) return -1;
                if (ret > 0 && (pfd.revents & POLLIN)) {
                    for (;;) {
                        uint8_t raw[MAX_PACKET_SIZE];
                        struct sockaddr_in from;
                        socklen_t from_len = sizeof(from);
                        int n = (int)recvfrom(s->sockfd, raw, sizeof(raw),
                                              MSG_DONTWAIT,
                                              (struct sockaddr *)&from, &from_len);
                        if (n < 0) break;
                        struct rudp_header hh;
                        const void *sack_pl;
                        int pl = rudp_parse_packet(raw, n, &hh, &sack_pl);
                        if (pl < 0) continue;
                        if (hh.type == RUDP_ACK || hh.type == RUDP_SACK) {
                            uint32_t ca = hh.ack_num;
                            if (ca >= s->send_base)
                                s->send_base = ca + 1;
                        }
                    }
                }
                if (s->fec_block_parity_pending
                    && s->send_base > s->fec_block_parity_seq + (uint32_t)K - 1) {
                    s->fec_block_parity_pending = 0;
                } else if (s->fec_block_parity_pending) {
                    if (sendto(s->sockfd, s->fec_block_parity_pkt,
                               s->fec_block_parity_pkt_len, 0, dest, dest_len) < 0)
                        return -1;
                    s->fec_block_parity_retx++;
                    if (s->fec_block_parity_retx > MAX_RETRANSMITS)
                        return -1;
                }
                if (s->fec_block_parity_pending) {
                    if (s->fec_block_parity_pkt_len > 0
                        && s->send_base > s->fec_block_parity_seq + (uint32_t)K - 1)
                        s->fec_block_parity_pending = 0;
                }
            }
            if (!s->fec_block_parity_pending)
                return data_len;
        }

        int timeout = s->rto_ms;
        int ret = poll(&pfd, 1, timeout);

        if (ret < 0) return -1;

        if (ret > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                uint8_t raw[MAX_PACKET_SIZE];
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);

                int n = (int)recvfrom(s->sockfd, raw, sizeof(raw),
                                      MSG_DONTWAIT,
                                      (struct sockaddr *)&from, &from_len);
                if (n < 0) break;

                struct rudp_header h;
                const void *sack_pl;
                int pay_len = rudp_parse_packet(raw, n, &h, &sack_pl);
                if (pay_len < 0) continue;

                if (h.type == RUDP_ACK || h.type == RUDP_SACK) {
                    uint32_t ca = h.ack_num;
                    if (ca >= s->send_base) {
                        int sampled = 0;
                        for (uint32_t seq = s->send_base; seq <= ca && seq < s->next_seq; seq++) {
                            int i = seq & (WINDOW_SIZE - 1);
                            if (s->slots[i].seq == seq) {
                                if (!sampled && s->slots[i].total_len > 0
                                    && !s->slots[i].retransmitted) {
                                    int64_t sample = now_ms() - s->slots[i].send_ts_ms;
                                    if (sample >= 0) update_rtt(s, sample);
                                    sampled = 1;
                                }
                                s->slots[i].total_len = 0;
                            }
                        }
                        s->send_base = ca + 1;
                    }

                    if (h.type == RUDP_SACK && pay_len >= 4) {
                        uint32_t bitmap;
                        memcpy(&bitmap, sack_pl, 4);
                        int sampled = 0;
                        for (int i = 0; i < WINDOW_SIZE; i++) {
                            if (bitmap & (1u << i)) {
                                uint32_t sack_seq = ca + 1 + i;
                                if (sack_seq >= s->send_base && sack_seq < s->next_seq) {
                                    int idx = sack_seq & (WINDOW_SIZE - 1);
                                    if (s->slots[idx].seq == sack_seq
                                        && s->slots[idx].total_len > 0) {
                                        if (!sampled && !s->slots[idx].retransmitted) {
                                            int64_t sample = now_ms() - s->slots[idx].send_ts_ms;
                                            if (sample >= 0) update_rtt(s, sample);
                                            sampled = 1;
                                        }
                                        s->slots[idx].total_len = 0;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            while (s->send_base < s->next_seq) {
                int idx = s->send_base & (WINDOW_SIZE - 1);
                if (s->slots[idx].seq == s->send_base && s->slots[idx].total_len > 0)
                    break;
                s->send_base++;
            }
        }

        if (ret == 0) {
            uint32_t oldest = s->send_base;
            int idx = oldest & (WINDOW_SIZE - 1);
            if (s->slots[idx].seq == oldest && s->slots[idx].total_len > 0) {
                sendto(s->sockfd, s->slots[idx].pkt,
                       s->slots[idx].total_len, 0, dest, dest_len);
                s->slots[idx].send_ts_ms = now_ms();
                s->slots[idx].retx_count++;
                s->slots[idx].retransmitted = 1;
                if (s->slots[idx].retx_count > MAX_RETRANSMITS)
                    return -1;
            } else if (s->fec_block_parity_pending
                       && s->fec_block_parity_pkt_len > 0
                       && s->send_base <= s->fec_block_parity_seq + (uint32_t)K - 1) {
                sendto(s->sockfd, s->fec_block_parity_pkt,
                       s->fec_block_parity_pkt_len, 0, dest, dest_len);
                s->fec_block_parity_retx++;
                if (s->fec_block_parity_retx > MAX_RETRANSMITS)
                    return -1;
            }
        }
    }

    return data_len;
}

static void receiver_shift_window(struct rudp_receiver *r, int n)
{
    if (n <= 0) return;
    if (n >= WINDOW_SIZE) {
        memset(r->packet_bufs, 0, sizeof(r->packet_bufs));
        memset(r->packet_lens, 0, sizeof(r->packet_lens));
        memset(r->packet_set, 0, sizeof(r->packet_set));
        r->recv_bitmap = 0;
        r->next_expected += (uint32_t)n;
        return;
    }
    for (int i = 0; i < WINDOW_SIZE - n; i++) {
        r->packet_lens[i] = r->packet_lens[i + n];
        r->packet_set[i] = r->packet_set[i + n];
        memcpy(r->packet_bufs[i], r->packet_bufs[i + n], MAX_PAYLOAD_SIZE);
    }
    for (int i = WINDOW_SIZE - n; i < WINDOW_SIZE; i++) {
        r->packet_lens[i] = 0;
        r->packet_set[i] = 0;
    }
    r->recv_bitmap >>= n;
    r->next_expected += (uint32_t)n;
}

static int receiver_try_finish_block(struct rudp_receiver *r,
                                     uint8_t *out, int buf_size, int *total_recv)
{
    int K = r->fec_k > 0 ? r->fec_k : FEC_K_DEFAULT;
    int present_count = 0;
    int missing_idx = -1;
    int max_len = 0;
    for (int i = 0; i < K; i++) {
        if (r->packet_set[i]) {
            present_count++;
            if (r->packet_lens[i] > max_len) max_len = r->packet_lens[i];
        } else {
            missing_idx = i;
        }
    }

    if (present_count == K) {
        for (int i = 0; i < K; i++) {
            int len = r->packet_lens[i];
            if (len > 0 && buf_size > 0 && *total_recv + len <= buf_size) {
                memcpy(out + *total_recv, r->packet_bufs[i], (size_t)len);
                *total_recv += len;
            }
        }
        receiver_shift_window(r, K);
        r->fec_block_start = r->next_expected;
        r->fec_block_parity_present = 0;
        r->fec_block_parity_len = 0;
        return 1;
    }

    if (present_count == K - 1 && r->fec_block_parity_present
        && missing_idx >= 0) {
        if (max_len > r->fec_block_parity_len) max_len = r->fec_block_parity_len;
        if (max_len > MAX_PAYLOAD_SIZE) max_len = MAX_PAYLOAD_SIZE;

        const uint8_t *present[FEC_MAX_K];
        int n_present = 0;
        for (int i = 0; i < K; i++) {
            if (r->packet_set[i]) {
                present[n_present++] = r->packet_bufs[i];
            }
        }
        fec_xor_recover(r->packet_bufs[missing_idx], present, n_present,
                        missing_idx, max_len, r->fec_block_parity);
        r->packet_lens[missing_idx] = max_len;
        r->packet_set[missing_idx] = 1;

        for (int i = 0; i < K; i++) {
            int len = r->packet_lens[i];
            if (len > 0 && buf_size > 0 && *total_recv + len <= buf_size) {
                memcpy(out + *total_recv, r->packet_bufs[i], (size_t)len);
                *total_recv += len;
            }
        }
        receiver_shift_window(r, K);
        r->fec_block_start = r->next_expected;
        r->fec_block_parity_present = 0;
        r->fec_block_parity_len = 0;
        return 1;
    }

    return 0;
}

static void receiver_deliver_partial(struct rudp_receiver *r,
                                     uint8_t *out, int buf_size, int *total_recv)
{
    int K = r->fec_k > 0 ? r->fec_k : FEC_K_DEFAULT;
    for (int i = 0; i < K; i++) {
        if (r->packet_set[i]) {
            int len = r->packet_lens[i];
            if (len > 0 && buf_size > 0 && *total_recv + len <= buf_size) {
                memcpy(out + *total_recv, r->packet_bufs[i], (size_t)len);
                *total_recv += len;
            }
        }
    }
}

int rudp_recv_fec_sliding(struct rudp_receiver *r,
                          void *buf, int buf_size,
                          struct sockaddr *src, socklen_t *src_len,
                          float drop_rate)
{
    uint8_t *out = (uint8_t *)buf;
    int total_recv = 0;

    while (1) {
        uint8_t raw[MAX_PACKET_SIZE];
        struct sockaddr_in sender_addr;
        socklen_t addrlen = sizeof(sender_addr);

        int n = (int)recvfrom(r->sockfd, raw, sizeof(raw), 0,
                              (struct sockaddr *)&sender_addr, &addrlen);
        if (n < 0) return -1;

        if (drop_rate > 0.0f &&
            (float)rand() / (float)RAND_MAX < drop_rate)
            continue;

        struct rudp_header h;
        const void *payload_ptr;
        int pay_len = rudp_parse_packet(raw, n, &h, &payload_ptr);
        if (pay_len < 0) continue;

        if (h.type == RUDP_FIN) {
            receiver_deliver_partial(r, out, buf_size, &total_recv);
            if (src && src_len)
                memcpy(src, &sender_addr,
                       *src_len < addrlen ? *src_len : addrlen);
            return total_recv;
        }

        if (h.type == RUDP_FEC) {
            uint32_t bs = h.seq_num;
            uint32_t cur_bs = r->fec_block_start > 0 ? r->fec_block_start : 1;
            if (bs != cur_bs) continue;
            if (pay_len < 1) continue;
            const uint8_t *parity_payload = (const uint8_t *)payload_ptr + 1;
            int parity_len = pay_len - 1;
            if (parity_len > MAX_PAYLOAD_SIZE) parity_len = MAX_PAYLOAD_SIZE;
            memcpy(r->fec_block_parity, parity_payload, (size_t)parity_len);
            r->fec_block_parity_len = parity_len;
            r->fec_block_parity_present = 1;
            receiver_try_finish_block(r, out, buf_size, &total_recv);
            continue;
        }

        if (h.type != RUDP_DATA) continue;

        uint32_t seq = h.seq_num;

        if (seq >= r->next_expected) {
            uint32_t offset = seq - r->next_expected;
            if (offset < WINDOW_SIZE) {
                int copy = pay_len;
                if (copy > MAX_PAYLOAD_SIZE) copy = MAX_PAYLOAD_SIZE;
                memcpy(r->packet_bufs[offset], payload_ptr, (size_t)copy);
                r->packet_lens[offset] = copy;
                r->packet_set[offset] = 1;
                r->recv_bitmap |= (1u << offset);
            }
        }

        send_sack(r->sockfd, r, &sender_addr, addrlen);

        receiver_try_finish_block(r, out, buf_size, &total_recv);

        if (buf_size > 0 && total_recv >= buf_size) {
            if (src && src_len)
                memcpy(src, &sender_addr,
                       *src_len < addrlen ? *src_len : addrlen);
            return total_recv;
        }
    }
}

/* ---- Block-ACK FEC (Phase 8) ---- */

static int sender_build_block_data_pkt(uint8_t *pkt_buf, uint32_t seq,
                                       const void *payload, int payload_len,
                                       int block_k)
{
    struct rudp_header h;
    memset(&h, 0, sizeof(h));
    h.type = RUDP_DATA;
    h.seq_num = seq;
    h.window = (uint8_t)block_k;
    return rudp_build_packet(pkt_buf, &h, payload, payload_len);
}

static void send_block_ack(int sockfd, uint32_t block_start,
                           uint32_t missing_bitmap,
                           struct sockaddr_in *dest, socklen_t dest_len)
{
    struct rudp_header h;
    memset(&h, 0, sizeof(h));
    h.type = RUDP_BLOCK_ACK;
    h.ack_num = block_start;
    rudp_sendto(sockfd, &h, &missing_bitmap, 4,
                (struct sockaddr *)dest, dest_len);
}

int rudp_send_block_fec(struct rudp_sender *s,
                        const void *data, int data_len,
                        const struct sockaddr *dest, socklen_t dest_len)
{
    const uint8_t *ptr = (const uint8_t *)data;
    int remaining = data_len;
    int K = s->fec_k > 0 ? s->fec_k : FEC_K_DEFAULT;
    if (K > 8) K = 8;

    struct pollfd pfd;
    pfd.fd = s->sockfd;
    pfd.events = POLLIN;

    while (remaining > 0) {
        uint32_t block_start = s->next_seq;
        uint8_t data_pkts[FEC_MAX_K][MAX_PAYLOAD_SIZE];
        uint8_t wire_pkts[FEC_MAX_K][MAX_PACKET_SIZE];
        int wire_lens[FEC_MAX_K];
        int data_lens[FEC_MAX_K];
        int block_count = 0;
        int max_len = 0;

        memset(data_pkts, 0, sizeof(data_pkts));
        memset(wire_lens, 0, sizeof(wire_lens));
        memset(data_lens, 0, sizeof(data_lens));

        while (block_count < K && remaining > 0) {
            int chunk = remaining;
            if (chunk > MAX_PAYLOAD_SIZE) chunk = MAX_PAYLOAD_SIZE;
            memcpy(data_pkts[block_count], ptr, (size_t)chunk);
            data_lens[block_count] = chunk;
            if (chunk > max_len) max_len = chunk;
            ptr += chunk;
            remaining -= chunk;
            block_count++;
        }

        if (block_count <= 0) break;

        for (int i = 0; i < block_count; i++) {
            uint32_t seq = block_start + (uint32_t)i;
            wire_lens[i] = sender_build_block_data_pkt(wire_pkts[i], seq,
                                                       data_pkts[i], data_lens[i],
                                                       block_count);
            if (wire_lens[i] < 0) return -1;
        }

        const uint8_t *data_ptrs[FEC_MAX_K];
        for (int i = 0; i < block_count; i++) data_ptrs[i] = data_pkts[i];
        uint8_t parity[MAX_PAYLOAD_SIZE];
        fec_xor_parity(data_ptrs, block_count, max_len, parity);

        uint8_t parity_pkt[MAX_PACKET_SIZE];
        int parity_len = sender_build_fec_pkt(parity_pkt, block_start,
                                              parity, max_len,
                                              block_count, 1, 0);
        if (parity_len < 0) return -1;

        for (int i = 0; i < block_count; i++) {
            if (sendto(s->sockfd, wire_pkts[i], wire_lens[i], 0, dest, dest_len) < 0)
                return -1;
        }
        if (sendto(s->sockfd, parity_pkt, parity_len, 0, dest, dest_len) < 0)
            return -1;

        int retx_count = 0;
        int retransmitted = 0;
        int64_t send_ts = now_ms();

        for (;;) {
            int ret = poll(&pfd, 1, s->rto_ms);
            if (ret < 0) return -1;

            if (ret > 0 && (pfd.revents & POLLIN)) {
                int done = 0;
                for (;;) {
                    uint8_t raw[MAX_PACKET_SIZE];
                    struct sockaddr_in from;
                    socklen_t from_len = sizeof(from);
                    int n = (int)recvfrom(s->sockfd, raw, sizeof(raw),
                                          MSG_DONTWAIT,
                                          (struct sockaddr *)&from, &from_len);
                    if (n < 0) break;

                    struct rudp_header h;
                    const void *ack_payload;
                    int pay_len = rudp_parse_packet(raw, n, &h, &ack_payload);
                    if (pay_len < 4 || h.type != RUDP_BLOCK_ACK) continue;
                    if (h.ack_num != block_start) continue;

                    uint32_t missing_bitmap;
                    memcpy(&missing_bitmap, ack_payload, 4);

                    if (missing_bitmap == 0) {
                        if (!retransmitted) {
                            int64_t sample = now_ms() - send_ts;
                            if (sample >= 0) update_rtt(s, sample);
                        }
                        s->next_seq = block_start + (uint32_t)block_count;
                        s->send_base = s->next_seq;
                        done = 1;
                        break;
                    }

                    for (int i = 0; i < block_count; i++) {
                        if (missing_bitmap & (1u << i)) {
                            if (sendto(s->sockfd, wire_pkts[i], wire_lens[i],
                                       0, dest, dest_len) < 0)
                                return -1;
                        }
                    }
                    retx_count++;
                    retransmitted = 1;
                    send_ts = now_ms();
                    if (retx_count > MAX_RETRANSMITS) return -1;
                }
                if (done) break;
            } else {
                retx_count++;
                retransmitted = 1;
                if (retx_count > MAX_RETRANSMITS) return -1;
                for (int i = 0; i < block_count; i++) {
                    if (sendto(s->sockfd, wire_pkts[i], wire_lens[i], 0,
                               dest, dest_len) < 0)
                        return -1;
                }
                if (sendto(s->sockfd, parity_pkt, parity_len, 0, dest, dest_len) < 0)
                    return -1;
                send_ts = now_ms();
            }
        }
    }

    return data_len;
}

static uint32_t block_missing_bitmap(const int *packet_set, int block_k)
{
    uint32_t missing = 0;
    for (int i = 0; i < block_k; i++) {
        if (!packet_set[i]) missing |= (1u << i);
    }
    return missing;
}

static int block_present_count(const int *packet_set, int block_k)
{
    int present = 0;
    for (int i = 0; i < block_k; i++) {
        if (packet_set[i]) present++;
    }
    return present;
}

static void block_deliver(struct rudp_receiver *r, int block_k,
                          uint8_t *out, int buf_size, int *total_recv)
{
    for (int i = 0; i < block_k; i++) {
        int len = r->packet_lens[i];
        if (len <= 0 || buf_size <= 0 || *total_recv >= buf_size) continue;
        int copy = len;
        if (*total_recv + copy > buf_size) copy = buf_size - *total_recv;
        memcpy(out + *total_recv, r->packet_bufs[i], (size_t)copy);
        *total_recv += copy;
    }
}

static int block_try_finish(struct rudp_receiver *r, int block_k,
                            uint8_t *out, int buf_size, int *total_recv)
{
    int present = block_present_count(r->packet_set, block_k);
    if (present == block_k) {
        block_deliver(r, block_k, out, buf_size, total_recv);
        return 1;
    }

    if (present == block_k - 1 && r->fec_block_parity_present) {
        int missing_idx = -1;
        int max_len = r->fec_block_parity_len;
        for (int i = 0; i < block_k; i++) {
            if (!r->packet_set[i]) missing_idx = i;
            else if (r->packet_lens[i] > max_len) max_len = r->packet_lens[i];
        }
        if (missing_idx < 0) return 0;
        if (max_len > MAX_PAYLOAD_SIZE) max_len = MAX_PAYLOAD_SIZE;

        const uint8_t *present_pkts[FEC_MAX_K];
        int n_present = 0;
        for (int i = 0; i < block_k; i++) {
            if (r->packet_set[i]) present_pkts[n_present++] = r->packet_bufs[i];
        }
        fec_xor_recover(r->packet_bufs[missing_idx], present_pkts, n_present,
                        missing_idx, max_len, r->fec_block_parity);
        r->packet_lens[missing_idx] = max_len;
        r->packet_set[missing_idx] = 1;
        block_deliver(r, block_k, out, buf_size, total_recv);
        return 1;
    }

    return 0;
}

static void block_reset_receiver(struct rudp_receiver *r, int block_k)
{
    for (int i = 0; i < block_k; i++) {
        r->packet_lens[i] = 0;
        r->packet_set[i] = 0;
        memset(r->packet_bufs[i], 0, MAX_PAYLOAD_SIZE);
    }
    r->next_expected += (uint32_t)block_k;
    r->fec_block_start = r->next_expected;
    r->fec_block_parity_present = 0;
    r->fec_block_parity_len = 0;
}

int rudp_recv_block_fec(struct rudp_receiver *r,
                        void *buf, int buf_size,
                        struct sockaddr *src, socklen_t *src_len,
                        float drop_rate)
{
    uint8_t *out = (uint8_t *)buf;
    int total_recv = 0;
    int block_k = 0;

    while (1) {
        uint8_t raw[MAX_PACKET_SIZE];
        struct sockaddr_in sender_addr;
        socklen_t addrlen = sizeof(sender_addr);

        int n = (int)recvfrom(r->sockfd, raw, sizeof(raw), 0,
                              (struct sockaddr *)&sender_addr, &addrlen);
        if (n < 0) return -1;

        if (drop_rate > 0.0f &&
            (float)rand() / (float)RAND_MAX < drop_rate)
            continue;

        struct rudp_header h;
        const void *payload_ptr;
        int pay_len = rudp_parse_packet(raw, n, &h, &payload_ptr);
        if (pay_len < 0) continue;

        if (h.type == RUDP_FIN) {
            if (src && src_len)
                memcpy(src, &sender_addr,
                       *src_len < addrlen ? *src_len : addrlen);
            return total_recv;
        }

        if (h.type == RUDP_DATA) {
            int pkt_block_k = h.window > 0 ? h.window : r->fec_k;
            if (pkt_block_k < 1 || pkt_block_k > 8) continue;
            if (h.seq_num < r->next_expected) {
                uint32_t old_start = h.seq_num - ((h.seq_num - 1) % (uint32_t)pkt_block_k);
                send_block_ack(r->sockfd, old_start, 0, &sender_addr, addrlen);
                continue;
            }
            if (block_k == 0) block_k = pkt_block_k;
            uint32_t offset = h.seq_num - r->next_expected;
            if (offset >= (uint32_t)block_k) continue;

            int copy = pay_len;
            if (copy > MAX_PAYLOAD_SIZE) copy = MAX_PAYLOAD_SIZE;
            memcpy(r->packet_bufs[offset], payload_ptr, (size_t)copy);
            r->packet_lens[offset] = copy;
            r->packet_set[offset] = 1;
        } else if (h.type == RUDP_FEC) {
            if (pay_len < 1) continue;
            const uint8_t *fec_payload = (const uint8_t *)payload_ptr;
            int pkt_block_k = ((fec_payload[0] >> 5) & 0x07) + 1;
            if (pkt_block_k < 1 || pkt_block_k > 8) continue;
            if (h.seq_num < r->next_expected) {
                send_block_ack(r->sockfd, h.seq_num, 0, &sender_addr, addrlen);
                continue;
            }
            if (h.seq_num != r->next_expected) continue;
            if (block_k == 0) block_k = pkt_block_k;
            if (pkt_block_k != block_k) continue;

            int parity_len = pay_len - 1;
            if (parity_len > MAX_PAYLOAD_SIZE) parity_len = MAX_PAYLOAD_SIZE;
            memcpy(r->fec_block_parity, fec_payload + 1, (size_t)parity_len);
            r->fec_block_parity_len = parity_len;
            r->fec_block_parity_present = 1;
        } else {
            continue;
        }

        if (block_k > 0 && block_try_finish(r, block_k, out, buf_size, &total_recv)) {
            uint32_t done_start = r->next_expected;
            send_block_ack(r->sockfd, done_start, 0, &sender_addr, addrlen);
            block_reset_receiver(r, block_k);
            block_k = 0;
            if (buf_size > 0 && total_recv >= buf_size) {
                if (src && src_len)
                    memcpy(src, &sender_addr,
                           *src_len < addrlen ? *src_len : addrlen);
                return total_recv;
            }
            continue;
        }

        if (block_k > 0 && r->fec_block_parity_present) {
            uint32_t missing = block_missing_bitmap(r->packet_set, block_k);
            if (missing != 0) {
                send_block_ack(r->sockfd, r->next_expected, missing,
                               &sender_addr, addrlen);
            }
        }
    }
}

int rudp_sender_get_rto(const struct rudp_sender *s)
{
    return s->rto_ms;
}

int rudp_sender_get_srtt(const struct rudp_sender *s)
{
    if (!s->rtt_initialized) return 0;
    return s->srtt >> RTO_SHIFT;
}
