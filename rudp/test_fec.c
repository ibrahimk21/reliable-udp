#define _DEFAULT_SOURCE
#include "fec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int test_xor_roundtrip(void)
{
    int K = 8;
    int pkt_len = 1024;
    uint8_t packets[8][1024];
    for (int i = 0; i < K; i++)
        for (int j = 0; j < pkt_len; j++)
            packets[i][j] = (uint8_t)((i * 31 + j * 17) & 0xff);

    const uint8_t *ptrs[8];
    for (int i = 0; i < K; i++) ptrs[i] = packets[i];

    uint8_t parity[1024];
    fec_xor_parity(ptrs, K, pkt_len, parity);

    int zeros = 0;
    for (int j = 0; j < pkt_len; j++)
        if (parity[j] == 0) zeros++;
    TEST(zeros < pkt_len, "XOR parity is not all zero on non-trivial data");
    return 0;
}

static int test_xor_recover_single(void)
{
    int K = 8;
    int pkt_len = 1024;
    uint8_t packets[8][1024];
    for (int i = 0; i < K; i++)
        for (int j = 0; j < pkt_len; j++)
            packets[i][j] = (uint8_t)((i * 31 + j * 17) & 0xff);

    const uint8_t *ptrs[8];
    for (int i = 0; i < K; i++) ptrs[i] = packets[i];

    uint8_t parity[1024];
    fec_xor_parity(ptrs, K, pkt_len, parity);

    int missing = 3;
    uint8_t recovered[1024];
    const uint8_t *present[7];
    int n = 0;
    for (int i = 0; i < K; i++) {
        if (i == missing) continue;
        present[n++] = packets[i];
    }
    fec_xor_recover(recovered, present, n, missing, pkt_len, parity);

    TEST(memcmp(recovered, packets[missing], pkt_len) == 0,
         "Recover missing packet 3 via XOR with parity");

    missing = 0;
    n = 0;
    for (int i = 0; i < K; i++) {
        if (i == missing) continue;
        present[n++] = packets[i];
    }
    fec_xor_recover(recovered, present, n, missing, pkt_len, parity);
    TEST(memcmp(recovered, packets[0], pkt_len) == 0,
         "Recover missing packet 0 via XOR with parity");

    missing = 7;
    n = 0;
    for (int i = 0; i < K; i++) {
        if (i == missing) continue;
        present[n++] = packets[i];
    }
    fec_xor_recover(recovered, present, n, missing, pkt_len, parity);
    TEST(memcmp(recovered, packets[7], pkt_len) == 0,
         "Recover missing packet 7 (last) via XOR with parity");
    return 0;
}

static int test_xor_recover_varlen(void)
{
    int K = 4;
    int pkt_len = 1024;
    uint8_t packets[4][1024];
    memset(packets, 0, sizeof(packets));
    for (int i = 0; i < K; i++)
        for (int j = 0; j < 100 + i * 50; j++)
            packets[i][j] = (uint8_t)((i + j) & 0xff);

    const uint8_t *ptrs[4];
    for (int i = 0; i < K; i++) ptrs[i] = packets[i];

    uint8_t parity[1024];
    fec_xor_parity(ptrs, K, pkt_len, parity);

    int missing = 2;
    uint8_t recovered[1024];
    const uint8_t *present[3];
    int n = 0;
    for (int i = 0; i < K; i++) {
        if (i == missing) continue;
        present[n++] = packets[i];
    }
    fec_xor_recover(recovered, present, n, missing, pkt_len, parity);
    TEST(memcmp(recovered, packets[missing], pkt_len) == 0,
         "XOR recover works when all packets padded to max length");
    return 0;
}

static int test_xor_idempotent(void)
{
    int K = 8;
    int pkt_len = 1024;
    uint8_t packets[8][1024];
    for (int i = 0; i < K; i++)
        for (int j = 0; j < pkt_len; j++)
            packets[i][j] = (uint8_t)(i ^ j);

    const uint8_t *ptrs[8];
    for (int i = 0; i < K; i++) ptrs[i] = packets[i];

    uint8_t parity[1024];
    fec_xor_parity(ptrs, K, pkt_len, parity);

    uint8_t reconstructed[8][1024];
    for (int i = 0; i < K; i++)
        memcpy(reconstructed[i], packets[i], pkt_len);

    int missing = 4;
    const uint8_t *present[7];
    int n = 0;
    for (int i = 0; i < K; i++) {
        if (i == missing) continue;
        present[n++] = packets[i];
    }
    uint8_t recovered[1024];
    fec_xor_recover(recovered, present, n, missing, pkt_len, parity);
    memcpy(reconstructed[missing], recovered, pkt_len);

    const uint8_t *rptrs[8];
    for (int i = 0; i < K; i++) rptrs[i] = reconstructed[i];
    uint8_t parity2[1024];
    fec_xor_parity(rptrs, K, pkt_len, parity2);
    TEST(memcmp(parity, parity2, pkt_len) == 0,
         "XOR parity is idempotent after recovery");
    return 0;
}

static int test_xor_noop_when_all_present(void)
{
    int K = 8;
    int pkt_len = 1024;
    uint8_t packets[8][1024];
    for (int i = 0; i < K; i++)
        for (int j = 0; j < pkt_len; j++)
            packets[i][j] = (uint8_t)((i * 7 + j) & 0xff);

    const uint8_t *ptrs[8];
    for (int i = 0; i < K; i++) ptrs[i] = packets[i];

    uint8_t parity[1024];
    fec_xor_parity(ptrs, K, pkt_len, parity);

    int non_zero = 0;
    for (int j = 0; j < pkt_len; j++)
        if (parity[j] != 0) { non_zero++; break; }
    TEST(non_zero > 0, "XOR parity on random data is not all-zero");
    return 0;
}

static int test_xor_single_packet_block(void)
{
    int K = 1;
    int pkt_len = 1024;
    uint8_t pkt[1024];
    for (int j = 0; j < pkt_len; j++) pkt[j] = (uint8_t)j;

    const uint8_t *ptrs[1] = { pkt };
    uint8_t parity[1024];
    fec_xor_parity(ptrs, K, pkt_len, parity);
    TEST(memcmp(parity, pkt, pkt_len) == 0,
         "K=1 parity equals the single data packet");
    return 0;
}

int main(void)
{
    printf("=== FEC unit tests (XOR parity) ===\n");
    test_xor_roundtrip();
    test_xor_recover_single();
    test_xor_recover_varlen();
    test_xor_idempotent();
    test_xor_noop_when_all_present();
    test_xor_single_packet_block();
    printf("\n%d / %d tests passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
