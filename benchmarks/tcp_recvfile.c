#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUF_SIZE 65536

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <port> <output_file>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    const char *out_path = argv[2];

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv);
        return 1;
    }
    if (listen(srv, 1) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    fprintf(stderr, "Listening on port %d, output=%s\n", port, out_path);

    int conn = accept(srv, NULL, NULL);
    if (conn < 0) {
        perror("accept");
        close(srv);
        return 1;
    }

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        perror("fopen");
        close(conn);
        close(srv);
        return 1;
    }

    char buf[BUF_SIZE];
    long total = 0;
    while (1) {
        ssize_t n = recv(conn, buf, BUF_SIZE, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            break;
        }
        if (n == 0) break;
        if (fwrite(buf, 1, (size_t)n, fout) != (size_t)n) {
            perror("fwrite");
            break;
        }
        total += n;
    }

    fprintf(stderr, "Received %ld bytes\n", total);

    fclose(fout);
    close(conn);
    close(srv);
    return 0;
}
