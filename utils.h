#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>

/**
 * Reads up to chunk_size bytes from an open binary file stream using fread().
 *
 * @param fp Pointer to an open FILE (must be opened in "rb" binary mode).
 * @param buffer Output buffer to receive raw bytes.
 * @param chunk_size Maximum number of bytes to read.
 * @return Number of bytes actually read (0 on EOF or error).
 */
int read_file_chunk(FILE *fp, char *buffer, int chunk_size);

/**
 * Writes exactly `bytes` bytes from buffer to an open binary file stream using fwrite().
 *
 * @param fp Pointer to an open FILE (must be opened in "wb" or "ab" binary mode).
 * @param buffer Input buffer containing raw bytes to write.
 * @param bytes Exact number of bytes to write.
 * @return 0 on success (all bytes written), -1 on failure.
 */
int write_file_chunk(FILE *fp, char *buffer, int bytes);

#endif // UTILS_H
