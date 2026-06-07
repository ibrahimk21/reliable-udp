#define _DEFAULT_SOURCE
#include "rudp.h"
#include "rudp_reliable.h"
#include "rudp_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
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

struct receiver_arg {
    int sockfd;
    char out_path[64];
    float drop_rate;
    int result;
    int fec_mode;
};

static void *receiver_thread(void *arg)
{
    struct receiver_arg *a = (struct receiver_arg *)arg;

    uint8_t meta_buf[1024];
    struct rudp_header h;
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);
    int n;

    while (1) {
        n = rudp_recvfrom(a->sockfd, &h, meta_buf, sizeof(meta_buf),
                          (struct sockaddr *)&client, &client_len);
        if (n < 0) continue;
        if (h.type == RUDP_SYN) break;
    }

    if (n < (int)(sizeof(uint32_t) + sizeof(uint64_t))) {
        fprintf(stderr, "[recv] bad metadata length\n");
        a->result = -1;
        return NULL;
    }

    struct rudp_file_metadata *meta = (struct rudp_file_metadata *)meta_buf;
    if (meta->magic != RUDP_FILE_MAGIC) {
        fprintf(stderr, "[recv] bad magic\n");
        a->result = -1;
        return NULL;
    }

    uint64_t file_size = meta->file_size;
    fprintf(stderr, "[recv] receiving %s (%llu bytes)\n",
            meta->filename, (unsigned long long)file_size);

    uint8_t *file_data = NULL;
    if (file_size > 0) {
        file_data = malloc((size_t)file_size);
    }

    struct rudp_receiver receiver;
    rudp_receiver_init(&receiver, a->sockfd);
    if (a->fec_mode == 2) rudp_receiver_set_fec_v2(&receiver, 8, 1);
    else if (a->fec_mode == 1) rudp_receiver_set_fec(&receiver, 8, 1);

    if (file_size > 0) {
        if (a->fec_mode == 2)
            a->result = rudp_recv_block_fec(&receiver, file_data, (int)file_size,
                                            NULL, NULL, a->drop_rate);
        else if (a->fec_mode == 1)
            a->result = rudp_recv_fec_sliding(&receiver, file_data, (int)file_size,
                                               NULL, NULL, a->drop_rate);
        else
            a->result = rudp_recv_sliding(&receiver, file_data, (int)file_size,
                                          NULL, NULL, a->drop_rate);
    } else {
        a->result = 0;
    }

    FILE *fout = fopen(a->out_path, "wb");
    if (fout) {
        if (file_data && a->result > 0) {
            fwrite(file_data, 1, (size_t)a->result, fout);
        }
        fclose(fout);
    }

    if (a->fec_mode == 0) {
        while (1) {
            n = rudp_recvfrom(a->sockfd, &h, NULL, 0, NULL, NULL);
            if (n < 0) continue;
            if (h.type == RUDP_FIN) break;
        }
    }

    if (file_data) free(file_data);
    return NULL;
}

static int make_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void run_transfer(int label, const uint8_t *data, int data_size,
                          float drop_rate, int fec_mode)
{
    char in_path[64], out_path[64];
    snprintf(in_path,  sizeof(in_path),  "/tmp/transfer_in_%d.bin",  label);
    snprintf(out_path, sizeof(out_path), "/tmp/transfer_out_%d.bin", label);

    FILE *f = fopen(in_path, "wb");
    fwrite(data, 1, (size_t)data_size, f);
    fclose(f);

    int recv_port = 15000 + label;

    int recv_fd = make_socket((uint16_t)recv_port);
    int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_fd < 0 || send_fd < 0) {
        fprintf(stderr, "FAIL: bind failure on test %d\n", label);
        if (recv_fd >= 0) close(recv_fd);
        if (send_fd >= 0) close(send_fd);
        tests++;
        remove(in_path);
        return;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(0);
    bind(send_fd, (struct sockaddr *)&local, sizeof(local));

    struct receiver_arg rarg;
    memset(&rarg, 0, sizeof(rarg));
    rarg.sockfd = recv_fd;
    snprintf(rarg.out_path, sizeof(rarg.out_path), "%s", out_path);
    rarg.drop_rate = drop_rate;
    rarg.fec_mode = fec_mode;

    pthread_t thread;
    pthread_create(&thread, NULL, receiver_thread, &rarg);
    usleep(20000);

    struct rudp_sender sender;
    rudp_sender_init(&sender, send_fd);
    if (fec_mode == 2) rudp_sender_set_fec_v2(&sender, 8, 1);
    else if (fec_mode == 1) rudp_sender_set_fec(&sender, 8, 1);

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server.sin_port = htons((uint16_t)recv_port);

    struct rudp_file_metadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = RUDP_FILE_MAGIC;
    meta.file_size = (uint64_t)data_size;
    snprintf(meta.filename, sizeof(meta.filename), "test_%d.bin", label);

    struct rudp_header h;
    memset(&h, 0, sizeof(h));
    h.type = RUDP_SYN;
    h.seq_num = 0;
    rudp_sendto(send_fd, &h, &meta, sizeof(meta),
                (struct sockaddr *)&server, sizeof(server));

    int ret = 0;
    if (data_size > 0) {
        if (fec_mode == 2)
            ret = rudp_send_block_fec(&sender, data, data_size,
                                      (struct sockaddr *)&server, sizeof(server));
        else if (fec_mode == 1)
            ret = rudp_send_fec_sliding(&sender, data, data_size,
                                         (struct sockaddr *)&server, sizeof(server));
        else
            ret = rudp_send_sliding(&sender, data, data_size,
                                    (struct sockaddr *)&server, sizeof(server));
    }

    memset(&h, 0, sizeof(h));
    h.type = RUDP_FIN;
    h.seq_num = 0;
    rudp_sendto(send_fd, &h, NULL, 0,
                (struct sockaddr *)&server, sizeof(server));

    pthread_join(thread, NULL);

    char msg[128];
    const char *mode_name = fec_mode == 2 ? ", FECv2" : (fec_mode == 1 ? ", FEC" : "");
    snprintf(msg, sizeof(msg), "test %d (%d%% drop%s): sender returned %d",
             label, (int)(drop_rate * 100), mode_name, ret);
    TEST(ret == data_size, msg);

    snprintf(msg, sizeof(msg), "test %d: receiver got %d bytes", label, rarg.result);
    TEST(rarg.result == data_size, msg);

    f = fopen(out_path, "rb");
    fseek(f, 0, SEEK_END);
    long out_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *out_data = NULL;
    if (out_size > 0) {
        out_data = malloc((size_t)out_size);
        fread(out_data, 1, (size_t)out_size, f);
    }
    fclose(f);

    int match = (out_size == data_size && out_data != NULL &&
                 memcmp(data, out_data, data_size) == 0);
    snprintf(msg, sizeof(msg), "test %d: file integrity check%s",
             label, fec_mode == 2 ? " (FECv2)" : (fec_mode == 1 ? " (FEC)" : ""));
    TEST(match, msg);

    if (out_data) free(out_data);
    close(recv_fd);
    close(send_fd);

    remove(in_path);
    remove(out_path);
}

int main(void)
{
    srand(42);

    int data_size = 50000;
    uint8_t *data = malloc((size_t)data_size);
    for (int i = 0; i < data_size; i++) data[i] = (uint8_t)(i % 251);

    printf("=== File transfer test (50KB payload) ===\n");

    run_transfer(1, data, data_size, 0.0f, 0);
    run_transfer(2, data, data_size, 0.1f, 0);
    run_transfer(3, data, data_size, 0.3f, 0);
    run_transfer(4, data, data_size, 0.5f, 0);
    run_transfer(5, data, data_size, 0.1f, 1);
    run_transfer(6, data, data_size, 0.3f, 1);
    run_transfer(7, data, data_size, 0.1f, 2);
    run_transfer(8, data, data_size, 0.3f, 2);

    free(data);

    printf("\n%d / %d tests passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
