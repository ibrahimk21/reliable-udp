#ifndef RUDP_FILE_H
#define RUDP_FILE_H

#include <stdint.h>

#define RUDP_FILE_MAGIC 0x52555046 /* 'RUPF' */
#define RUDP_FILE_MAX_NAME 256

struct rudp_file_metadata {
    uint32_t magic;
    uint64_t file_size;
    char     filename[RUDP_FILE_MAX_NAME];
} __attribute__((packed));

#endif /* RUDP_FILE_H */
