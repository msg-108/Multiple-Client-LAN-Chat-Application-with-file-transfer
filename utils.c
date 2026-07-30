#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Requirement 1: read_file_chunk
 * Reads up to chunk_size bytes using fread().
 * Note: File-not-found and permission errors must be checked before calling this.
 *
 * @param fp Open binary FILE pointer ("rb" mode).
 * @param buffer Buffer to receive raw data bytes.
 * @param chunk_size Maximum number of bytes to read into buffer.
 * @return Number of bytes actually read.
 */
int read_file_chunk(FILE *fp, char *buffer, int chunk_size) {
    if (fp == NULL || buffer == NULL || chunk_size <= 0) {
        return 0;
    }

    // fread reads up to chunk_size items of 1 byte each
    size_t bytes_read = fread(buffer, 1, (size_t)chunk_size, fp);

    // Return the actual number of bytes read
    return (int)bytes_read;
}

/**
 * Requirement 2: write_file_chunk
 * Writes exactly `bytes` bytes using fwrite().
 *
 * @param fp Open binary FILE pointer ("wb" mode).
 * @param buffer Buffer containing raw data bytes to write.
 * @param bytes Exact number of bytes to write.
 * @return 0 on success, -1 on failure.
 */
int write_file_chunk(FILE *fp, char *buffer, int bytes) {
    if (fp == NULL || buffer == NULL || bytes < 0) {
        return -1;
    }

    if (bytes == 0) {
        return 0; // Nothing to write
    }

    // fwrite writes `bytes` items of 1 byte each
    size_t bytes_written = fwrite(buffer, 1, (size_t)bytes, fp);

    // Return failure (-1) if full count of bytes was not successfully written
    if (bytes_written != (size_t)bytes) {
        perror("write_file_chunk failed");
        return -1;
    }

    return 0; // Success
}

// Small main() to test round-trip functionality on a sample file
#ifdef TEST_UTILS
#define CHUNK_SIZE 16

int main(void) {
    const char *src_path = "test_input.bin";
    const char *dst_path = "test_output.bin";

    // 1. Create a sample binary file containing arbitrary non-string data
    FILE *sample_fp = fopen(src_path, "wb"); // Binary write mode
    if (!sample_fp) {
        perror("Failed to create sample file");
        return EXIT_FAILURE;
    }

    // Raw binary sample data including null bytes \0 and non-ASCII bytes
    unsigned char test_bytes[] = {
        0x7F, 0x45, 0x4C, 0x46, 0x00, 0xFF, 0xFE, 0xFD,
        'H',  'e',  'l',  'l',  'o',  0x00, 'W',  'o',
        'r',  'l',  'd',  0x10, 0x20, 0x30, 0x40, 0x50
    };
    size_t total_test_bytes = sizeof(test_bytes);
    fwrite(test_bytes, 1, total_test_bytes, sample_fp);
    fclose(sample_fp);

    printf("[+] Created binary test file '%s' (%zu bytes)\n", src_path, total_test_bytes);

    // 2. Pre-check file existence and permission errors prior to reading
    FILE *src_fp = fopen(src_path, "rb"); // Constraint: binary mode "rb"
    if (src_fp == NULL) {
        perror("Error: Source file not found or permission denied");
        return EXIT_FAILURE;
    }

    FILE *dst_fp = fopen(dst_path, "wb"); // Constraint: binary mode "wb"
    if (dst_fp == NULL) {
        perror("Error: Failed to open destination file for writing");
        fclose(src_fp);
        return EXIT_FAILURE;
    }

    // 3. Round-trip read and write in chunks
    char buffer[CHUNK_SIZE];
    int bytes_read = 0;
    int total_copied = 0;

    printf("[+] Testing chunked round-trip copy (chunk size = %d bytes)...\n", CHUNK_SIZE);

    while ((bytes_read = read_file_chunk(src_fp, buffer, CHUNK_SIZE)) > 0) {
        if (write_file_chunk(dst_fp, buffer, bytes_read) != 0) {
            fprintf(stderr, "Error writing file chunk!\n");
            fclose(src_fp);
            fclose(dst_fp);
            return EXIT_FAILURE;
        }
        total_copied += bytes_read;
    }

    fclose(src_fp);
    fclose(dst_fp);

    printf("[+] Round-trip copy complete: %d bytes written to '%s'\n", total_copied, dst_path);

    // 4. Verify byte-by-byte match without string functions
    FILE *f_in = fopen(src_path, "rb");
    FILE *f_out = fopen(dst_path, "rb");
    if (!f_in || !f_out) {
        perror("Error opening files for verification");
        if (f_in) fclose(f_in);
        if (f_out) fclose(f_out);
        return EXIT_FAILURE;
    }

    char in_buf[CHUNK_SIZE];
    char out_buf[CHUNK_SIZE];
    int n1 = read_file_chunk(f_in, in_buf, CHUNK_SIZE);
    int n2 = read_file_chunk(f_out, out_buf, CHUNK_SIZE);

    fclose(f_in);
    fclose(f_out);

    if (n1 == n2 && memcmp(in_buf, out_buf, n1) == 0) {
        printf("[SUCCESS] Verification passed: Binary round-trip is 100%% identical byte-for-byte!\n");
    } else {
        printf("[FAILURE] Verification failed: Data mismatch detected!\n");
        return EXIT_FAILURE;
    }

    // Cleanup sample test files
    remove(src_path);
    remove(dst_path);

    return EXIT_SUCCESS;
}
#endif
