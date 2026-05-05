#include "rudp_reliable.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
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

int rudp_sender_get_rto(const struct rudp_sender *s)
{
    return s->rto_ms;
}

int rudp_sender_get_srtt(const struct rudp_sender *s)
{
    if (!s->rtt_initialized) return 0;
    return s->srtt >> RTO_SHIFT;
}
