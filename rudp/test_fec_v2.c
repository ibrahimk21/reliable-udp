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

struct recv_arg {
    int sockfd;
    uint8_t *buf;
    int len;
    float drop_rate;
    int result;
};

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

static void *recv_thread(void *arg)
{
    struct recv_arg *a = (struct recv_arg *)arg;
    struct rudp_receiver r;
    rudp_receiver_init(&r, a->sockfd);
    rudp_receiver_set_fec_v2(&r, 8, 1);
    a->result = rudp_recv_block_fec(&r, a->buf, a->len,
                                    NULL, NULL, a->drop_rate);
    return NULL;
}

static void run_case(int label, int data_len, float drop_rate)
{
    uint8_t *in = malloc((size_t)data_len);
    uint8_t *out = calloc(1, (size_t)data_len);
    if (!in || !out) {
        TEST(0, "allocation failure");
        free(in);
        free(out);
        return;
    }
    for (int i = 0; i < data_len; i++) in[i] = (uint8_t)((i * 31 + label) & 0xff);

    int recv_fd = make_socket((uint16_t)(18000 + label));
    int send_fd = make_socket(0);
    if (recv_fd < 0 || send_fd < 0) {
        TEST(0, "socket setup");
        if (recv_fd >= 0) close(recv_fd);
        if (send_fd >= 0) close(send_fd);
        free(in);
        free(out);
        return;
    }

    struct recv_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.sockfd = recv_fd;
    arg.buf = out;
    arg.len = data_len;
    arg.drop_rate = drop_rate;

    pthread_t thread;
    pthread_create(&thread, NULL, recv_thread, &arg);
    usleep(20000);

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons((uint16_t)(18000 + label));

    struct rudp_sender s;
    rudp_sender_init(&s, send_fd);
    rudp_sender_set_fec_v2(&s, 8, 1);

    int sent = rudp_send_block_fec(&s, in, data_len,
                                   (struct sockaddr *)&dest, sizeof(dest));

    struct rudp_header fin;
    memset(&fin, 0, sizeof(fin));
    fin.type = RUDP_FIN;
    rudp_sendto(send_fd, &fin, NULL, 0, (struct sockaddr *)&dest, sizeof(dest));

    pthread_join(thread, NULL);

    char msg[128];
    snprintf(msg, sizeof(msg), "FECv2 case %d: %d bytes at %d%% drop",
             label, data_len, (int)(drop_rate * 100));
    TEST(sent == data_len && arg.result == data_len &&
         memcmp(in, out, (size_t)data_len) == 0, msg);

    close(recv_fd);
    close(send_fd);
    free(in);
    free(out);
}

int main(void)
{
    srand(42);

    printf("=== Block-ACK FEC v2 tests ===\n");
    run_case(1, 8 * MAX_PAYLOAD_SIZE, 0.0f);
    run_case(2, 12345, 0.0f);
    run_case(3, 50 * 1024, 0.10f);
    run_case(4, 50 * 1024, 0.20f);

    printf("\n%d / %d tests passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
