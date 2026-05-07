#define _DEFAULT_SOURCE
#include "rudp_reliable.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>

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

struct thread_arg {
    struct rudp_receiver *receiver;
    char   buf[64];
    int    n;
    float  drop_rate;
};

static void *receive_thread(void *arg)
{
    struct thread_arg *a = (struct thread_arg *)arg;
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    a->n = rudp_recv_reliable(a->receiver, a->buf, sizeof(a->buf),
                              (struct sockaddr *)&src, &src_len, a->drop_rate);
    return NULL;
}

static int make_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void convergence_test(void)
{
    int recv_port = 6001;
    int send_port = 6002;

    int recv_fd = make_socket((uint16_t)recv_port);
    int send_fd = make_socket((uint16_t)send_port);
    if (recv_fd < 0 || send_fd < 0) {
        fprintf(stderr, "FAIL: could not bind sockets\n");
        tests++;
        if (recv_fd >= 0) close(recv_fd);
        if (send_fd >= 0) close(send_fd);
        return;
    }

    struct rudp_sender sender;
    struct rudp_receiver receiver;
    rudp_sender_init(&sender, send_fd);
    rudp_receiver_init(&receiver, recv_fd);

    struct sockaddr_in recv_addr;
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recv_addr.sin_port = htons((uint16_t)recv_port);

    int initial_rto = rudp_sender_get_rto(&sender);
    TEST(initial_rto == INITIAL_RTO_MS, "initial RTO is 500ms");

    int prev_rto = initial_rto;
    int converged = 0;
    int total_sends = 10;
    int rtos[12];

    for (int i = 0; i < total_sends; i++) {
        struct thread_arg targ;
        memset(&targ, 0, sizeof(targ));
        targ.receiver = &receiver;
        targ.drop_rate = 0.0f;

        pthread_t thread;
        pthread_create(&thread, NULL, receive_thread, &targ);
        usleep(20000);

        int ret = rudp_send_reliable(&sender, "x", 1,
                                      (struct sockaddr *)&recv_addr,
                                      sizeof(recv_addr));
        pthread_join(thread, NULL);

        int rto = rudp_sender_get_rto(&sender);
        int srtt = rudp_sender_get_srtt(&sender);
        rtos[i] = rto;
        printf("  send %2d: rto=%4d srtt=%3d ret=%d\n", i, rto, srtt, ret);
        fflush(stdout);

        if (rto < prev_rto) converged = 1;
        prev_rto = rto;
    }

    int final_rto = rtos[total_sends - 1];
    TEST(final_rto < INITIAL_RTO_MS, "RTO adapted from initial 500ms");
    TEST(final_rto >= MIN_RTO_MS, "RTO >= MIN_RTO_MS (100ms)");
    TEST(final_rto <= MAX_RTO_MS, "RTO <= MAX_RTO_MS (10000ms)");
    TEST(converged, "RTO decreased at least once (adaptation occurred)");

    int max_jitter = 0;
    for (int i = 1; i < total_sends; i++) {
        int diff = rtos[i] - rtos[i - 1];
        if (diff < 0) diff = -diff;
        if (diff > max_jitter) max_jitter = diff;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "RTO stable (max jitter %dms)", max_jitter);
    TEST(max_jitter < 50, msg);

    close(recv_fd);
    close(send_fd);
}

static void karn_test(void)
{
    int send_port = 6004;

    int send_fd = make_socket((uint16_t)send_port);
    if (send_fd < 0) {
        fprintf(stderr, "FAIL: karn test bind\n");
        tests++;
        return;
    }

    struct rudp_sender sender;
    rudp_sender_init(&sender, send_fd);
    sender.rto_ms = MIN_RTO_MS;

    struct sockaddr_in recv_addr;
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recv_addr.sin_port = htons(6005);

    int ret = rudp_send_reliable(&sender, "y", 1,
                                  (struct sockaddr *)&recv_addr,
                                  sizeof(recv_addr));

    TEST(ret < 0, "karn test: send fails with no receiver (max retransmits)");

    int srtt = rudp_sender_get_srtt(&sender);
    TEST(srtt == 0, "karn test: SRTT not updated after failed retransmits");

    close(send_fd);
}

int main(void)
{
    srand(42);

    printf("=== Convergence test (10 single-packet sends) ===\n");
    convergence_test();

    printf("\n=== Karn's algorithm test (100%% drop forces retransmit) ===\n");
    karn_test();

    printf("\n%d / %d tests passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
