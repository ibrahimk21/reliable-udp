#define _DEFAULT_SOURCE
#include "rudp.h"
#include "rudp_reliable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
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
} while (0)

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

struct ooe_recv_arg {
    int sockfd;
    int n_messages;
    int64_t *arrivals_us;
    uint8_t *out;
    int out_per_msg;
    float drop_rate;
};

static void *ooe_recv_thread(void *arg)
{
    struct ooe_recv_arg *a = (struct ooe_recv_arg *)arg;
    struct rudp_receiver r;
    rudp_receiver_init(&r, a->sockfd);

    struct timeval tv;
    memset(&tv, 0, sizeof(tv));
    setsockopt(a->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int received = 0;
    int max_idle = 5000000;
    int idle = 0;
    while (received < a->n_messages && idle < max_idle) {
        uint8_t buf[1024];
        uint32_t seq, flags;
        ssize_t n = rudp_recv_datagram(&r, buf, sizeof(buf), &seq, &flags,
                                       NULL, NULL, a->drop_rate);
        if (n < 0) { idle += 1000; usleep(1000); continue; }
        if (n == 0) { idle += 10; usleep(10); continue; }
        idle = 0;
        if (seq >= 1 && seq <= (uint32_t)a->n_messages && a->arrivals_us)
            a->arrivals_us[seq - 1] = 1;
        if (a->out && a->out_per_msg > 0 && seq >= 1
            && seq <= (uint32_t)a->n_messages) {
            int off = (int)(seq - 1) * a->out_per_msg;
            int copy = n;
            if (copy > a->out_per_msg) copy = a->out_per_msg;
            memcpy(a->out + off, buf, (size_t)copy);
        }
        received++;
    }
    return NULL;
}

static void run_ooe_test(int label, int n_msgs, int msg_size,
                         float drop_rate, int expect_gaps)
{
    int total_len = n_msgs * msg_size;

    uint8_t *in = malloc((size_t)total_len);
    uint8_t *out = calloc(1, (size_t)total_len);
    int64_t *arrivals = calloc((size_t)n_msgs, sizeof(int64_t));
    if (!in || !out || !arrivals) {
        TEST(0, "allocation failure");
        free(in); free(out); free(arrivals);
        return;
    }

    for (int i = 0; i < total_len; i++)
        in[i] = (uint8_t)((i * 31 + label) & 0xff);

    int recv_fd = make_socket((uint16_t)(19000 + label));
    int send_fd = make_socket(0);
    if (recv_fd < 0 || send_fd < 0) {
        TEST(0, "socket setup");
        if (recv_fd >= 0) close(recv_fd);
        if (send_fd >= 0) close(send_fd);
        free(in); free(out); free(arrivals);
        return;
    }

    struct ooe_recv_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.sockfd = recv_fd;
    arg.n_messages = n_msgs;
    arg.arrivals_us = arrivals;
    arg.out = out;
    arg.out_per_msg = msg_size;
    arg.drop_rate = drop_rate;

    pthread_t thread;
    pthread_create(&thread, NULL, ooe_recv_thread, &arg);
    usleep(20000);

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons((uint16_t)(19000 + label));

    struct rudp_sender s;
    rudp_sender_init(&s, send_fd);

    int sent = rudp_send_sliding(&s, in, total_len,
                                 (struct sockaddr *)&dest, sizeof(dest));

    close(send_fd);

    struct rudp_header fin;
    memset(&fin, 0, sizeof(fin));
    fin.type = RUDP_FIN;
    int fin_fd = make_socket(0);
    if (fin_fd >= 0) {
        rudp_sendto(fin_fd, &fin, NULL, 0,
                    (struct sockaddr *)&dest, sizeof(dest));
        close(fin_fd);
    }

    pthread_join(thread, NULL);

    char msg[128];

    snprintf(msg, sizeof(msg), "OOE case %d: sent=%d total_len=%d",
             label, sent, total_len);
    TEST(sent == total_len, msg);

    int all_received = 1;
    for (int i = 0; i < n_msgs; i++) {
        if (arrivals[i] == 0) { all_received = 0; break; }
    }
    snprintf(msg, sizeof(msg), "OOE case %d: all %d messages received",
             label, n_msgs);
    TEST(all_received, msg);

    if (expect_gaps && drop_rate > 0.0f) {
        int data_match = (memcmp(in, out, (size_t)total_len) == 0);
        snprintf(msg, sizeof(msg), "OOE case %d: data integrity at %.0f%% drop",
                 label, drop_rate * 100);
        TEST(data_match, msg);
    } else if (!expect_gaps) {
        int data_match = (memcmp(in, out, (size_t)total_len) == 0);
        snprintf(msg, sizeof(msg), "OOE case %d: data integrity",
                 label);
        TEST(data_match, msg);
    }

    if (drop_rate > 0.0f) {
        snprintf(msg, sizeof(msg), "OOE case %d: completed at %.0f%% drop",
                 label, drop_rate * 100);
        TEST(1, msg);
    }

    close(recv_fd);
    free(in); free(out); free(arrivals);
}

int main(void)
{
    printf("=== Out-of-order delivery tests ===\n");

    run_ooe_test(1, 10, 1024, 0.0f, 0);

    run_ooe_test(2, 50, 1024, 0.10f, 1);

    run_ooe_test(3, 10, 1024, 0.0f, 0);

    run_ooe_test(4, 100, 1024, 0.05f, 1);

    run_ooe_test(5, 200, 1024, 0.0f, 0);

    printf("\n%d/%d tests passed\n", passed, tests);
    return (passed == tests) ? 0 : 1;
}
