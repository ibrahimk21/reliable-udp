#ifndef RUDP_RELIABLE_H
#define RUDP_RELIABLE_H

#include "rudp.h"
#include <stdint.h>
#include <netinet/in.h>

#define INITIAL_RTO_MS  500
#define MAX_RETRANSMITS 10
#define MIN_RTO_MS      100
#define MAX_RTO_MS      10000

#define RTO_SHIFT       3
#define RTO_ALPHA_SHIFT 3
#define RTO_BETA_SHIFT  2

#define WINDOW_SIZE 32

#define FEC_MAX_K 32

struct sender_slot {
    uint8_t  pkt[MAX_PACKET_SIZE];
    uint32_t seq;
    int      total_len;
    int64_t  send_ts_ms;
    int      retx_count;
    int      retransmitted;
};

struct rudp_sender {
    int      sockfd;
    uint32_t next_seq;
    uint32_t send_base;

    uint8_t  pkt_buf[MAX_PACKET_SIZE];
    int      pkt_len;
    uint32_t pkt_seq;
    int64_t  send_ts_ms;
    int      retx_count;
    int      retransmitted;

    struct sender_slot slots[WINDOW_SIZE];

    int rto_ms;
    int srtt;
    int rttvar;
    int rtt_initialized;

    int fec_k;
    int fec_p;
    int use_fec;
    int use_fec_v2;
    int fec_block_pos;
    uint32_t fec_block_start;
    int fec_block_pkt_lens[FEC_MAX_K];
    uint8_t fec_block_pkts[FEC_MAX_K][MAX_PAYLOAD_SIZE];
    int fec_block_max_len;
    int fec_block_parity_pending;
    uint32_t fec_block_parity_seq;
    int fec_block_parity_len;
    uint8_t fec_block_parity[MAX_PAYLOAD_SIZE];
    uint8_t fec_block_parity_pkt[MAX_PACKET_SIZE];
    int fec_block_parity_pkt_len;
    int fec_block_parity_retx;
};

struct rudp_receiver {
    int      sockfd;
    uint32_t next_expected;
    uint32_t recv_bitmap;
    uint8_t  packet_bufs[WINDOW_SIZE][MAX_PAYLOAD_SIZE];
    int      packet_lens[WINDOW_SIZE];
    int      packet_set[WINDOW_SIZE];

    int fec_k;
    int fec_p;
    int use_fec;
    int use_fec_v2;
    uint32_t fec_block_start;
    int fec_block_parity_present;
    int fec_block_parity_len;
    uint8_t fec_block_parity[MAX_PAYLOAD_SIZE];
};

void rudp_sender_init(struct rudp_sender *s, int sockfd);
void rudp_receiver_init(struct rudp_receiver *r, int sockfd);

void rudp_sender_set_fec(struct rudp_sender *s, int k, int p);
void rudp_receiver_set_fec(struct rudp_receiver *r, int k, int p);
void rudp_sender_set_fec_v2(struct rudp_sender *s, int k, int p);
void rudp_receiver_set_fec_v2(struct rudp_receiver *r, int k, int p);

int rudp_send_reliable(struct rudp_sender *s,
                       const void *payload, int payload_len,
                       const struct sockaddr *dest, socklen_t dest_len);

int rudp_recv_reliable(struct rudp_receiver *r,
                       void *payload_buf, int buf_size,
                       struct sockaddr *src, socklen_t *src_len,
                       float drop_rate);

int rudp_send_sliding(struct rudp_sender *s,
                      const void *data, int data_len,
                      const struct sockaddr *dest, socklen_t dest_len);

int rudp_recv_sliding(struct rudp_receiver *r,
                      void *buf, int buf_size,
                      struct sockaddr *src, socklen_t *src_len,
                      float drop_rate);

int rudp_send_fec_sliding(struct rudp_sender *s,
                          const void *data, int data_len,
                          const struct sockaddr *dest, socklen_t dest_len);

int rudp_recv_fec_sliding(struct rudp_receiver *r,
                          void *buf, int buf_size,
                          struct sockaddr *src, socklen_t *src_len,
                          float drop_rate);

int rudp_send_block_fec(struct rudp_sender *s,
                        const void *data, int data_len,
                        const struct sockaddr *dest, socklen_t dest_len);

int rudp_recv_block_fec(struct rudp_receiver *r,
                        void *buf, int buf_size,
                        struct sockaddr *src, socklen_t *src_len,
                        float drop_rate);

int rudp_sender_get_rto(const struct rudp_sender *s);
int rudp_sender_get_srtt(const struct rudp_sender *s);

#endif /* RUDP_RELIABLE_H */
