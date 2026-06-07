#ifndef RUDP_H
#define RUDP_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

#define RUDP_HEADER_SIZE 14
#define MAX_PAYLOAD_SIZE 1024
#define MAX_PACKET_SIZE  (RUDP_HEADER_SIZE + MAX_PAYLOAD_SIZE)

enum rudp_type {
    RUDP_DATA = 0,
    RUDP_ACK  = 1,
    RUDP_SACK = 2,
    RUDP_SYN  = 3,
    RUDP_FIN  = 4,
    RUDP_FEC  = 5,
    RUDP_BLOCK_ACK = 6,
};

struct rudp_header {
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t checksum;
    uint16_t length;
    uint8_t  type;
    uint8_t  window;
} __attribute__((packed));

uint16_t rudp_checksum(const void *buf, int len);

void rudp_header_encode(void *dst, const struct rudp_header *h);

void rudp_header_decode(struct rudp_header *h, const void *src);

int rudp_build_packet(void *buf, const struct rudp_header *h,
                      const void *payload, int payload_len);

int rudp_parse_packet(const void *buf, int buf_len,
                      struct rudp_header *h, const void **payload);

int rudp_sendto(int sockfd, const struct rudp_header *h,
                const void *payload, int payload_len,
                const struct sockaddr *dest_addr, socklen_t addrlen);

int rudp_recvfrom(int sockfd, struct rudp_header *h,
                  void *payload_buf, int payload_size,
                  struct sockaddr *src_addr, socklen_t *addrlen);

#endif /* RUDP_H */
