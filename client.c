#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdint.h>
#include "protocol.h"

#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUFFER_SIZE  1024

// State flag to signal thread shutdown
static volatile int is_running = 1;

/**
 * Reads EXACTLY `len` bytes from socket file descriptor.
 * Standard recv() may return partial data; this loops until all requested bytes are read.
 * 
 * @param fd Socket descriptor.
 * @param buf Output buffer pointer.
 * @param len Exact byte count to receive.
 * @return Number of bytes read (len on success, 0 on EOF, -1 on error).
 */
ssize_t read_exact(int fd, void *buf, size_t len) {
    size_t total_read = 0;
    char *ptr = (char *)buf;

    while (total_read < len) {
        ssize_t bytes_read = recv(fd, ptr + total_read, len - total_read, 0);
        if (bytes_read > 0) {
            total_read += bytes_read;
        } else if (bytes_read == 0) {
            return (total_read == 0) ? 0 : -1;
        } else {
            return -1;
        }
    }
    return (ssize_t)total_read;
}

/**
 * Writes EXACTLY `len` bytes to socket file descriptor.
 * 
 * @param fd Socket descriptor.
 * @param buf Input buffer pointer.
 * @param len Exact byte count to send.
 * @return Number of bytes written (len on success, -1 on error).
 */
ssize_t write_exact(int fd, const void *buf, size_t len) {
    size_t total_written = 0;
    const char *ptr = (const char *)buf;

    while (total_written < len) {
        ssize_t bytes_written = send(fd, ptr + total_written, len - total_written, MSG_NOSIGNAL);
        if (bytes_written > 0) {
            total_written += bytes_written;
        } else {
            return -1;
        }
    }
    return (ssize_t)total_written;
}

/**
 * Sends a structured message packet: Header (8 bytes) followed by Payload (Header.length bytes).
 * 
 * Requirement: Send Header -> Payload, always.
 * 
 * @param sock_fd Socket file descriptor.
 * @param type Message enum type (CHAT, USER_JOIN, USER_LEAVE, etc.).
 * @param payload Memory buffer containing payload data.
 * @param payload_len Exact payload size in bytes.
 * @return 0 on success, -1 on failure.
 */
int send_packet(int sock_fd, int32_t type, const void *payload, int32_t payload_len) {
    Header header;
    header.type = type;
    header.length = payload_len;

    // 1. Send Header first
    if (write_exact(sock_fd, &header, sizeof(Header)) < 0) {
        return -1;
    }

    // 2. Send Payload (exactly Header.length bytes)
    if (payload_len > 0 && payload != NULL) {
        if (write_exact(sock_fd, payload, (size_t)payload_len) < 0) {
            return -1;
        }
    }

    return 0;
}

/**
 * Receiver Worker Thread Routine
 * 
 * Requirement:
 * - Continuously reads incoming messages using structured protocol.
 * - Always receives Header first (8 bytes).
 * - Trust Header.length — never assume fixed payload sizes.
 * - Handles incoming message types: CHAT, USER_JOIN, USER_LEAVE.
 */
void *receive_handler_thread(void *arg) {
    int sock_fd = (int)(intptr_t)arg;
    Header header;

    while (is_running) {
        // Step A: Read Header first
        ssize_t h_res = read_exact(sock_fd, &header, sizeof(Header));
        if (h_res <= 0) {
            if (is_running) {
                if (h_res == 0) {
                    printf("\n[-] Server disconnected.\n");
                } else {
                    printf("\n[-] Error reading packet header from server.\n");
                }
                is_running = 0;
            }
            break;
        }

        // Step B: Read Payload dynamically trusting header.length
        char *payload = NULL;
        if (header.length > 0) {
            payload = malloc((size_t)header.length + 1);
            if (!payload) {
                fprintf(stderr, "Memory allocation failed for payload length %d\n", header.length);
                break;
            }

            if (read_exact(sock_fd, payload, (size_t)header.length) <= 0) {
                free(payload);
                if (is_running) {
                    printf("\n[-] Error reading packet payload from server.\n");
                    is_running = 0;
                }
                break;
            }
            payload[header.length] = '\0'; // Safe null-termination
        }

        // Step C: Handle incoming message types (CHAT, USER_JOIN, USER_LEAVE)
        switch (header.type) {
            case CHAT:
                printf("%s", payload ? payload : "");
                if (payload && header.length > 0 && payload[header.length - 1] != '\n') {
                    printf("\n");
                }
                fflush(stdout);
                break;

            case USER_JOIN:
                printf("[+] User joined chat: %s\n", (payload && header.length > 0) ? payload : "Anonymous");
                fflush(stdout);
                break;

            case USER_LEAVE:
                printf("[-] User left chat: %s\n", (payload && header.length > 0) ? payload : "Anonymous");
                fflush(stdout);
                break;

            case FILE_START:
                printf("[FILE] Starting file transfer (%d bytes metadata)\n", header.length);
                fflush(stdout);
                break;

            case FILE_CHUNK:
                printf("[FILE] Receiving file chunk (%d bytes)\n", header.length);
                fflush(stdout);
                break;

            case FILE_END:
                printf("[FILE] File transfer completed.\n");
                fflush(stdout);
                break;

            default:
                if (payload) {
                    printf("[PACKET %d]: %s\n", header.type, payload);
                }
                fflush(stdout);
                break;
        }

        if (payload) {
            free(payload);
        }
    }

    return NULL;
}

/**
 * Creates an IPv4 stream socket (TCP).
 */
int create_client_socket(void) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation error");
        return -1;
    }
    return sock_fd;
}

/**
 * Connects to server using IP and port.
 */
int connect_to_server(int sock_fd, const char *ip, int port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid or unsupported IP address format");
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to server failed");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    // Unbuffer standard streams for responsive display
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    const char *server_ip = (argc > 1) ? argv[1] : DEFAULT_IP;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;

    printf("Attempting to connect to server at %s:%d...\n", server_ip, port);

    // 1. Create socket
    int sock_fd = create_client_socket();
    if (sock_fd < 0) {
        exit(EXIT_FAILURE);
    }

    // 2. Connect to server
    if (connect_to_server(sock_fd, server_ip, port) < 0) {
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    printf("Connected to server at %s:%d!\n", server_ip, port);

    // 3. Send USER_JOIN packet with username
    char default_username[MAX_USERNAME];
    snprintf(default_username, sizeof(default_username), "User_%d", getpid());
    const char *username = (argc > 4) ? argv[4] : default_username;

    printf("Registering username '%s' via USER_JOIN packet...\n", username);
    send_packet(sock_fd, USER_JOIN, username, (int32_t)strlen(username));

    // 4. Spawn Receiver Thread using pthreads for non-blocking packet reception
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receive_handler_thread, (void *)(intptr_t)sock_fd) != 0) {
        perror("Failed to create receiving thread");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    // 5. Main Thread Input Loop
    if (argc > 3) {
        // Command-line mode: Send single message as CHAT packet
        const char *msg = argv[3];
        printf("Sending CHAT packet: %s\n", msg);
        send_packet(sock_fd, CHAT, msg, (int32_t)strlen(msg));
        usleep(300000); // 300ms pause to allow receiving thread to process broadcasts
    } else {
        // Interactive mode: Read user input from stdin
        printf("==================================================\n");
        printf("  Structured Protocol Chat Client\n");
        printf("  Registered Username: %s\n", username);
        printf("  Type messages and press ENTER to send.\n");
        printf("  Type /quit or /exit to disconnect.\n");
        printf("==================================================\n\n");

        char input_buffer[BUFFER_SIZE];
        while (is_running && fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
            // Check for disconnect command
            if (strncmp(input_buffer, "/quit", 5) == 0 || strncmp(input_buffer, "/exit", 5) == 0) {
                // Send USER_LEAVE signal packet
                printf("Sending USER_LEAVE packet and disconnecting...\n");
                send_packet(sock_fd, USER_LEAVE, username, (int32_t)strlen(username));
                break;
            }

            // Send typed input as CHAT packet: Header -> Payload
            if (send_packet(sock_fd, CHAT, input_buffer, (int32_t)strlen(input_buffer)) < 0) {
                perror("[-] Send packet failed");
                break;
            }
        }
    }

    // Graceful Shutdown
    is_running = 0;
    close(sock_fd);
    pthread_join(recv_thread, NULL);

    printf("Connection closed gracefully.\n");
    return 0;
}
