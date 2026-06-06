#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define CHUNK_SIZE 1024
#define BUF_SIZE 65536
#define DEFAULT_RETX_DELAY_US 100000

static int g_drop_pct = 0;
static int g_listen_port = 0;
static int g_upstream_port = 0;
static char g_upstream_host[64] = "127.0.0.1";
static int g_is_udp = 0;
static int g_retx_delay_us = DEFAULT_RETX_DELAY_US;

static int should_drop(void) {
    if (g_drop_pct <= 0) return 0;
    int r = rand() % 100;
    return r < g_drop_pct;
}

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-proto") == 0 && i + 1 < argc) {
            g_is_udp = (strcmp(argv[i + 1], "udp") == 0);
            i++;
        } else if (strcmp(argv[i], "-listen") == 0 && i + 1 < argc) {
            g_listen_port = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-upstream") == 0 && i + 1 < argc) {
            char *colon = strchr(argv[i + 1], ':');
            if (colon) {
                *colon = '\0';
                strncpy(g_upstream_host, argv[i + 1], 63);
                g_upstream_host[63] = '\0';
                g_upstream_port = atoi(colon + 1);
            }
            i++;
        } else if (strcmp(argv[i], "-drop") == 0 && i + 1 < argc) {
            g_drop_pct = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-seed") == 0 && i + 1 < argc) {
            srand((unsigned int)atoi(argv[i + 1]));
            i++;
        } else if (strcmp(argv[i], "-retx-delay") == 0 && i + 1 < argc) {
            g_retx_delay_us = atoi(argv[i + 1]);
            i++;
        }
    }
}

static int make_listen_udp(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_listen_port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(s);
        return -1;
    }
    return s;
}



static int make_listen_tcp(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_listen_port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(s);
        return -1;
    }
    if (listen(s, 1) < 0) {
        perror("listen");
        close(s);
        return -1;
    }
    return s;
}

static int connect_upstream_tcp(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_aton(g_upstream_host, &addr.sin_addr);
    addr.sin_port = htons((uint16_t)g_upstream_port);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(s);
        return -1;
    }
    return s;
}

static void run_udp(void) {
    int srv = make_listen_udp();
    if (srv < 0) exit(1);

    struct sockaddr_in upstream;
    memset(&upstream, 0, sizeof(upstream));
    upstream.sin_family = AF_INET;
    inet_aton(g_upstream_host, &upstream.sin_addr);
    upstream.sin_port = htons((uint16_t)g_upstream_port);

    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    int sender_known = 0;

    char buf[BUF_SIZE];
    struct sockaddr_in peer;
    socklen_t peer_len;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        int maxfd = srv + 1;

        struct timeval tv = {0, 200000};
        int r = select(maxfd, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        if (FD_ISSET(srv, &rfds)) {
            peer_len = sizeof(peer);
            ssize_t n = recvfrom(srv, buf, BUF_SIZE, 0,
                                 (struct sockaddr *)&peer, &peer_len);
            if (n <= 0) continue;

            int from_upstream = (peer.sin_addr.s_addr == upstream.sin_addr.s_addr &&
                                 peer.sin_port == upstream.sin_port);

            if (from_upstream) {
                if (sender_known) {
                    sendto(srv, buf, n, 0,
                           (struct sockaddr *)&sender, sender_len);
                }
            } else {
                if (!sender_known) {
                    sender = peer;
                    sender_known = 1;
                }
                if (!should_drop()) {
                    sendto(srv, buf, n, 0,
                           (struct sockaddr *)&upstream, sizeof(upstream));
                }
            }
        }
    }

    close(srv);
}

typedef struct {
    int a;
    int b;
    int drop;
} pipe_args_t;

static void *pipe_thread(void *arg) {
    pipe_args_t *p = (pipe_args_t *)arg;
    char buf[BUF_SIZE];
    while (1) {
        ssize_t n = recv(p->a, buf, BUF_SIZE, 0);
        if (n <= 0) break;

        if (p->drop) {
            ssize_t offset = 0;
            while (offset < n) {
                ssize_t chunk = n - offset;
                if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;
                if (should_drop()) {
                    usleep((useconds_t)g_retx_delay_us);
                }
                ssize_t sent = 0;
                while (sent < chunk) {
                    ssize_t s = send(p->b, buf + offset + sent,
                                     chunk - sent, 0);
                    if (s <= 0) break;
                    sent += s;
                }
                offset += chunk;
            }
        } else {
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t s = send(p->b, buf + sent, n - sent, 0);
                if (s <= 0) break;
                sent += s;
            }
        }
    }
    shutdown(p->b, SHUT_WR);
    return NULL;
}

static void run_tcp(void) {
    int srv = make_listen_tcp();
    if (srv < 0) exit(1);

    int cli = accept(srv, NULL, NULL);
    if (cli < 0) { perror("accept"); close(srv); exit(1); }

    int ups = connect_upstream_tcp();
    if (ups < 0) { close(srv); close(cli); exit(1); }

    pipe_args_t forward = {cli, ups, 1};
    pipe_args_t backward = {ups, cli, 0};

    pthread_t t1, t2;
    pthread_create(&t1, NULL, pipe_thread, &forward);
    pthread_create(&t2, NULL, pipe_thread, &backward);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    close(cli);
    close(ups);
    close(srv);
}

int main(int argc, char **argv) {
    parse_args(argc, argv);

    if (g_listen_port == 0 || g_upstream_port == 0) {
        fprintf(stderr, "Usage: %s -proto tcp|udp -listen PORT -upstream HOST:PORT "
                "-drop PCT -seed S [-retx-delay US]\n"
                "  For UDP: drops only in the sender->receiver direction.\n"
                "  For TCP: simulates loss cost by sleeping then forwarding.\n",
                argv[0]);
        return 1;
    }

    if (g_is_udp) {
        run_udp();
    } else {
        run_tcp();
    }

    return 0;
}
