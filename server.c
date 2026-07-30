#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT        8080
#define BACKLOG     10
#define BUFFER_SIZE 1024

/**
 * Step 1: Create a TCP Socket
 * 
 * Creates an IPv4 stream socket (TCP) and sets SO_REUSEADDR option.
 * 
 * @return File descriptor of created socket, or -1 on error.
 */
int create_server_socket(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

/**
 * Step 2: Bind Socket to Port 8080
 * 
 * Binds the server socket to INADDR_ANY and port 8080.
 * 
 * @param server_fd Socket file descriptor.
 * @param port Port number to bind.
 * @return 0 on success, -1 on failure.
 */
int bind_server_socket(int server_fd, int port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return -1;
    }

    return 0;
}

/**
 * Step 3: Listen for Incoming Connections
 * 
 * Marks the socket into passive mode to listen for client connections.
 * 
 * @param server_fd Socket file descriptor.
 * @param backlog Length of connection queue.
 * @return 0 on success, -1 on failure.
 */
int start_listening(int server_fd, int backlog) {
    if (listen(server_fd, backlog) < 0) {
        perror("Listen failed");
        return -1;
    }
    return 0;
}

/**
 * Handles communication with an accepted client socket using recv().
 * 
 * - Receives messages using recv() into a 1024-byte buffer
 * - Prints received messages to the server terminal
 * - Detects client disconnect when recv() returns 0
 * - Closes the client socket when client disconnects or an error occurs
 * 
 * @param client_fd Client socket descriptor.
 * @param client_ip String representation of client IP address.
 * @param client_port Client port number.
 */
void handle_client(int client_fd, const char *client_ip, int client_port) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    printf("[*] Connected to client %s:%d. Waiting for messages...\n", client_ip, client_port);
    fflush(stdout);

    // Loop: Continuously call recv() to read messages sent by the connected client
    while ((bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        // Null-terminate the string buffer for safe printing
        buffer[bytes_received] = '\0';

        // Print received message with client details
        printf("[%s:%d]: %s", client_ip, client_port, buffer);

        // Ensure output ends with a newline character if message didn't contain one
        if (bytes_received > 0 && buffer[bytes_received - 1] != '\n') {
            printf("\n");
        }
        fflush(stdout);
    }

    // Check return value of recv()
    if (bytes_received == 0) {
        // recv() returning 0 signals graceful client disconnect (peer sent TCP FIN)
        printf("[-] Client %s:%d disconnected (recv returned 0).\n\n", client_ip, client_port);
    } else {
        // recv() returning -1 signals a socket read error or abrupt disconnect
        perror("[-] recv error");
        printf("[-] Connection lost with client %s:%d.\n\n", client_ip, client_port);
    }
    fflush(stdout);

    // Gracefully close client socket descriptor
    close(client_fd);
}

/**
 * Step 4: Accept Loop
 * 
 * Sequentially accepts client connections in an infinite loop.
 * Passes each client socket descriptor to handle_client().
 * When a client disconnects, the loop resumes to accept the next client.
 * 
 * @param server_fd Master server socket descriptor.
 */
void accept_clients_loop(int server_fd) {
    printf("=========================================\n");
    printf("  TCP Server listening on port %d...\n", PORT);
    printf("  Waiting for incoming client connections...\n");
    printf("=========================================\n\n");
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // accept() blocks execution until a new client connects
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("Accept failed");
            continue; // Continue listening for next client
        }

        // Convert client binary IP address and port to human-readable format
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, sizeof(client_ip));
        int client_port = ntohs(client_addr.sin_port);

        printf("[+] New client connected: IP = %s, Port = %d (Socket FD = %d)\n",
               client_ip, client_port, client_fd);
        fflush(stdout);

        // Process client messages until disconnect
        handle_client(client_fd, client_ip, client_port);

        printf("[*] Ready for next client connection...\n\n");
        fflush(stdout);
    }
}

int main(void) {
    // 1. Create socket
    int server_fd = create_server_socket();
    if (server_fd < 0) {
        exit(EXIT_FAILURE);
    }
    printf("[Step 1] Socket created successfully (FD: %d)\n", server_fd);

    // 2. Bind to port 8080
    if (bind_server_socket(server_fd, PORT) < 0) {
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("[Step 2] Bound successfully to port %d\n", PORT);

    // 3. Listen for incoming connections
    if (start_listening(server_fd, BACKLOG) < 0) {
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("[Step 3] Server listening with backlog queue of %d\n\n", BACKLOG);
    fflush(stdout);

    // 4. Accept clients sequentially and receive messages
    accept_clients_loop(server_fd);

    // Cleanup (unreachable infinite loop)
    close(server_fd);
    return 0;
}
