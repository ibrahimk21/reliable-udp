#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MSG_SIZE 1024

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <N> [msg_size]\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    int N = atoi(argv[3]);
    int msg_size = (argc > 4) ? atoi(argv[4]) : MSG_SIZE;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(server_ip);
    server.sin_port = htons((uint16_t)server_port);

    if (connect(sockfd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect"); close(sockfd); return 1;
    }

    uint8_t *msg = malloc((size_t)msg_size);
    if (!msg) { fprintf(stderr, "malloc failed\n"); close(sockfd); return 1; }
    memset(msg, 0, (size_t)msg_size);

    int total_sent = 0;
    for (int seq = 1; seq <= N; seq++) {
        memcpy(msg, &seq, sizeof(seq));
        int remaining = msg_size;
        uint8_t *ptr = msg;
        while (remaining > 0) {
            ssize_t s = send(sockfd, ptr, (size_t)remaining, 0);
            if (s < 0) {
                if (errno == EINTR) continue;
                perror("send"); close(sockfd); free(msg); return 1;
            }
            ptr += s;
            remaining -= (int)s;
        }
        total_sent += msg_size;
    }

    shutdown(sockfd, SHUT_WR);

    fprintf(stderr, "TCP sent %d messages (%d bytes)\n", N, total_sent);
    close(sockfd);
    free(msg);
    return 0;
}
