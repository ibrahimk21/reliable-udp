#include "rudp.h"
#include <string.h>

uint16_t rudp_checksum(const void *buf, int len)
{
    const uint16_t *words = (const uint16_t *)buf;
    uint32_t sum = 0;

    for (int i = 0; i < len / 2; i++)
        sum += words[i];

    if (len & 1)
        sum += ((uint32_t)((const uint8_t *)buf)[len - 1]) << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

void rudp_header_encode(void *dst, const struct rudp_header *h)
{
    uint8_t *d = (uint8_t *)dst;
    d[0] = (uint8_t)(h->seq_num >> 24);
    d[1] = (uint8_t)(h->seq_num >> 16);
    d[2] = (uint8_t)(h->seq_num >> 8);
    d[3] = (uint8_t)(h->seq_num);
    d[4] = (uint8_t)(h->ack_num >> 24);
    d[5] = (uint8_t)(h->ack_num >> 16);
    d[6] = (uint8_t)(h->ack_num >> 8);
    d[7] = (uint8_t)(h->ack_num);
    d[8] = (uint8_t)(h->checksum >> 8);
    d[9] = (uint8_t)(h->checksum);
    d[10] = (uint8_t)(h->length >> 8);
    d[11] = (uint8_t)(h->length);
    d[12] = h->type;
    d[13] = h->window;
}

void rudp_header_decode(struct rudp_header *h, const void *src)
{
    const uint8_t *s = (const uint8_t *)src;
    h->seq_num  = ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) |
                  ((uint32_t)s[2] << 8)  | (uint32_t)s[3];
    h->ack_num  = ((uint32_t)s[4] << 24) | ((uint32_t)s[5] << 16) |
                  ((uint32_t)s[6] << 8)  | (uint32_t)s[7];
    h->checksum = ((uint16_t)s[8] << 8) | (uint16_t)s[9];
    h->length   = ((uint16_t)s[10] << 8) | (uint16_t)s[11];
    h->type     = s[12];
    h->window   = s[13];
}

int rudp_build_packet(void *buf, const struct rudp_header *h,
                       const void *payload, int payload_len)
{
    struct rudp_header tmp = *h;
    uint8_t *b = (uint8_t *)buf;

    tmp.length = RUDP_HEADER_SIZE + payload_len;
    tmp.checksum = 0;

    rudp_header_encode(b, &tmp);

    if (payload && payload_len > 0)
        memcpy(b + RUDP_HEADER_SIZE, payload, payload_len);

    tmp.checksum = rudp_checksum(buf, tmp.length);
    b[8] = (uint8_t)(tmp.checksum >> 8);
    b[9] = (uint8_t)(tmp.checksum);

    return tmp.length;
}

int rudp_parse_packet(const void *buf, int buf_len,
                       struct rudp_header *h, const void **payload)
{
    if (buf_len < RUDP_HEADER_SIZE)
        return -1;

    rudp_header_decode(h, buf);

    if (buf_len < h->length)
        return -1;

    uint16_t csum = h->checksum;
    uint8_t tmp[h->length];
    memcpy(tmp, buf, h->length);
    tmp[8] = tmp[9] = 0;
    uint16_t local_csum = rudp_checksum(tmp, h->length);

    if (local_csum != csum)
        return -1;

    if (payload && h->length > RUDP_HEADER_SIZE)
        *payload = (const uint8_t *)buf + RUDP_HEADER_SIZE;

    return h->length - RUDP_HEADER_SIZE;
}

int rudp_sendto(int sockfd, const struct rudp_header *h,
                const void *payload, int payload_len,
                const struct sockaddr *dest_addr, socklen_t addrlen)
{
    uint8_t buf[MAX_PACKET_SIZE];
    int total_len = rudp_build_packet(buf, h, payload, payload_len);
    if (total_len < 0)
        return -1;

    return sendto(sockfd, buf, total_len, 0, dest_addr, addrlen);
}

int rudp_recvfrom(int sockfd, struct rudp_header *h,
                  void *payload_buf, int payload_size,
                  struct sockaddr *src_addr, socklen_t *addrlen)
{
    uint8_t raw[MAX_PACKET_SIZE];
    int n = (int)recvfrom(sockfd, raw, sizeof(raw), 0, src_addr, addrlen);
    if (n < 0)
        return -1;

    const void *payload_ptr;
    int pay_len = rudp_parse_packet(raw, n, h, &payload_ptr);
    if (pay_len < 0)
        return -1;

    if (pay_len > 0 && payload_buf && payload_size > 0) {
        int copy = pay_len < payload_size ? pay_len : payload_size;
        memcpy(payload_buf, payload_ptr, copy);
    }

    return pay_len;
}
