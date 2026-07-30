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

typedef struct {
    int socket_fd;
    char username[MAX_USERNAME];
    struct sockaddr_in client_addr;
} client_entry_t;

typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
} temp_client_info_t;

typedef enum {
    ADD_SUCCESS = 0,
    ADD_ERR_FULL = -1,
    ADD_ERR_DUPLICATE = -2
} add_status_t;

static client_entry_t *clients[MAX_CLIENTS] = { NULL };
static int client_count = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Reads EXACTLY `len` bytes from socket.
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
 * Writes EXACTLY `len` bytes to socket.
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
 * Sends structured Header + Payload packet.
 */
int send_packet(int fd, int32_t type, const void *payload, int32_t payload_len) {
    Header header = { .type = type, .length = payload_len };
    if (write_exact(fd, &header, sizeof(Header)) < 0) return -1;
    if (payload_len > 0 && payload != NULL) {
        if (write_exact(fd, payload, (size_t)payload_len) < 0) return -1;
    }
    return 0;
}

/**
 * Reads Header from socket.
 */
int read_header(int fd, Header *header) {
    ssize_t res = read_exact(fd, header, sizeof(Header));
    if (res == (ssize_t)sizeof(Header)) return 1;
    if (res == 0) return 0;
    return -1;
}

/**
 * Reads Payload from socket.
 */
int read_payload(int fd, void *payload_buf, size_t payload_len) {
    if (payload_len == 0) return 1;
    ssize_t res = read_exact(fd, payload_buf, payload_len);
    return (res == (ssize_t)payload_len) ? 1 : -1;
}

/**
 * Constraint: Duplicate check and client addition under the SAME mutex lock.
 * 
 * @param client_fd Client socket descriptor.
 * @param addr Client address structure.
 * @param raw_username Requested username string.
 * @param out_entry Output created entry pointer.
 * @return ADD_SUCCESS, ADD_ERR_FULL, or ADD_ERR_DUPLICATE.
 */
add_status_t add_client(int client_fd, struct sockaddr_in addr, const char *raw_username, client_entry_t **out_entry) {
    pthread_mutex_lock(&clients_mutex);

    // 1. Check capacity limit
    if (client_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&clients_mutex);
        return ADD_ERR_FULL;
    }

    // Constraint: Limit username to MAX_USERNAME (32) chars
    char clean_name[MAX_USERNAME];
    strncpy(clean_name, raw_username, MAX_USERNAME - 1);
    clean_name[MAX_USERNAME - 1] = '\0';

    // Strip trailing line endings
    size_t len = strlen(clean_name);
    while (len > 0 && (clean_name[len - 1] == '\n' || clean_name[len - 1] == '\r')) {
        clean_name[--len] = '\0';
    }

    if (len == 0) {
        snprintf(clean_name, sizeof(clean_name), "Guest_%d", client_fd);
    }

    // 2. CRITICAL: Reject duplicates under the SAME lock!
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != NULL && strcmp(clients[i]->username, clean_name) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            return ADD_ERR_DUPLICATE; // Duplicate name found
        }
    }

    // 3. Add new client entry (lock held continuously)
    client_entry_t *entry = malloc(sizeof(client_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&clients_mutex);
        return ADD_ERR_FULL;
    }

    entry->socket_fd = client_fd;
    entry->client_addr = addr;
    strcpy(entry->username, clean_name);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == NULL) {
            clients[i] = entry;
            client_count++;
            break;
        }
    }

    *out_entry = entry;
    pthread_mutex_unlock(&clients_mutex);
    return ADD_SUCCESS;
}

/**
 * Removes a client from the global client list under mutex protection.
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
 * Returns current count of connected clients.
 */
int get_client_count(void) {
    pthread_mutex_lock(&clients_mutex);
    int count = client_count;
    pthread_mutex_unlock(&clients_mutex);
    return count;
}

/**
 * Broadcasts Header + Payload packet to all clients except sender.
 * Locks mutex ONLY to snapshot target sockets.
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

    if (target_count == 0) return;

    for (int i = 0; i < target_count; i++) {
        int dest_fd = target_sockets[i];
        if (write_exact(dest_fd, &header, sizeof(Header)) < 0) {
            remove_client(dest_fd);
            close(dest_fd);
            continue;
        }
        if (header.length > 0 && payload != NULL) {
            if (write_exact(dest_fd, payload, header.length) < 0) {
                remove_client(dest_fd);
                close(dest_fd);
            }
        }
    }
}

/**
 * Requirement: Broadcast "[username] joined" / "[username] left" on connect/disconnect.
 */
void broadcast_user_event(int sender_fd, int32_t type, const char *username) {
    Header header = { .type = type, .length = (int32_t)strlen(username) };
    broadcast_packet(sender_fd, header, username);
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
 * Worker Thread Handler for Connected Client
 */
void *client_handler_thread(void *arg) {
    pthread_detach(pthread_self());

    temp_client_info_t *temp_info = (temp_client_info_t *)arg;
    int client_fd = temp_info->client_fd;
    struct sockaddr_in client_addr = temp_info->client_addr;
    free(temp_info);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, sizeof(client_ip));
    int client_port = ntohs(client_addr.sin_port);

    // Requirement: First message received is the username (USER_JOIN or CHAT)
    Header first_header;
    int h_res = read_header(client_fd, &first_header);
    if (h_res <= 0) {
        printf("[-] Client %s:%d disconnected before registering username.\n", client_ip, client_port);
        close(client_fd);
        return NULL;
    }

    char raw_username[MAX_USERNAME] = {0};
    if (first_header.length > 0) {
        size_t read_len = (first_header.length < MAX_USERNAME - 1) ? first_header.length : (MAX_USERNAME - 1);
        if (read_payload(client_fd, raw_username, read_len) < 0) {
            printf("[-] Error reading username payload from %s:%d\n", client_ip, client_port);
            close(client_fd);
            return NULL;
        }
        raw_username[read_len] = '\0';

        // Discard any remaining bytes if payload exceeded MAX_USERNAME
        if (first_header.length > (int32_t)read_len) {
            size_t remaining = first_header.length - read_len;
            char *trash = malloc(remaining);
            if (trash) {
                read_exact(client_fd, trash, remaining);
                free(trash);
            }
        }
    } else {
        snprintf(raw_username, sizeof(raw_username), "Guest_%d", client_fd);
    }

    // Constraint: Add client & check duplicate username under the SAME mutex lock
    client_entry_t *entry = NULL;
    add_status_t status = add_client(client_fd, client_addr, raw_username, &entry);

    if (status == ADD_ERR_DUPLICATE) {
        // Constraint: Send error message to client and close politely on duplicate
        char err_msg[BUFFER_SIZE];
        snprintf(err_msg, sizeof(err_msg), "ERROR: Username '%s' is already taken. Connection rejected.\n", raw_username);
        send_packet(client_fd, CHAT, err_msg, (int32_t)strlen(err_msg));

        printf("[REJECTED DUPLICATE] Client %s:%d rejected — Username '%s' already exists.\n",
               client_ip, client_port, raw_username);
        close(client_fd);
        return NULL;
    } else if (status == ADD_ERR_FULL) {
        // Constraint: Send error message to client and close politely on capacity full
        const char *err_msg = "ERROR: Server capacity full (100 clients). Connection rejected.\n";
        send_packet(client_fd, CHAT, err_msg, (int32_t)strlen(err_msg));

        printf("[REJECTED FULL] Client %s:%d rejected — Server full (%d/%d clients).\n",
               client_ip, client_port, MAX_CLIENTS, MAX_CLIENTS);
        close(client_fd);
        return NULL;
    }

    printf("[Worker Thread %lu] [+] Client '%s' registered: %s:%d (FD: %d) [Connected: %d/%d]\n",
           (unsigned long)pthread_self(), entry->username, client_ip, client_port, client_fd,
           get_client_count(), MAX_CLIENTS);

    // Requirement: Broadcast "[username] joined" on connect
    printf("[BROADCAST EVENT] '%s' joined the chat.\n", entry->username);
    broadcast_user_event(client_fd, USER_JOIN, entry->username);

    Header header;

    // Receive loop for subsequent packets
    while (1) {
        h_res = read_header(client_fd, &header);
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

        char *payload = NULL;
        if (header.length > 0) {
            payload = malloc((size_t)header.length + 1);
            if (!payload) break;

            if (read_payload(client_fd, payload, (size_t)header.length) < 0) {
                printf("[-] Error reading payload (%d bytes) from '%s'.\n", header.length, entry->username);
                free(payload);
                break;
            }
            payload[header.length] = '\0';
        }

        switch (header.type) {
            case CHAT: {
                printf("[%s (%s:%d)]: %s", entry->username, client_ip, client_port, payload ? payload : "");
                if (payload && header.length > 0 && payload[header.length - 1] != '\n') {
                    printf("\n");
                }
                broadcast_packet(client_fd, header, payload);
                break;
            }

            case USER_LEAVE: {
                printf("[USER_LEAVE] Client '%s' sent leave packet.\n", entry->username);
                break;
            }

            case FILE_START: {
                printf("[FILE_START] File metadata from '%s' (%d bytes)\n", entry->username, header.length);
                broadcast_packet(client_fd, header, payload);
                break;
            }

            case FILE_CHUNK: {
                printf("[FILE_CHUNK] Relaying file chunk (%d bytes) from '%s'\n", header.length, entry->username);
                broadcast_packet(client_fd, header, payload);
                break;
            }

            case FILE_END: {
                printf("[FILE_END] File transfer complete signal from '%s'\n", entry->username);
                broadcast_packet(client_fd, header, payload);
                break;
            }

            default:
                printf("[UNKNOWN] Header type %d from '%s'\n", header.type, entry->username);
                break;
        }

        if (payload) free(payload);

        if (header.type == USER_LEAVE) {
            break;
        }
    }

    // Save username copy for post-cleanup log and broadcast
    char username_copy[MAX_USERNAME];
    strncpy(username_copy, entry->username, MAX_USERNAME - 1);
    username_copy[MAX_USERNAME - 1] = '\0';

    // Requirement: Broadcast "[username] left" on disconnect
    printf("[BROADCAST EVENT] '%s' left the chat.\n", username_copy);
    broadcast_user_event(client_fd, USER_LEAVE, username_copy);

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
    printf("  Max Clients: %d | Username Check: Atomic Mutex\n", MAX_CLIENTS);
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

        printf("[Main Thread] Accepted connection from %s:%d (FD: %d). Spawning worker thread...\n",
               client_ip, client_port, client_fd);

        temp_client_info_t *temp_info = malloc(sizeof(temp_client_info_t));
        if (!temp_info) {
            close(client_fd);
            continue;
        }
        temp_info->client_fd = client_fd;
        temp_info->client_addr = client_addr;

        pthread_t tid;
        int rc = pthread_create(&tid, NULL, client_handler_thread, temp_info);
        if (rc != 0) {
            fprintf(stderr, "Failed to create thread: %s\n", strerror(rc));
            close(client_fd);
            free(temp_info);
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
