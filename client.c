#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT 8080

/**
 * Step 1: Create TCP Socket
 * 
 * Creates an IPv4 stream socket (TCP).
 * 
 * @return File descriptor of the created socket, or -1 on error.
 */
int create_client_socket(void) {
    // AF_INET: IPv4 address family
    // SOCK_STREAM: Connection-oriented TCP byte stream
    // 0: Automatic protocol selection (IPPROTO_TCP)
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation error");
        return -1;
    }
    return sock_fd;
}

/**
 * Step 2 & 3: Prepare Server Address Structure & Connect to Server
 * 
 * Configures the target server IP and Port in struct sockaddr_in,
 * converts the string IP to binary, and initiates TCP connection via connect().
 * 
 * @param sock_fd Socket file descriptor.
 * @param ip Server IP address string (e.g. "127.0.0.1").
 * @param port Server port number (e.g. 8080).
 * @return 0 on successful connection, -1 on failure.
 */
int connect_to_server(int sock_fd, const char *ip, int port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    // Configure address family and port
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port); // Convert port to Network Byte Order (Big-Endian)

    // Step 2: Convert IP address string (e.g. "127.0.0.1") to binary network address
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid or unsupported IP address format");
        return -1;
    }

    // Step 3: Connect to server using connect()
    // Connect initiates the TCP 3-way handshake with the target server
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to server failed");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    // Determine target IP and Port (allow optional CLI overrides: ./client [IP] [PORT])
    const char *server_ip = (argc > 1) ? argv[1] : DEFAULT_IP;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;

    printf("Attempting to connect to server at %s:%d...\n", server_ip, port);

    // 1. Create socket using socket()
    int sock_fd = create_client_socket();
    if (sock_fd < 0) {
        exit(EXIT_FAILURE);
    }
    printf("[Step 1] Client socket created successfully (FD: %d)\n", sock_fd);

    // 2 & 3. Connect to server using IP and port
    if (connect_to_server(sock_fd, server_ip, port) < 0) {
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    // Requirement: Print "Connected to server"
    printf("[Step 2 & 3] Connected to server at %s:%d!\n", server_ip, port);

    // Step 4: Close connection gracefully on exit
    // close() sends a TCP FIN packet to terminate the session cleanly
    printf("[Step 4] Closing connection gracefully...\n");
    close(sock_fd);
    printf("Connection closed.\n");

    return 0;
}
