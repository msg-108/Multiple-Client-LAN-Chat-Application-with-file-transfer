#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define PORT        8080
#define BACKLOG     10
#define BUFFER_SIZE 1024

// Structure used to safely pass client context (socket + address) to worker thread
typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
} client_info_t;

/**
 * Step 1: Create a TCP Socket
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
 */
int start_listening(int server_fd, int backlog) {
    if (listen(server_fd, backlog) < 0) {
        perror("Listen failed");
        return -1;
    }
    return 0;
}

/**
 * Worker Thread Handler for Each Connected Client
 * 
 * Thread Lifecycle Explanation:
 * 1. Detach Thread: Calls pthread_detach(pthread_self()) so POSIX/kernel automatically
 *    reclaims thread stack and resources when execution finishes (prevents memory leaks).
 * 2. Unpack Arguments: Extracts client_fd and client_addr from heap pointer and frees heap memory.
 * 3. Receive Loop: Continuously calls recv() to handle messages from this client until disconnect.
 * 4. Cleanup & Termination: Closes client_fd upon client disconnect (recv returns 0) or error (recv returns -1), then exits.
 */
void *client_handler_thread(void *arg) {
    // 1. Detach thread for automatic resource reclamation upon completion
    pthread_detach(pthread_self());

    // 2. Unpack heap-allocated client info struct and free memory
    client_info_t *info = (client_info_t *)arg;
    if (!info) {
        return NULL;
    }

    int client_fd = info->client_fd;
    struct sockaddr_in client_addr = info->client_addr;
    free(info); // Free memory allocated in main thread

    // Extract client IP address string and port number
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, sizeof(client_ip));
    int client_port = ntohs(client_addr.sin_port);

    printf("[Worker Thread %lu] [+] Client connected: %s:%d (Socket FD: %d)\n",
           (unsigned long)pthread_self(), client_ip, client_port, client_fd);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    // 3. Communication Loop: Read messages sent by client using recv()
    while ((bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0'; // Null-terminate received string

        printf("[%s:%d | Thread %lu]: %s",
               client_ip, client_port, (unsigned long)pthread_self(), buffer);
        if (buffer[bytes_received - 1] != '\n') {
            printf("\n");
        }
    }

    // 4. Handle client disconnection or socket error
    if (bytes_received == 0) {
        printf("[-] Client %s:%d disconnected (recv returned 0).\n", client_ip, client_port);
    } else {
        perror("[-] recv error");
        printf("[-] Connection lost with client %s:%d.\n", client_ip, client_port);
    }

    // Close client socket descriptor cleanly
    close(client_fd);
    printf("[Worker Thread %lu] Closed socket FD %d & exiting thread for %s:%d.\n\n",
           (unsigned long)pthread_self(), client_fd, client_ip, client_port);

    return NULL;
}

/**
 * Step 4: Multithreaded Accept Loop
 * 
 * Main thread accepts incoming connections and immediately spawns a dedicated worker thread
 * via pthread_create() for each client. The main thread continues accepting new clients.
 */
void accept_clients_loop(int server_fd) {
    printf("=====================================================\n");
    printf("  Multithreaded TCP Server listening on port %d...\n", PORT);
    printf("  Waiting for incoming client connections...\n");
    printf("=====================================================\n\n");

    while (1) {
        // Allocate heap memory for client info to prevent race conditions on stack variables
        client_info_t *info = malloc(sizeof(client_info_t));
        if (!info) {
            perror("Failed to allocate memory for client info");
            continue;
        }

        socklen_t client_len = sizeof(struct sockaddr_in);

        // Accept client connection (blocks until client connects)
        info->client_fd = accept(server_fd, (struct sockaddr *)&info->client_addr, &client_len);
        if (info->client_fd < 0) {
            perror("Accept failed");
            free(info);
            continue;
        }

        printf("[Main Thread] Accepted client socket FD %d. Spawning worker thread...\n", info->client_fd);

        // Spawn a dedicated POSIX thread using pthread_create()
        pthread_t tid;
        int rc = pthread_create(&tid, NULL, client_handler_thread, info);
        if (rc != 0) {
            fprintf(stderr, "Failed to create thread: %s\n", strerror(rc));
            close(info->client_fd);
            free(info);
            continue;
        }
    }
}

int main(void) {
    // Disable stdout and stderr buffering so log messages print instantly
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

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
    printf("[Step 3] Multithreaded server listening with backlog queue of %d\n\n", BACKLOG);

    // 4. Accept clients concurrently using POSIX pthreads
    accept_clients_loop(server_fd);

    close(server_fd);
    return 0;
}
