#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    #define close_socket(s) closesocket(s)
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <signal.h>
    #define close_socket(s) close(s)
#endif

// MSG_NOSIGNAL is a Linux socket flag not defined on macOS (Darwin) or Windows
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/**
 * Initializes socket subsystem (WSAStartup on Windows, no-op on POSIX).
 */
int init_sockets(void);

/**
 * Cleans up socket subsystem (WSACleanup on Windows, no-op on POSIX).
 */
void cleanup_sockets(void);

/**
 * Sets socket options for signal safety (SO_NOSIGPIPE on macOS/Darwin).
 */
void set_socket_nosigpipe(int sock);

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

/**
 * Sends all `len` bytes over socket, handling partial sends.
 *
 * @param sock Socket file descriptor.
 * @param buf Buffer containing bytes to send.
 * @param len Exact number of bytes to send.
 * @return Total bytes sent (len) on success, -1 on failure.
 */
int send_all(int sock, void *buf, int len);

/**
 * Receives all `len` bytes from socket, handling partial receives.
 * Returns -1 immediately if recv() returns 0 or -1 (disconnect / error).
 *
 * @param sock Socket file descriptor.
 * @param buf Output buffer pointer.
 * @param len Exact number of bytes to receive.
 * @return Total bytes received (len) on success, -1 on failure/disconnect.
 */
int recv_all(int sock, void *buf, int len);

#endif // UTILS_H

