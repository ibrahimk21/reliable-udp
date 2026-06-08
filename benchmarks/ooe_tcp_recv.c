#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

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
        fprintf(stderr, "Usage: %s <port> <N> [msg_size]\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    int N = atoi(argv[2]);
    int msg_size = (argc > 3) ? atoi(argv[3]) : MSG_SIZE;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); return 1;
    }
    if (listen(srv, 1) < 0) {
        perror("listen"); close(srv); return 1;
    }

    int conn = accept(srv, NULL, NULL);
    if (conn < 0) { perror("accept"); close(srv); return 1; }
    close(srv);

    int64_t *arrivals = calloc((size_t)N, sizeof(int64_t));
    if (!arrivals) { close(conn); return 1; }

    struct pollfd pfd;
    pfd.fd = conn;
    pfd.events = POLLIN;

    uint8_t *buf = malloc((size_t)msg_size);
    if (!buf) { free(arrivals); close(conn); return 1; }

    int received = 0;
    int idle_ms = 0;

    while (received < N && idle_ms < IDLE_TIMEOUT_MS) {
        int ret = poll(&pfd, 1, 100);
        if (ret < 0) break;
        if (ret == 0) { idle_ms += 100; continue; }
        idle_ms = 0;

        int total = 0;
        while (total < msg_size) {
            ssize_t n = recv(conn, buf + total, (size_t)(msg_size - total), 0);
            if (n <= 0) goto done;
            total += n;
        }

        arrivals[received] = now_us();
        received++;
    }

done:
    ;
    int64_t first_ts = 0;
    for (int i = 0; i < received && i < N; i++) {
        if (arrivals[i] > 0) { first_ts = arrivals[i]; break; }
    }

    for (int i = 0; i < N; i++) {
        int64_t rel = (i < received && arrivals[i] > 0 && first_ts > 0)
                      ? arrivals[i] - first_ts : -1;
        printf("%d,%ld\n", i + 1, (long)rel);
    }

    fprintf(stderr, "OOE TCP: received %d/%d messages\n", received, N);
    close(conn);
    free(arrivals);
    free(buf);
    return (received == N) ? 0 : 1;
}
