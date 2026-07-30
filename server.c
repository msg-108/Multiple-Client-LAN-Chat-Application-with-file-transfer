#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080
#define BACKLOG 10

/**
 * Step 1: Create a TCP Socket
 * 
 * Creates a IPv4 stream socket (TCP) and configures SO_REUSEADDR option
 * so the port can be reused immediately after stopping the server.
 * 
 * @return File descriptor of the created socket, or -1 on error.
 */
int create_server_socket(void) {
    // AF_INET: IPv4 internet protocol family
    // SOCK_STREAM: Connection-based byte stream protocol (TCP)
    // 0: System automatically chooses default protocol (IPPROTO_TCP)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    // Enable SO_REUSEADDR option to avoid "Address already in use" errors on restart
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
 * Binds the server socket to INADDR_ANY (all available network interfaces)
 * and the specified port number.
 * 
 * @param server_fd Socket file descriptor.
 * @param port Port number to bind.
 * @return 0 on success, -1 on failure.
 */
int bind_server_socket(int server_fd, int port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    // Configure address structure
    server_addr.sin_family = AF_INET;                // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;         // Accept connections on any IP interface
    server_addr.sin_port = htons(port);              // Convert port to Network Byte Order (Big-Endian)

    // Bind socket to IP address and Port
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return -1;
    }

    return 0;
}

/**
 * Step 3: Listen for Incoming Connections
 * 
 * Sets the socket into passive mode to listen for incoming client connection requests.
 * 
 * @param server_fd Socket file descriptor.
 * @param backlog Maximum length of the queue of pending connections.
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
 * Step 4 & 5: Accept Clients in a Loop & Print IP + Port
 * 
 * Infinite loop accepting incoming client connections using accept(),
 * extracting the client's IP address and port number, and printing them.
 * 
 * @param server_fd Socket file descriptor.
 */
void accept_clients_loop(int server_fd) {
    printf("Server listening on port %d...\n", PORT);
    printf("Waiting for incoming client connections...\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // Step 4: Accept connection
        // accept() blocks until a client connects, returning a new socket for client communication
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("Accept failed");
            continue; // Continue loop to accept subsequent connections
        }

        // Step 5: Extract client IP and Port
        char client_ip[INET_ADDRSTRLEN];
        // Convert client binary IP address to presentation string format
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, sizeof(client_ip));
        // Convert client port from Network Byte Order to Host Byte Order
        int client_port = ntohs(client_addr.sin_port);

        // Print client connection information
        printf("[+] New client connected: IP = %s, Port = %d (Socket FD = %d)\n",
               client_ip, client_port, client_fd);

        // Close client socket (since threading/concurrency is not implemented yet)
        close(client_fd);
        printf("[-] Closed connection for %s:%d\n\n", client_ip, client_port);
    }
}

int main(void) {
    // 1. Create socket using socket()
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
    printf("[Step 3] Listening for connections (Backlog: %d)\n", BACKLOG);

    // 4 & 5. Accept clients in a loop and print client IP + port
    accept_clients_loop(server_fd);

    // Cleanup (unreachable due to infinite loop, included for completeness)
    close(server_fd);
    return 0;
}
