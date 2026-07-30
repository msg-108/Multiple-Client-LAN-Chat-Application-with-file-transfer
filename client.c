#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUFFER_SIZE  1024

/**
 * Creates an IPv4 stream socket (TCP).
 * 
 * @return File descriptor of the created socket, or -1 on error.
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
 * Prepares server address structure and connects to server using connect().
 * 
 * @param sock_fd Socket file descriptor.
 * @param ip Server IP address string.
 * @param port Server port number.
 * @return 0 on success, -1 on failure.
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

    // 3. Send sample messages or interactive user input to server
    if (argc > 3) {
        // Send custom message provided as command line argument
        const char *msg = argv[3];
        printf("Sending message: %s\n", msg);
        send(sock_fd, msg, strlen(msg), 0);
    } else {
        // Send default messages
        const char *msg1 = "Hello, Server! (Message 1)\n";
        const char *msg2 = "Sending second message before disconnect.\n";

        printf("Sending message 1: %s", msg1);
        send(sock_fd, msg1, strlen(msg1), 0);

        usleep(100000); // 100ms pause between messages

        printf("Sending message 2: %s", msg2);
        send(sock_fd, msg2, strlen(msg2), 0);
    }

    // 4. Gracefully close connection on exit (sends TCP FIN to trigger recv() returning 0 on server)
    printf("Closing connection gracefully...\n");
    close(sock_fd);
    printf("Connection closed.\n");

    return 0;
}
