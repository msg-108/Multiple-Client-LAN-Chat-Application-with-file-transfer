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
 * Sends a structured message (Header + Payload) to server.
 * Requirement: Send Header -> Payload, always.
 */
int send_packet(int sock_fd, int32_t type, const void *payload, int32_t payload_len) {
    Header header;
    header.type = type;
    header.length = payload_len;

    if (write_exact(sock_fd, &header, sizeof(Header)) < 0) {
        return -1;
    }

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
 * Handles incoming broadcast packets (CHAT, USER_JOIN, USER_LEAVE).
 */
void *receive_handler_thread(void *arg) {
    int sock_fd = (int)(intptr_t)arg;
    Header header;

    while (is_running) {
        // Read Header
        ssize_t h_res = read_exact(sock_fd, &header, sizeof(Header));
        if (h_res <= 0) {
            if (is_running) {
                if (h_res == 0) {
                    printf("\n[-] Server disconnected.\n");
                } else {
                    printf("\n[-] Error reading header from server.\n");
                }
                is_running = 0;
            }
            break;
        }

        // Read Payload trusting header.length
        char *payload = NULL;
        if (header.length > 0) {
            payload = malloc((size_t)header.length + 1);
            if (!payload) break;

            if (read_exact(sock_fd, payload, (size_t)header.length) <= 0) {
                free(payload);
                if (is_running) {
                    printf("\n[-] Error reading payload from server.\n");
                    is_running = 0;
                }
                break;
            }
            payload[header.length] = '\0';
        }

        // Handle message type
        switch (header.type) {
            case CHAT:
                printf("%s", payload ? payload : "");
                if (payload && header.length > 0 && payload[header.length - 1] != '\n') {
                    printf("\n");
                }
                fflush(stdout);
                break;

            case USER_JOIN:
                printf("[+] %s joined the chat\n", (payload && header.length > 0) ? payload : "Guest");
                fflush(stdout);
                break;

            case USER_LEAVE:
                printf("[-] %s left the chat\n", (payload && header.length > 0) ? payload : "Guest");
                fflush(stdout);
                break;

            default:
                if (payload) printf("%s\n", payload);
                fflush(stdout);
                break;
        }

        if (payload) free(payload);
    }

    return NULL;
}

int create_client_socket(void) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation error");
        return -1;
    }
    return sock_fd;
}

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
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    const char *server_ip = (argc > 1) ? argv[1] : DEFAULT_IP;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    const char *cli_message = (argc > 3) ? argv[3] : NULL;
    const char *cli_username = (argc > 4) ? argv[4] : NULL;

    printf("Connecting to server at %s:%d...\n", server_ip, port);

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

    printf("Connected to server at %s:%d!\n\n", server_ip, port);

    // Requirement 1 & 3: Interactive / Retry Username Registration Loop
    char username[MAX_USERNAME] = {0};
    int registered = 0;

    while (!registered) {
        if (cli_username != NULL) {
            // Constraint: Limit username to 32 (MAX_USERNAME) chars client-side
            strncpy(username, cli_username, MAX_USERNAME - 1);
            username[MAX_USERNAME - 1] = '\0';
        } else {
            // Requirement 1: Ask user to enter a username at startup
            printf("Enter your username (max %d chars): ", MAX_USERNAME - 1);
            if (fgets(username, sizeof(username), stdin) == NULL) {
                printf("\nNo username entered. Exiting.\n");
                close(sock_fd);
                exit(EXIT_SUCCESS);
            }

            // Strip trailing newlines
            size_t len = strlen(username);
            while (len > 0 && (username[len - 1] == '\n' || username[len - 1] == '\r')) {
                username[--len] = '\0';
            }

            if (len == 0) {
                printf("Username cannot be empty. Please try again.\n");
                continue;
            }
        }

        // Requirement 2: Send username to server immediately using Header + payload format
        if (send_packet(sock_fd, USER_JOIN, username, (int32_t)strlen(username)) < 0) {
            perror("[-] Error sending username packet");
            close(sock_fd);
            exit(EXIT_FAILURE);
        }

        // Check for immediate server rejection packet (e.g. duplicate username or server full)
        // Set short socket timeout to check for immediate response
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        Header resp_header;
        ssize_t h_res = read_exact(sock_fd, &resp_header, sizeof(Header));

        // Reset socket timeout back to non-blocking/blocking default
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (h_res == (ssize_t)sizeof(Header)) {
            // Server sent a response packet!
            char *resp_payload = NULL;
            if (resp_header.length > 0) {
                resp_payload = malloc((size_t)resp_header.length + 1);
                if (resp_payload) {
                    read_exact(sock_fd, resp_payload, (size_t)resp_header.length);
                    resp_payload[resp_header.length] = '\0';
                }
            }

            if (resp_payload && (strstr(resp_payload, "ERROR:") || strstr(resp_payload, "already taken") || strstr(resp_payload, "REJECTION"))) {
                // Requirement 3: Print clear rejection message and retry with a different name
                printf("\n[REJECTED] %s\n", resp_payload);
                free(resp_payload);

                if (cli_username != NULL) {
                    // Reset cli_username to force interactive prompt for retry
                    cli_username = NULL;
                }
                continue; // Retry loop!
            }

            if (resp_payload) free(resp_payload);
        }

        // Registration accepted!
        registered = 1;
    }

    printf("[+] Registered successfully as '%s'!\n\n", username);

    // 4. Spawn Receiver Thread using pthreads for ongoing chat messages
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receive_handler_thread, (void *)(intptr_t)sock_fd) != 0) {
        perror("Failed to create receiving thread");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    // 5. Main Thread Input Loop
    if (cli_message != NULL) {
        printf("Sending CHAT packet: %s\n", cli_message);
        send_packet(sock_fd, CHAT, cli_message, (int32_t)strlen(cli_message));
        usleep(300000); // 300ms pause to process broadcast replies
    } else {
        printf("==================================================\n");
        printf("  Structured Protocol Chat Client\n");
        printf("  Logged in as: %s\n", username);
        printf("  Type messages and press ENTER to send.\n");
        printf("  Type /quit or /exit to disconnect.\n");
        printf("==================================================\n\n");

        char input_buffer[BUFFER_SIZE];
        while (is_running && fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
            if (strncmp(input_buffer, "/quit", 5) == 0 || strncmp(input_buffer, "/exit", 5) == 0) {
                printf("Sending USER_LEAVE packet and disconnecting...\n");
                send_packet(sock_fd, USER_LEAVE, username, (int32_t)strlen(username));
                break;
            }

            if (send_packet(sock_fd, CHAT, input_buffer, (int32_t)strlen(input_buffer)) < 0) {
                perror("[-] Send packet failed");
                break;
            }
        }
    }

    is_running = 0;
    close(sock_fd);
    pthread_join(recv_thread, NULL);

    printf("Connection closed gracefully.\n");
    return 0;
}
