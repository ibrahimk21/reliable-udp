#include "rudp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int tests = 0, passed = 0;

#define TEST(cond, msg) do { \
    tests++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
    } else { \
        passed++; \
        printf("PASS: %s\n", msg); \
    } \
} while(0)

int main(void)
{
    /* Test 1: all-zero buffer yields 0xFFFF */
    {
        uint8_t buf[8] = {0};
        uint16_t c = rudp_checksum(buf, 8);
        TEST(c == 0xFFFF, "all-zero buffer yields 0xFFFF");
    }

    /* Test 2: checksum is deterministic (same input → same output) */
    {
        uint8_t buf[] = {0x41, 0x42, 0x43, 0x44};
        uint16_t c1 = rudp_checksum(buf, 4);
        uint16_t c2 = rudp_checksum(buf, 4);
        TEST(c1 == c2, "checksum is deterministic");
    }

    /* Test 3: build and parse round-trip */
    {
        struct rudp_header h;
        memset(&h, 0, sizeof(h));
        h.seq_num = 42;
        h.ack_num = 7;
        h.type    = RUDP_DATA;
        h.window  = 32;

        uint8_t payload[] = "hello";
        uint8_t buf[MAX_PACKET_SIZE];

        int total_len = rudp_build_packet(buf, &h, payload, 6);

        struct rudp_header h2;
        const void *pay2;
        int pay_len = rudp_parse_packet(buf, total_len, &h2, &pay2);

        TEST(pay_len == 6, "parse returns correct payload len");
        TEST(h2.seq_num == 42, "seq_num round-trips");
        TEST(h2.ack_num == 7, "ack_num round-trips");
        TEST(h2.type == RUDP_DATA, "type round-trips");
        TEST(h2.window == 32, "window round-trips");
        TEST(h2.length == total_len, "length matches");
        TEST(pay2 != NULL && memcmp(pay2, "hello", 6) == 0, "payload content matches");
    }

    /* Test 4: checksum mismatch detection */
    {
        struct rudp_header h;
        memset(&h, 0, sizeof(h));
        h.seq_num = 1;
        h.type = RUDP_DATA;

        uint8_t buf[MAX_PACKET_SIZE];
        int total_len = rudp_build_packet(buf, &h, "x", 1);

        buf[total_len - 1] ^= 0xFF;

        struct rudp_header h2;
        const void *pay2;
        int ret = rudp_parse_packet(buf, total_len, &h2, &pay2);
        TEST(ret == -1, "corrupted packet returns -1");
    }

    /* Test 5: build and parse with various seq/ack values */
    {
        struct rudp_header h;
        memset(&h, 0, sizeof(h));
        h.seq_num = 0xFFFFFFFF;
        h.ack_num = 0;
        h.type    = RUDP_ACK;
        h.window  = 16;

        uint8_t buf[MAX_PACKET_SIZE];
        int total_len = rudp_build_packet(buf, &h, NULL, 0);

        struct rudp_header h2;
        const void *pay2;
        int pay_len = rudp_parse_packet(buf, total_len, &h2, &pay2);

        TEST(pay_len == 0, "zero-length payload OK");
        TEST(h2.seq_num == 0xFFFFFFFF, "max seq_num round-trips");
        TEST(h2.type == RUDP_ACK, "ACK type round-trips");
    }

    /* Test 6: SACK type packet */
    {
        struct rudp_header h;
        memset(&h, 0, sizeof(h));
        h.seq_num = 100;
        h.ack_num = 50;
        h.type    = RUDP_SACK;
        h.window  = 64;

        uint8_t bitmap[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        uint8_t buf[MAX_PACKET_SIZE];
        int total_len = rudp_build_packet(buf, &h, bitmap, 4);

        struct rudp_header h2;
        const void *pay2;
        int pay_len = rudp_parse_packet(buf, total_len, &h2, &pay2);

        TEST(pay_len == 4, "SACK payload len OK");
        TEST(h2.seq_num == 100, "SACK seq_num OK");
        TEST(h2.type == RUDP_SACK, "SACK type OK");
        TEST(memcmp(pay2, bitmap, 4) == 0, "SACK bitmap OK");
    }

    printf("\n%d / %d tests passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
