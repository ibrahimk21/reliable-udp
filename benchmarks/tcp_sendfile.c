#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define CHUNK_SIZE 1024
#define BUF_SIZE 65536

int main(int argc, char *argv[]) {
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
        if (!file_data) {
            fprintf(stderr, "malloc failed\n");
            fclose(f);
            return 1;
        }
        if (fread(file_data, 1, (size_t)file_size, f) != (size_t)file_size) {
            perror("fread");
            free(file_data);
            fclose(f);
            return 1;
        }
    }
    fclose(f);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        free(file_data);
        return 1;
    }

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(server_ip);
    server.sin_port = htons((uint16_t)server_port);

    if (connect(sockfd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect");
        close(sockfd);
        free(file_data);
        return 1;
    }

    fprintf(stderr, "Sending %ld bytes via TCP in 1KB chunks...\n", file_size);

    long sent_total = 0;
    while (sent_total < file_size) {
        long to_send = file_size - sent_total;
        if (to_send > CHUNK_SIZE) to_send = CHUNK_SIZE;
        ssize_t s = send(sockfd, file_data + sent_total, (size_t)to_send, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            perror("send");
            close(sockfd);
            free(file_data);
            return 1;
        }
        if (s == 0) break;
        sent_total += s;
    }

    fprintf(stderr, "Sent %ld bytes\n", sent_total);

    shutdown(sockfd, SHUT_WR);

    free(file_data);
    close(sockfd);
    return 0;
}
