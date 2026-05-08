#define _DEFAULT_SOURCE
#include "rudp.h"
#include "rudp_reliable.h"
#include "rudp_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <port> <output_file> [-drop N]\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    const char *out_path = argv[2];
    float drop_rate = 0.0f;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-drop") == 0 && i + 1 < argc) {
            drop_rate = atof(argv[i + 1]) / 100.0f;
            i++;
        }
    }

    fprintf(stderr, "Listening on port %d, output=%s, drop_rate=%.0f%%\n",
            port, out_path, drop_rate * 100);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    uint8_t meta_buf[1024];
    struct rudp_header h;
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);
    int n;

    fprintf(stderr, "Waiting for sender...\n");
    while (1) {
        n = rudp_recvfrom(sockfd, &h, meta_buf, sizeof(meta_buf),
                          (struct sockaddr *)&client, &client_len);
        if (n < 0) continue;
        if (h.type == RUDP_SYN) break;
    }

    if (n < (int)(sizeof(uint32_t) + sizeof(uint64_t))) {
        fprintf(stderr, "Invalid metadata length %d\n", n);
        close(sockfd);
        return 1;
    }

    struct rudp_file_metadata *meta = (struct rudp_file_metadata *)meta_buf;
    if (meta->magic != RUDP_FILE_MAGIC) {
        fprintf(stderr, "Bad magic: 0x%08x\n", meta->magic);
        close(sockfd);
        return 1;
    }

    uint64_t file_size = meta->file_size;
    fprintf(stderr, "Receiving %s (%llu bytes)\n",
            meta->filename, (unsigned long long)file_size);

    uint8_t *file_data = NULL;
    if (file_size > 0) {
        file_data = malloc((size_t)file_size);
        if (!file_data) {
            fprintf(stderr, "malloc failed\n");
            close(sockfd);
            return 1;
        }
    }

    struct rudp_receiver receiver;
    rudp_receiver_init(&receiver, sockfd);

    int total = 0;
    if (file_size > 0) {
        total = rudp_recv_sliding(&receiver, file_data, (int)file_size,
                                   NULL, NULL, drop_rate);
    }
    fprintf(stderr, "Received %d bytes\n", total);

    FILE *fout = fopen(out_path, "wb");
    if (fout) {
        if (file_data && total > 0) {
            fwrite(file_data, 1, (size_t)total, fout);
        }
        fclose(fout);
    }

    while (1) {
        n = rudp_recvfrom(sockfd, &h, NULL, 0, NULL, NULL);
        if (n < 0) continue;
        if (h.type == RUDP_FIN) break;
    }
    fprintf(stderr, "Transfer complete: %s\n", out_path);

    if (file_data) free(file_data);
    close(sockfd);
    return 0;
}
