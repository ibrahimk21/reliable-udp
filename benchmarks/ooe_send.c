#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../rudp/rudp.h"
#include "../rudp/rudp_reliable.h"

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

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(server_ip);
    dest.sin_port = htons((uint16_t)server_port);

    uint8_t *msg = malloc((size_t)msg_size);
    if (!msg) { fprintf(stderr, "malloc failed\n"); close(sockfd); return 1; }
    memset(msg, 0, (size_t)msg_size);

    struct rudp_sender s;
    rudp_sender_init(&s, sockfd);

    int total_sent = 0;
    for (int seq = 1; seq <= N; seq++) {
        memcpy(msg, &seq, sizeof(seq));
        int ret = rudp_send_sliding(&s, msg, msg_size,
                                    (struct sockaddr *)&dest, sizeof(dest));
        if (ret != msg_size) {
            fprintf(stderr, "Failed to send message %d (sent=%d)\n", seq, ret);
            close(sockfd);
            free(msg);
            return 1;
        }
        total_sent += ret;
    }

    fprintf(stderr, "Sent %d messages (%d bytes)\n", N, total_sent);
    close(sockfd);
    free(msg);
    return 0;
}
