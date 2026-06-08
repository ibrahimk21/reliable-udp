#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include "../rudp/rudp.h"
#include "../rudp/rudp_reliable.h"

#define MSG_SIZE 1024
#define IDLE_TIMEOUT_MS 30000

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <port> <N>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    int N = atoi(argv[2]);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int rcvbuf = 4194304;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sockfd); return 1;
    }

    int64_t *arrivals = calloc((size_t)N, sizeof(int64_t));
    if (!arrivals) { fprintf(stderr, "malloc failed\n"); close(sockfd); return 1; }

    struct rudp_receiver r;
    rudp_receiver_init(&r, sockfd);

    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLIN;

    int received = 0;
    int idle_ms = 0;

    while (received < N && idle_ms < IDLE_TIMEOUT_MS) {
        int ret = poll(&pfd, 1, 100);
        if (ret < 0) break;
        if (ret == 0) { idle_ms += 100; continue; }
        idle_ms = 0;

        uint8_t buf[MSG_SIZE];
        uint32_t seq, flags;
        ssize_t n = rudp_recv_datagram(&r, buf, sizeof(buf), &seq, &flags,
                                       NULL, NULL, 0.0f);
        if (n <= 0) continue;

        if (seq >= 1 && seq <= (uint32_t)N && arrivals[seq - 1] == 0) {
            arrivals[seq - 1] = now_us();
            received++;
        }
    }

    int64_t first_ts = 0;
    for (int i = 0; i < N; i++) {
        if (arrivals[i] > 0) { first_ts = arrivals[i]; break; }
    }

    for (int i = 0; i < N; i++) {
        int64_t rel = (arrivals[i] > 0 && first_ts > 0)
                      ? arrivals[i] - first_ts : -1;
        printf("%d,%ld\n", i + 1, (long)rel);
    }

    fprintf(stderr, "OOE RUDP: received %d/%d messages\n", received, N);
    close(sockfd);
    free(arrivals);
    return (received == N) ? 0 : 1;
}
