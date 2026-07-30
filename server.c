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

/**
 * Structure representing a connected client entry in the global list
 */
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
 * Helper: Reads EXACTLY `len` bytes from socket file descriptor.
 * Naive recv() may return fewer bytes than requested; this loops until all `len` bytes
 * are received or until connection closes/errors.
 * 
 * @param fd Socket file descriptor.
 * @param buf Output buffer pointer.
 * @param len Exact number of bytes to receive.
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
            // EOF: Client disconnected
            return (total_read == 0) ? 0 : -1;
        } else {
            perror("read_exact error");
            return -1;
        }
    }

    return (ssize_t)total_read;
}

/**
 * Helper: Writes EXACTLY `len` bytes to socket file descriptor.
 * 
 * @param fd Socket file descriptor.
 * @param buf Input buffer pointer.
 * @param len Exact number of bytes to send.
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
            perror("write_exact error");
            return -1;
        }
    }

    return (ssize_t)total_written;
}

/**
 * Helper: Reads a complete Header struct (sizeof(Header) = 8 bytes) from socket.
 * 
 * @param fd Socket descriptor.
 * @param header Pointer to Header output struct.
 * @return 1 on success, 0 on client disconnect, -1 on error.
 */
int read_header(int fd, Header *header) {
    ssize_t res = read_exact(fd, header, sizeof(Header));
    if (res == (ssize_t)sizeof(Header)) {
        return 1;
    } else if (res == 0) {
        return 0; // Clean disconnect
    } else {
        return -1; // Read error
    }
}

/**
 * Helper: Reads exactly payload_len bytes (Header.length) from socket.
 * 
 * @param fd Socket descriptor.
 * @param payload_buf Pointer to destination buffer.
 * @param payload_len Exact number of bytes to read.
 * @return 1 on success, -1 on error.
 */
int read_payload(int fd, void *payload_buf, size_t payload_len) {
    if (payload_len == 0) {
        return 1; // Empty payload (e.g. FILE_END)
    }

    ssize_t res = read_exact(fd, payload_buf, payload_len);
    if (res == (ssize_t)payload_len) {
        return 1;
    }
    return -1;
}

/**
 * Adds a client to the global client list under mutex protection.
 */
client_entry_t *add_client(int client_fd, struct sockaddr_in addr, const char *username) {
    pthread_mutex_lock(&clients_mutex);

    if (client_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
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
 * Broadcasts a structured packet (Header + Payload) to all connected clients except sender.
 * 
 * Locks mutex ONLY to copy target socket descriptors, then releases mutex BEFORE write_exact().
 * 
 * @param sender_fd Socket descriptor of sending client.
 * @param header Structured Header to send.
 * @param payload Payload memory pointer.
 */
void broadcast_packet(int sender_fd, Header header, const void *payload) {
    int target_sockets[MAX_CLIENTS];
    int target_count = 0;

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != NULL && clients[i]->socket_fd != sender_fd) {
            target_sockets[target_count++] = clients[i]->socket_fd;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (target_count == 0) {
        return;
    }

    printf("[BROADCAST] Relaying Packet (Type: %d, Length: %d bytes) to %d recipient(s)...\n",
           header.type, header.length, target_count);

    for (int i = 0; i < target_count; i++) {
        int dest_fd = target_sockets[i];

        // Send Header (8 bytes)
        if (write_exact(dest_fd, &header, sizeof(Header)) < 0) {
            printf("[BROADCAST ERROR] Failed to send Header to FD %d. Removing client.\n", dest_fd);
            remove_client(dest_fd);
            close(dest_fd);
            continue;
        }

        // Send Payload if length > 0
        if (header.length > 0 && payload != NULL) {
            if (write_exact(dest_fd, payload, header.length) < 0) {
                printf("[BROADCAST ERROR] Failed to send Payload to FD %d. Removing client.\n", dest_fd);
                remove_client(dest_fd);
                close(dest_fd);
            }
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
 * 
 * Requirement:
 * - Always receive Header first, then exactly Header.length bytes of payload
 * - Process messages using switch(header.type)
 * - Trust Header.length for payload memory allocation
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

    Header header;

    // Loop: Receive Header first, then exact payload
    while (1) {
        // Step A: Read Header
        int h_res = read_header(client_fd, &header);
        if (h_res <= 0) {
            if (h_res == 0) {
                printf("[-] Client '%s' (%s:%d) disconnected cleanly.\n",
                       entry->username, client_ip, client_port);
            } else {
                printf("[-] Error reading header from client '%s' (%s:%d).\n",
                       entry->username, client_ip, client_port);
            }
            break;
        }

        // Step B: Read Payload (trusting header.length)
        char *payload = NULL;
        if (header.length > 0) {
            payload = malloc((size_t)header.length + 1);
            if (!payload) {
                fprintf(stderr, "Memory allocation failed for payload length %d\n", header.length);
                break;
            }

            if (read_payload(client_fd, payload, (size_t)header.length) < 0) {
                printf("[-] Error reading payload (%d bytes) from client '%s'.\n",
                       header.length, entry->username);
                free(payload);
                break;
            }
            payload[header.length] = '\0'; // Safe null-termination
        }

        // Step C: Process messages using switch(header.type)
        switch (header.type) {
            case CHAT: {
                printf("[CHAT | %s (%s:%d)]: %s",
                       entry->username, client_ip, client_port, payload ? payload : "");
                if (payload && header.length > 0 && payload[header.length - 1] != '\n') {
                    printf("\n");
                }

                // Broadcast CHAT message to other clients
                broadcast_packet(client_fd, header, payload);
                break;
            }

            case USER_JOIN: {
                if (payload && header.length > 0) {
                    pthread_mutex_lock(&clients_mutex);
                    strncpy(entry->username, payload, MAX_USERNAME - 1);
                    entry->username[MAX_USERNAME - 1] = '\0';
                    pthread_mutex_unlock(&clients_mutex);
                }
                printf("[USER_JOIN] Client on FD %d registered username: '%s'\n", client_fd, entry->username);
                broadcast_packet(client_fd, header, payload);
                break;
            }

            case USER_LEAVE: {
                printf("[USER_LEAVE] Client '%s' (FD: %d) sent leave signal.\n",
                       entry->username, client_fd);
                broadcast_packet(client_fd, header, payload);
                break;
            }

            case FILE_START: {
                printf("[FILE_START] Received file metadata from '%s' (%d bytes payload)\n",
                       entry->username, header.length);
                if (header.length >= (int32_t)sizeof(FileStartPayload)) {
                    FileStartPayload *meta = (FileStartPayload *)payload;
                    printf("             File: '%s' | Sender: '%s' -> Target: '%s'\n",
                           meta->filename, meta->sender_username, meta->target_username);
                }
                broadcast_packet(client_fd, header, payload);
                break;
            }

            case FILE_CHUNK: {
                printf("[FILE_CHUNK] Relaying file chunk (%d bytes) from '%s'\n",
                       header.length, entry->username);
                broadcast_packet(client_fd, header, payload);
                break;
            }

            case FILE_END: {
                printf("[FILE_END] Received file transfer complete signal from '%s'\n",
                       entry->username);
                broadcast_packet(client_fd, header, payload);
                break;
            }

            default: {
                printf("[UNKNOWN PACKET] Unknown header type %d (length: %d) from '%s'\n",
                       header.type, header.length, entry->username);
                break;
            }
        }

        if (payload) {
            free(payload);
        }

        if (header.type == USER_LEAVE) {
            break;
        }
    }

    // Cleanup client entry
    char username_copy[MAX_USERNAME];
    strncpy(username_copy, entry->username, MAX_USERNAME - 1);
    username_copy[MAX_USERNAME - 1] = '\0';

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
    printf("  Structured TCP Server listening on port %d...\n", PORT);
    printf("  Protocol: Header (8B) + Payload | Max Clients: %d\n", MAX_CLIENTS);
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

        char placeholder_name[MAX_USERNAME];
        snprintf(placeholder_name, sizeof(placeholder_name), "Guest_%d", client_fd);

        client_entry_t *entry = add_client(client_fd, client_addr, placeholder_name);
        if (entry == NULL) {
            // Rejection Packet
            Header reject_header = { .type = CHAT, .length = 74 };
            const char *reject_msg = "SERVER REJECTION: Maximum client limit (100) reached. Connection closed.\n";
            write_exact(client_fd, &reject_header, sizeof(Header));
            write_exact(client_fd, reject_msg, strlen(reject_msg));

            printf("[REJECTED] Client %s:%d (FD: %d) rejected - Server full (%d/%d clients).\n",
                   client_ip, client_port, client_fd, MAX_CLIENTS, MAX_CLIENTS);

            close(client_fd);
            continue;
        }

        printf("[Main Thread] Accepted client '%s' from %s:%d (FD: %d). Spawning worker thread...\n",
               entry->username, client_ip, client_port, client_fd);

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
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    int server_fd = create_server_socket();
    if (server_fd < 0) {
        exit(EXIT_FAILURE);
    }
    printf("[Step 1] Socket created successfully (FD: %d)\n", server_fd);

    if (bind_server_socket(server_fd, PORT) < 0) {
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("[Step 2] Bound successfully to port %d\n", PORT);

    if (start_listening(server_fd, BACKLOG) < 0) {
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("[Step 3] Multithreaded server listening with backlog queue of %d\n\n", BACKLOG);

    accept_clients_loop(server_fd);

    pthread_mutex_destroy(&clients_mutex);
    close(server_fd);
    return 0;
}
