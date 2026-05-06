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
    void  *buf;
    int    buf_size;
    int    result;
    float  drop_rate;
};

static void *receive_thread(void *arg)
{
    struct thread_arg *a = (struct thread_arg *)arg;
    a->result = rudp_recv_sliding(a->receiver, a->buf, a->buf_size,
                                   NULL, NULL, a->drop_rate);
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

static void run_sliding_test(const uint8_t *data, int data_len,
                              float drop_rate, int label)
{
    int recv_port = 8000 + label;
    int send_port = 11000 + label;

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

    uint8_t *recv_buf = malloc(data_len);
    memset(recv_buf, 0, data_len);

    struct thread_arg targ;
    memset(&targ, 0, sizeof(targ));
    targ.receiver = &receiver;
    targ.buf = recv_buf;
    targ.buf_size = data_len;
    targ.drop_rate = drop_rate;

    pthread_t thread;
    pthread_create(&thread, NULL, receive_thread, &targ);
    usleep(50000);

    int ret = rudp_send_sliding(&sender, data, data_len,
                                 (struct sockaddr *)&recv_addr,
                                 sizeof(recv_addr));

    pthread_join(thread, NULL);

    char msg[128];
    snprintf(msg, sizeof(msg), "test %d (%d%% drop): send returns %d bytes",
             label, (int)(drop_rate * 100), ret);
    TEST(ret == data_len, msg);

    int recv_ok = (targ.result == data_len);
    snprintf(msg, sizeof(msg), "test %d: recv returns %d bytes",
             label, targ.result);
    TEST(recv_ok, msg);

    int match = (recv_ok && memcmp(data, recv_buf, data_len) == 0);
    snprintf(msg, sizeof(msg), "test %d: data integrity check", label);
    TEST(match, msg);

    free(recv_buf);
    close(recv_fd);
    close(send_fd);
}

int main(void)
{
    srand(42);

    int data_len = 5000;
    uint8_t *data = malloc(data_len);
    for (int i = 0; i < data_len; i++)
        data[i] = (uint8_t)(i % 251);

    run_sliding_test(data, data_len, 0.0f, 1);
    run_sliding_test(data, data_len, 0.2f, 2);
    run_sliding_test(data, data_len, 0.4f, 3);

    free(data);

    printf("\n%d / %d tests passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
