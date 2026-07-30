#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "protocol.h"

#define PORT        8080
#define BACKLOG     10
#define BUFFER_SIZE 1024

// Structure representing a connected client entry in the global list
typedef struct {
    int socket_fd;
    char username[MAX_USERNAME];
    struct sockaddr_in client_addr;
} client_entry_t;

// Global connected clients list, current count, and protecting mutex
static client_entry_t *clients[MAX_CLIENTS] = { NULL };
static int client_count = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Adds a client to the global client list under mutex protection.
 * 
 * @param client_fd Socket file descriptor of connected client.
 * @param addr Address structure of client.
 * @param username Initial placeholder username string.
 * @return Pointer to allocated client_entry_t on success, NULL if server is full.
 */
client_entry_t *add_client(int client_fd, struct sockaddr_in addr, const char *username) {
    pthread_mutex_lock(&clients_mutex);

    // Constraint Check: Do not exceed MAX_CLIENTS
    if (client_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&clients_mutex);
        return NULL; // Server full
    }

    client_entry_t *entry = malloc(sizeof(client_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    entry->socket_fd = client_fd;
    entry->client_addr = addr;
    strncpy(entry->username, username, MAX_USERNAME - 1);
    entry->username[MAX_USERNAME - 1] = '\0';

    // Find available slot in global array
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == NULL) {
            clients[i] = entry;
            client_count++;
            break;
        }
    }

    pthread_mutex_unlock(&clients_mutex);
    return entry;
}

/**
 * Removes a client from the global client list by socket file descriptor.
 * Protected by clients_mutex.
 * 
 * @param client_fd Socket descriptor of client to remove.
 */
void remove_client(int client_fd) {
    pthread_mutex_lock(&clients_mutex);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != NULL && clients[i]->socket_fd == client_fd) {
            free(clients[i]);
            clients[i] = NULL;
            client_count--;
            break;
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}

/**
 * Returns current count of connected clients (thread-safe).
 */
int get_client_count(void) {
    pthread_mutex_lock(&clients_mutex);
    int count = client_count;
    pthread_mutex_unlock(&clients_mutex);
    return count;
}

/**
 * Broadcasts a message from sender to all other connected clients.
 * 
 * Constraint Enforcement:
 * 1. Lock clients_mutex ONLY to copy target socket descriptors into local array.
 * 2. Release clients_mutex BEFORE invoking send() on any socket.
 * 3. Safely handle failed send() by removing disconnected target clients without crashing.
 * 
 * @param sender_fd Socket descriptor of client sending the message.
 * @param sender_username Username string of sender.
 * @param message Message content buffer.
 */
void broadcast_message(int sender_fd, const char *sender_username, const char *message) {
    int target_sockets[MAX_CLIENTS];
    int target_count = 0;

    // 1. Lock mutex ONLY long enough to snapshot target client socket descriptors
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != NULL && clients[i]->socket_fd != sender_fd) {
            target_sockets[target_count++] = clients[i]->socket_fd;
        }
    }
    // RELEASE mutex before sending data!
    pthread_mutex_unlock(&clients_mutex);

    if (target_count == 0) {
        return; // No other clients connected
    }

    // Format broadcast payload string
    char formatted_msg[BUFFER_SIZE + MAX_USERNAME + 16];
    snprintf(formatted_msg, sizeof(formatted_msg), "[%s]: %s", sender_username, message);
    size_t msg_len = strlen(formatted_msg);

    printf("[BROADCAST] Relaying message from '%s' (FD: %d) to %d recipient(s)...\n",
           sender_username, sender_fd, target_count);

    // 2. Perform send() calls OUTSIDE the mutex lock
    for (int i = 0; i < target_count; i++) {
        int dest_fd = target_sockets[i];

        // Send message to target client (MSG_NOSIGNAL prevents SIGPIPE crash if client disconnected)
        ssize_t bytes_sent = send(dest_fd, formatted_msg, msg_len, MSG_NOSIGNAL);

        if (bytes_sent < 0) {
            // Handle send failure safely: remove broken socket and close connection
            perror("[BROADCAST ERROR] send() failed for destination socket");
            printf("[BROADCAST ERROR] Removing unresponsive client (FD: %d)\n", dest_fd);

            remove_client(dest_fd);
            close(dest_fd);
        }
    }
}

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
 */
void *client_handler_thread(void *arg) {
    pthread_detach(pthread_self());

    client_entry_t *entry = (client_entry_t *)arg;
    if (!entry) {
        return NULL;
    }

    int client_fd = entry->socket_fd;
    struct sockaddr_in client_addr = entry->client_addr;

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, sizeof(client_ip));
    int client_port = ntohs(client_addr.sin_port);

    printf("[Worker Thread %lu] [+] Client '%s' connected: %s:%d (FD: %d) [Connected: %d/%d]\n",
           (unsigned long)pthread_self(), entry->username, client_ip, client_port, client_fd,
           get_client_count(), MAX_CLIENTS);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    // Receive loop: Handles messages sent by client
    while ((bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0'; // Null-terminate string

        printf("[%s (%s:%d) | Thread %lu]: %s",
               entry->username, client_ip, client_port, (unsigned long)pthread_self(), buffer);
        if (buffer[bytes_received - 1] != '\n') {
            printf("\n");
        }

        // Broadcast message to all other connected clients
        broadcast_message(client_fd, entry->username, buffer);
    }

    // Handle client disconnect or connection error
    if (bytes_received == 0) {
        printf("[-] Client '%s' (%s:%d) disconnected (recv returned 0).\n",
               entry->username, client_ip, client_port);
    } else {
        perror("[-] recv error");
        printf("[-] Connection lost with client '%s' (%s:%d).\n",
               entry->username, client_ip, client_port);
    }

    // Save username copy before freeing entry in remove_client
    char username_copy[MAX_USERNAME];
    strncpy(username_copy, entry->username, MAX_USERNAME - 1);
    username_copy[MAX_USERNAME - 1] = '\0';

    // Thread Safety: Remove client from global list under mutex protection
    remove_client(client_fd);
    close(client_fd);

    printf("[Worker Thread %lu] Removed '%s' & closed FD %d. Remaining connected clients: %d/%d.\n\n",
           (unsigned long)pthread_self(), username_copy, client_fd, get_client_count(), MAX_CLIENTS);

    return NULL;
}

/**
 * Step 4: Multithreaded Accept Loop
 */
void accept_clients_loop(int server_fd) {
    printf("=====================================================\n");
    printf("  Multithreaded TCP Server listening on port %d...\n", PORT);
    printf("  Max Clients Capacity: %d (Protected by pthread_mutex)\n", MAX_CLIENTS);
    printf("  Waiting for incoming client connections...\n");
    printf("=====================================================\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(struct sockaddr_in);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, sizeof(client_ip));
        int client_port = ntohs(client_addr.sin_port);

        // Generate placeholder username for new client
        char placeholder_name[MAX_USERNAME];
        snprintf(placeholder_name, sizeof(placeholder_name), "Guest_%d", client_fd);

        // Thread Safety: Attempt to add client to global list under mutex protection
        client_entry_t *entry = add_client(client_fd, client_addr, placeholder_name);
        if (entry == NULL) {
            // Requirement: Reject connection with clear message before closing socket if capacity full
            const char *reject_msg = "SERVER REJECTION: Maximum client limit (100) reached. Connection closed.\n";
            send(client_fd, reject_msg, strlen(reject_msg), 0);

            printf("[REJECTED] Client %s:%d (FD: %d) rejected - Server full (%d/%d clients).\n",
                   client_ip, client_port, client_fd, MAX_CLIENTS, MAX_CLIENTS);

            close(client_fd);
            continue;
        }

        printf("[Main Thread] Accepted client '%s' from %s:%d (FD: %d). Spawning worker thread...\n",
               entry->username, client_ip, client_port, client_fd);

        // Spawn a dedicated POSIX thread for this client
        pthread_t tid;
        int rc = pthread_create(&tid, NULL, client_handler_thread, entry);
        if (rc != 0) {
            fprintf(stderr, "Failed to create thread: %s\n", strerror(rc));
            remove_client(client_fd);
            close(client_fd);
            continue;
        }
    }
}

int main(void) {
    // Unbuffer stdout and stderr for instant terminal logs
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

    // 4. Accept clients concurrently
    accept_clients_loop(server_fd);

    // Clean up mutex on shutdown (unreachable infinite loop)
    pthread_mutex_destroy(&clients_mutex);
    close(server_fd);
    return 0;
}
