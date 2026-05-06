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
    int    expect_fail;
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

static void run_test(float drop_rate, const char *payload, int len,
                     int label, int expect_fail)
{
    int recv_port = 9000 + label;
    int send_port = 10000 + label;

    int recv_fd = make_socket((uint16_t)recv_port);
    int send_fd = make_socket((uint16_t)send_port);
    if (recv_fd < 0 || send_fd < 0) {
        fprintf(stderr, "FAIL: could not bind sockets for test %d\n", label);
        if (recv_fd >= 0) close(recv_fd);
        if (send_fd >= 0) close(send_fd);
        tests++;
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

    struct thread_arg targ;
    memset(&targ, 0, sizeof(targ));
    targ.receiver = &receiver;
    targ.drop_rate = drop_rate;
    targ.expect_fail = expect_fail;

    pthread_t thread;
    memset(&thread, 0, sizeof(thread));
    int thread_started = 0;

    if (!expect_fail) {
        pthread_create(&thread, NULL, receive_thread, &targ);
        usleep(50000);
        thread_started = 1;
    }

    int ret = rudp_send_reliable(&sender, payload, len,
                                  (struct sockaddr *)&recv_addr,
                                  sizeof(recv_addr));

    char msg[128];
    snprintf(msg, sizeof(msg), "test %d (%d%% drop): send returns %s",
             label, (int)(drop_rate * 100),
             expect_fail ? "expected error" : "success");
    if (expect_fail)
        TEST(ret < 0, msg);
    else
        TEST(ret == len, msg);

    if (thread_started)
        pthread_join(thread, NULL);

    if (!expect_fail) {
        snprintf(msg, sizeof(msg), "test %d: recv returns %d bytes", label, len);
        TEST(targ.n == len, msg);

        snprintf(msg, sizeof(msg), "test %d: payload matches", label);
        TEST(memcmp(targ.buf, payload, len) == 0, msg);
    }

    close(recv_fd);
    close(send_fd);
}

int main(void)
{
    srand(42);

    run_test(0.0f,  "hello",     5,  1, 0);
    run_test(0.3f,  "world",     5,  2, 0);
    run_test(0.5f,  "fifty",     5,  3, 0);
    run_test(0.8f,  "eight",     5,  4, 0);
    run_test(0.0f,  "abc",       3,  5, 0);
    run_test(1.0f,  "dead",      4,  6, 1);

    printf("\n%d / %d tests passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
