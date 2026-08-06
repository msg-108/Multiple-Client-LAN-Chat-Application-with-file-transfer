#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int init_sockets(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[ERROR] WSAStartup failed.\n");
        return -1;
    }
#endif
    return 0;
}

void cleanup_sockets(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

void set_socket_nosigpipe(int sock) {
#ifdef SO_NOSIGPIPE
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#else
    (void)sock;
#endif
}


/**
 * Requirement: send_all
 * Loops until all `len` bytes are sent, handling partial sends.
 *
 * @param sock Socket file descriptor.
 * @param buf Buffer containing raw bytes to send.
 * @param len Total length of buffer in bytes.
 * @return Total bytes transferred (len) on success, -1 on failure.
 */
int send_all(int sock, void *buf, int len) {
    if (sock < 0 || buf == NULL || len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    int total_sent = 0;
    const char *ptr = (const char *)buf;

    while (total_sent < len) {
        ssize_t bytes_sent = send(sock, ptr + total_sent, (size_t)(len - total_sent), MSG_NOSIGNAL);
        if (bytes_sent > 0) {
            total_sent += (int)bytes_sent;
        } else {
            // send() failed or socket disconnected
            return -1;
        }
    }

    return total_sent;
}

/**
 * Requirement: recv_all
 * Loops until all `len` bytes are received, handling partial receives.
 * Returns -1 immediately if recv() returns 0 (disconnect) or -1 (error).
 *
 * @param sock Socket file descriptor.
 * @param buf Output buffer pointer.
 * @param len Total length of buffer to receive in bytes.
 * @return Total bytes transferred (len) on success, -1 on failure/disconnect.
 */
int recv_all(int sock, void *buf, int len) {
    if (sock < 0 || buf == NULL || len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    int total_received = 0;
    char *ptr = (char *)buf;

    while (total_received < len) {
        ssize_t bytes_read = recv(sock, ptr + total_received, (size_t)(len - total_received), 0);
        if (bytes_read > 0) {
            total_received += (int)bytes_read;
        } else {
            // Return -1 immediately on 0 (disconnect) or -1 (error)
            return -1;
        }
    }

    return total_received;
}

/**
 * read_file_chunk
 * Reads up to chunk_size bytes using fread().
 */
int read_file_chunk(FILE *fp, char *buffer, int chunk_size) {
    if (fp == NULL || buffer == NULL || chunk_size <= 0) {
        return 0;
    }
    size_t bytes_read = fread(buffer, 1, (size_t)chunk_size, fp);
    return (int)bytes_read;
}

/**
 * write_file_chunk
 * Writes exactly `bytes` bytes using fwrite().
 */
int write_file_chunk(FILE *fp, char *buffer, int bytes) {
    if (fp == NULL || buffer == NULL || bytes < 0) {
        return -1;
    }
    if (bytes == 0) {
        return 0;
    }
    size_t bytes_written = fwrite(buffer, 1, (size_t)bytes, fp);
    if (bytes_written != (size_t)bytes) {
        perror("write_file_chunk failed");
        return -1;
    }
    return 0;
}

// Test main for utils functionality
#ifdef TEST_UTILS
#define CHUNK_SIZE 16

int main(void) {
    const char *src_path = "test_input.bin";
    const char *dst_path = "test_output.bin";

    FILE *sample_fp = fopen(src_path, "wb");
    if (!sample_fp) {
        perror("Failed to create sample file");
        return EXIT_FAILURE;
    }

    unsigned char test_bytes[] = {
        0x7F, 0x45, 0x4C, 0x46, 0x00, 0xFF, 0xFE, 0xFD,
        'H',  'e',  'l',  'l',  'o',  0x00, 'W',  'o',
        'r',  'l',  'd',  0x10, 0x20, 0x30, 0x40, 0x50
    };
    size_t total_test_bytes = sizeof(test_bytes);
    fwrite(test_bytes, 1, total_test_bytes, sample_fp);
    fclose(sample_fp);

    printf("[+] Created binary test file '%s' (%zu bytes)\n", src_path, total_test_bytes);

    FILE *src_fp = fopen(src_path, "rb");
    if (src_fp == NULL) {
        perror("Error opening source file");
        return EXIT_FAILURE;
    }

    FILE *dst_fp = fopen(dst_path, "wb");
    if (dst_fp == NULL) {
        perror("Error opening destination file");
        fclose(src_fp);
        return EXIT_FAILURE;
    }

    char buffer[CHUNK_SIZE];
    int bytes_read = 0;
    int total_copied = 0;

    while ((bytes_read = read_file_chunk(src_fp, buffer, CHUNK_SIZE)) > 0) {
        if (write_file_chunk(dst_fp, buffer, bytes_read) != 0) {
            fclose(src_fp);
            fclose(dst_fp);
            return EXIT_FAILURE;
        }
        total_copied += bytes_read;
    }

    fclose(src_fp);
    fclose(dst_fp);

    printf("[+] Round-trip copy complete: %d bytes written\n", total_copied);

    remove(src_path);
    remove(dst_path);

    return EXIT_SUCCESS;
}
#endif
