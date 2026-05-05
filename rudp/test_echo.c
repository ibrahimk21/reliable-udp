#include "rudp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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

int main(void)
{
    int port = 9999;

    /* --- Server socket --- */
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    /* --- Client socket --- */
    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in client_bind;
    memset(&client_bind, 0, sizeof(client_bind));
    client_bind.sin_family      = AF_INET;
    client_bind.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    client_bind.sin_port        = htons(0);
    if (bind(client_fd, (struct sockaddr *)&client_bind, sizeof(client_bind)) < 0) {
        perror("bind client"); return 1;
    }

    /* --- Client sends "hello" to server --- */
    struct rudp_header tx_h;
    memset(&tx_h, 0, sizeof(tx_h));
    tx_h.type    = RUDP_DATA;
    tx_h.seq_num = 1;
    tx_h.ack_num = 0;
    tx_h.window  = 32;

    const char *payload = "hello";
    int ret = rudp_sendto(client_fd, &tx_h, payload, 5,
                          (struct sockaddr *)&server_addr, sizeof(server_addr));
    TEST(ret >= 0, "rudp_sendto succeeds");

    /* --- Server receives --- */
    struct rudp_header rx_h;
    char rx_buf[MAX_PAYLOAD_SIZE];
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);

    int pay_len = rudp_recvfrom(server_fd, &rx_h, rx_buf, sizeof(rx_buf),
                                 (struct sockaddr *)&peer_addr, &peer_len);
    TEST(pay_len >= 0, "rudp_recvfrom succeeds on server");
    TEST(pay_len == 5, "received payload length is 5");
    TEST(rx_h.type == RUDP_DATA, "received type is DATA");
    TEST(rx_h.seq_num == 1, "received seq_num is 1");
    TEST(rx_h.window == 32, "received window is 32");
    TEST(memcmp(rx_buf, "hello", 5) == 0, "received payload matches 'hello'");

    /* --- Server echoes back --- */
    struct rudp_header echo_h;
    memset(&echo_h, 0, sizeof(echo_h));
    echo_h.type    = RUDP_DATA;
    echo_h.seq_num = 100;
    echo_h.ack_num = rx_h.seq_num;

    ret = rudp_sendto(server_fd, &echo_h, rx_buf, pay_len,
                      (struct sockaddr *)&peer_addr, peer_len);
    TEST(ret >= 0, "rudp_sendto echo succeeds");

    /* --- Client receives echo --- */
    struct rudp_header resp_h;
    char resp_buf[MAX_PAYLOAD_SIZE];

    pay_len = rudp_recvfrom(client_fd, &resp_h, resp_buf, sizeof(resp_buf),
                             NULL, NULL);
    TEST(pay_len >= 0, "rudp_recvfrom succeeds on client");
    TEST(pay_len == 5, "echo payload length is 5");
    TEST(resp_h.type == RUDP_DATA, "echo type is DATA");
    TEST(resp_h.ack_num == 1, "echo ack_num is 1");
    TEST(memcmp(resp_buf, "hello", 5) == 0, "echo payload matches 'hello'");

    close(server_fd);
    close(client_fd);

    printf("\n%d / %d tests passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
