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
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <file_path>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    const char *file_path = argv[3];

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *file_data = NULL;
    if (file_size > 0) {
        file_data = malloc((size_t)file_size);
        if (fread(file_data, 1, (size_t)file_size, f) != (size_t)file_size) {
            perror("fread");
            free(file_data);
            fclose(f);
            return 1;
        }
    }
    fclose(f);

    const char *filename = strrchr(file_path, '/');
    if (filename) filename++;
    else filename = file_path;

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        free(file_data);
        return 1;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(0);
    if (bind(sockfd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        close(sockfd);
        free(file_data);
        return 1;
    }

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(server_ip);
    server.sin_port = htons((uint16_t)server_port);

    struct rudp_sender sender;
    rudp_sender_init(&sender, sockfd);

    struct rudp_file_metadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = RUDP_FILE_MAGIC;
    meta.file_size = (uint64_t)file_size;
    strncpy(meta.filename, filename, RUDP_FILE_MAX_NAME - 1);

    struct rudp_header h;
    memset(&h, 0, sizeof(h));
    h.type = RUDP_SYN;
    h.seq_num = 0;
    if (rudp_sendto(sockfd, &h, &meta, sizeof(meta),
                    (struct sockaddr *)&server, sizeof(server)) < 0) {
        fprintf(stderr, "SYN send failed\n");
        close(sockfd);
        free(file_data);
        return 1;
    }

    fprintf(stderr, "Sending %s (%ld bytes)...\n", filename, file_size);

    int ret = 0;
    if (file_size > 0) {
        ret = rudp_send_sliding(&sender, file_data, (int)file_size,
                                 (struct sockaddr *)&server, sizeof(server));
        if (ret < 0) {
            fprintf(stderr, "send failed\n");
            close(sockfd);
            free(file_data);
            return 1;
        }
    }
    fprintf(stderr, "Sent %d bytes\n", ret);

    memset(&h, 0, sizeof(h));
    h.type = RUDP_FIN;
    h.seq_num = 0;
    rudp_sendto(sockfd, &h, NULL, 0,
                (struct sockaddr *)&server, sizeof(server));

    free(file_data);
    close(sockfd);
    return 0;
}
