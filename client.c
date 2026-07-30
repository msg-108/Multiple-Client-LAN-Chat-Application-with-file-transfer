#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdint.h>

#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUFFER_SIZE  1024

// State flag to signal thread shutdown
static volatile int is_running = 1;

/**
 * Receiver Worker Thread Routine
 * 
 * Requirement: Add a second thread using pthreads.
 * - Thread continuously calls recv() from server.
 * - Prints received messages to terminal.
 * - Receiving thread handles output (stdout).
 * 
 * @param arg Socket file descriptor cast to void*.
 */
void *receive_handler_thread(void *arg) {
    int sock_fd = (int)(intptr_t)arg;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    while (is_running) {
        // Continuously read data sent by server
        bytes_received = recv(sock_fd, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0'; // Null-terminate string
            // Print server output to terminal
            printf("%s", buffer);
            if (buffer[bytes_received - 1] != '\n') {
                printf("\n");
            }
            fflush(stdout);
        } else if (bytes_received == 0) {
            // Server disconnected or rejected connection
            if (is_running) {
                printf("\n[-] Server disconnected.\n");
                is_running = 0;
            }
            break;
        } else {
            // Socket read error or closed by main thread
            if (is_running) {
                perror("[-] recv error");
                is_running = 0;
            }
            break;
        }
    }

    return NULL;
}

/**
 * Creates an IPv4 stream socket (TCP).
 * 
 * @return File descriptor of created socket, or -1 on error.
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
 * Connects to server using IP and port via connect().
 * 
 * @param sock_fd Socket file descriptor.
 * @param ip Server IP address.
 * @param port Server port number.
 * @return 0 on success, -1 on error.
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

/**
 * Main Thread Handler
 * 
 * Constraint: Main thread handles input (stdin).
 */
int main(int argc, char *argv[]) {
    // Unbuffer standard streams for responsive terminal display
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

    // 3. Create Receiver Thread using pthreads for server output
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receive_handler_thread, (void *)(intptr_t)sock_fd) != 0) {
        perror("Failed to create receiving thread");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    // 4. Main Thread Input Loop
    if (argc > 3) {
        // Non-interactive mode: Send message specified in command line argument
        const char *msg = argv[3];
        printf("Sending message: %s\n", msg);
        send(sock_fd, msg, strlen(msg), 0);
        usleep(300000); // 300ms pause to allow receiver thread to process replies
    } else {
        // Interactive mode: Read user input from stdin
        printf("==================================================\n");
        printf("  Interactive Client Terminal Connected\n");
        printf("  Type messages and press ENTER to send.\n");
        printf("  Type /quit or /exit to disconnect.\n");
        printf("==================================================\n\n");

        char input_buffer[BUFFER_SIZE];
        while (is_running && fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
            // Check for exit command
            if (strncmp(input_buffer, "/quit", 5) == 0 || strncmp(input_buffer, "/exit", 5) == 0) {
                printf("Disconnecting from server...\n");
                break;
            }

            // Send message to server
            if (send(sock_fd, input_buffer, strlen(input_buffer), 0) < 0) {
                perror("[-] Send failed");
                break;
            }
        }
    }

    // Graceful Shutdown
    is_running = 0;
    close(sock_fd); // Close socket descriptor to unblock recv() in receiving thread
    pthread_join(recv_thread, NULL); // Wait for receiving thread to complete

    printf("Connection closed gracefully.\n");
    return 0;
}
