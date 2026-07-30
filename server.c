#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "protocol.h"
#include "utils.h"

#define PORT        8080
#define BACKLOG     10
#define BUFFER_SIZE 1024

// Robustness payload limits
#define MAX_PAYLOAD_CHAT        4096
#define MAX_PAYLOAD_FILE_CHUNK  (CHUNK_SIZE * 64) // 64 KB limit
#define MAX_PAYLOAD_FILE_START  (int32_t)(sizeof(FileStartPayload) + 128)
#define MAX_PAYLOAD_USER_EVENT  (MAX_USERNAME * 2)

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
 * Validates payload size limits for each message type to prevent memory exhaustion attacks.
 */
int is_payload_size_valid(int32_t type, int32_t length) {
    if (length < 0) return 0;

    switch (type) {
        case CHAT:
            return length <= MAX_PAYLOAD_CHAT;
        case USER_JOIN:
        case USER_LEAVE:
            return length <= MAX_PAYLOAD_USER_EVENT;
        case FILE_START:
            return length <= MAX_PAYLOAD_FILE_START;
        case FILE_CHUNK:
            return length <= MAX_PAYLOAD_FILE_CHUNK;
        case FILE_END:
            return length == 0;
        default:
            return length <= MAX_PAYLOAD_CHAT;
    }
}

/**
 * Searches global client registry for connected target username under mutex protection.
 */
int find_client_socket_by_username(const char *target_username) {
    pthread_mutex_lock(&clients_mutex);
    int target_fd = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != NULL && strcmp(clients[i]->username, target_username) == 0) {
            target_fd = clients[i]->socket_fd;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return target_fd;
}

/**
 * Sends structured Header + Payload packet using send_all().
 */
int send_packet(int fd, int32_t type, const void *payload, int32_t payload_len) {
    Header header = { .type = type, .length = payload_len };
    if (send_all(fd, &header, (int)sizeof(Header)) < 0) {
        fprintf(stderr, "[SERVER ERROR] Failed to send header to FD %d\n", fd);
        return -1;
    }
    if (payload_len > 0 && payload != NULL) {
        if (send_all(fd, (void *)payload, payload_len) < 0) {
            fprintf(stderr, "[SERVER ERROR] Failed to send payload (%d bytes) to FD %d\n", payload_len, fd);
            return -1;
        }
    }
    return 0;
}

/**
 * Reads Header from socket using recv_all().
 */
int read_header(int fd, Header *header) {
    int res = recv_all(fd, header, (int)sizeof(Header));
    if (res == (int)sizeof(Header)) return 1;
    return -1;
}

/**
 * Reads Payload from socket using recv_all().
 */
int read_payload(int fd, void *payload_buf, int payload_len) {
    if (payload_len == 0) return 1;
    int res = recv_all(fd, payload_buf, payload_len);
    return (res == payload_len) ? 1 : -1;
}

/**
 * Adds client & checks duplicate username atomically under clients_mutex.
 */
add_status_t add_client(int client_fd, struct sockaddr_in addr, const char *raw_username, client_entry_t **out_entry) {
    pthread_mutex_lock(&clients_mutex);

    if (client_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&clients_mutex);
        return ADD_ERR_FULL;
    }

    char clean_name[MAX_USERNAME];
    strncpy(clean_name, raw_username, MAX_USERNAME - 1);
    clean_name[MAX_USERNAME - 1] = '\0';

    size_t len = strlen(clean_name);
    while (len > 0 && (clean_name[len - 1] == '\n' || clean_name[len - 1] == '\r')) {
        clean_name[--len] = '\0';
    }

    if (len == 0) {
        snprintf(clean_name, sizeof(clean_name), "Guest_%d", client_fd);
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != NULL && strcmp(clients[i]->username, clean_name) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            return ADD_ERR_DUPLICATE;
        }
    }

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
 * Removes client from global list under mutex protection.
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
 * Broadcasts Header + Payload packet to all clients except sender using send_all().
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
        if (send_all(dest_fd, &header, (int)sizeof(Header)) < 0) {
            fprintf(stderr, "[SERVER WARNING] Failed to broadcast header to FD %d. Removing client.\n", dest_fd);
            remove_client(dest_fd);
            close(dest_fd);
            continue;
        }
        if (header.length > 0 && payload != NULL) {
            if (send_all(dest_fd, (void *)payload, header.length) < 0) {
                fprintf(stderr, "[SERVER WARNING] Failed to broadcast payload to FD %d. Removing client.\n", dest_fd);
                remove_client(dest_fd);
                close(dest_fd);
            }
        }
    }
}

/**
 * Broadcasts user event (USER_JOIN / USER_LEAVE).
 */
void broadcast_user_event(int sender_fd, int32_t type, const char *username) {
    Header header = { .type = type, .length = (int32_t)strlen(username) };
    broadcast_packet(sender_fd, header, username);
}

int create_server_socket(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[SERVER ERROR] Socket creation failed");
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[SERVER ERROR] setsockopt SO_REUSEADDR failed");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

int bind_server_socket(int server_fd, int port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[SERVER ERROR] Bind failed");
        return -1;
    }

    return 0;
}

int start_listening(int server_fd, int backlog) {
    if (listen(server_fd, backlog) < 0) {
        perror("[SERVER ERROR] Listen failed");
        return -1;
    }
    return 0;
}

/**
 * Worker Thread Handler with Comprehensive Robustness & Error Logging.
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

    // Track active file transfer target for cleanup if sender disconnects mid-transfer
    int active_file_target_fd = -1;

    Header first_header;
    int h_res = read_header(client_fd, &first_header);
    if (h_res <= 0) {
        printf("[SERVER WARNING] Client %s:%d disconnected before registering username.\n", client_ip, client_port);
        close(client_fd);
        return NULL;
    }

    // Validate registration payload size
    if (!is_payload_size_valid(first_header.type, first_header.length)) {
        fprintf(stderr, "[SERVER ERROR] Oversized payload (%d bytes) in registration packet from %s:%d. Closing.\n",
                first_header.length, client_ip, client_port);
        close(client_fd);
        return NULL;
    }

    char raw_username[MAX_USERNAME] = {0};
    if (first_header.length > 0) {
        int read_len = (first_header.length < (int32_t)(MAX_USERNAME - 1)) ? first_header.length : (int32_t)(MAX_USERNAME - 1);
        if (read_payload(client_fd, raw_username, read_len) < 0) {
            fprintf(stderr, "[SERVER ERROR] Error reading username payload from %s:%d\n", client_ip, client_port);
            close(client_fd);
            return NULL;
        }
        raw_username[read_len] = '\0';

        if (first_header.length > read_len) {
            int remaining = first_header.length - read_len;
            char *trash = malloc(remaining);
            if (trash) {
                recv_all(client_fd, trash, remaining);
                free(trash);
            }
        }
    } else {
        snprintf(raw_username, sizeof(raw_username), "Guest_%d", client_fd);
    }

    client_entry_t *entry = NULL;
    add_status_t status = add_client(client_fd, client_addr, raw_username, &entry);

    if (status == ADD_ERR_DUPLICATE) {
        char err_msg[BUFFER_SIZE];
        snprintf(err_msg, sizeof(err_msg), "ERROR: Username '%s' is already taken. Connection rejected.\n", raw_username);
        send_packet(client_fd, CHAT, err_msg, (int32_t)strlen(err_msg));

        printf("[REJECTED DUPLICATE] Client %s:%d rejected — Username '%s' already exists.\n",
               client_ip, client_port, raw_username);
        close(client_fd);
        return NULL;
    } else if (status == ADD_ERR_FULL) {
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

    printf("[BROADCAST EVENT] '%s' joined the chat.\n", entry->username);
    broadcast_user_event(client_fd, USER_JOIN, entry->username);

    Header header;

    // Main packet receive loop with robustness checks
    while (1) {
        h_res = read_header(client_fd, &header);
        if (h_res <= 0) {
            printf("[-] Client '%s' (%s:%d) disconnected or read error.\n",
                   entry->username, client_ip, client_port);
            break;
        }

        // Robustness 2: Reject oversized payloads without trying to allocate/read them
        if (!is_payload_size_valid(header.type, header.length)) {
            fprintf(stderr, "[SERVER ERROR] Rejected oversized payload (%d bytes) for type %d from client '%s'. Closing socket.\n",
                    header.length, header.type, entry->username);
            break;
        }

        char *payload = NULL;
        if (header.length > 0) {
            payload = malloc((size_t)header.length + 1);
            if (!payload) {
                fprintf(stderr, "[SERVER ERROR] Out of memory allocating %d bytes payload for '%s'. Closing socket.\n",
                        header.length, entry->username);
                break;
            }

            if (read_payload(client_fd, payload, header.length) < 0) {
                fprintf(stderr, "[SERVER ERROR] Failed reading payload (%d bytes) from '%s'.\n", header.length, entry->username);
                free(payload);
                break;
            }
            payload[header.length] = '\0';
        }

        // Robustness 1: Handle invalid message types by logging and ignoring
        switch (header.type) {
            case CHAT: {
                printf("[%s (%s:%d)]: %s", entry->username, client_ip, client_port, payload ? payload : "");
                if (payload && header.length > 0 && payload[header.length - 1] != '\n') {
                    printf("\n");
                }
                broadcast_packet(client_fd, header, payload);
                break;
            }

            case FILE_START: {
                if (header.length >= (int32_t)sizeof(FileStartPayload) && payload != NULL) {
                    FileStartPayload *meta = (FileStartPayload *)payload;
                    printf("[FILE ROUTER] FILE_START request: File '%s' from '%s' -> Target '%s'\n",
                           meta->filename, entry->username, meta->target_username);

                    int target_fd = find_client_socket_by_username(meta->target_username);

                    if (target_fd < 0) {
                        char err_msg[BUFFER_SIZE];
                        snprintf(err_msg, sizeof(err_msg),
                                 "ERROR: Target user '%s' is not connected. File transfer canceled.\n",
                                 meta->target_username);
                        send_packet(client_fd, CHAT, err_msg, (int32_t)strlen(err_msg));
                        printf("[FILE ROUTER WARNING] Target user '%s' not connected. Rejection sent to '%s'.\n",
                               meta->target_username, entry->username);
                        active_file_target_fd = -1;
                    } else {
                        active_file_target_fd = target_fd;
                        if (send_all(target_fd, &header, (int)sizeof(Header)) < 0 ||
                            send_all(target_fd, payload, header.length) < 0) {
                            fprintf(stderr, "[FILE ROUTER ERROR] Failed to forward FILE_START to target FD %d\n", target_fd);
                            active_file_target_fd = -1;
                        } else {
                            printf("[FILE ROUTER] Forwarded FILE_START ('%s') to '%s' (FD: %d)\n",
                                   meta->filename, meta->target_username, target_fd);
                        }
                    }
                } else {
                    fprintf(stderr, "[SERVER WARNING] Invalid FILE_START payload size (%d bytes) from '%s'. Ignored.\n",
                            header.length, entry->username);
                }
                break;
            }

            case FILE_CHUNK: {
                if (active_file_target_fd > 0 && payload != NULL && header.length > 0) {
                    if (send_all(active_file_target_fd, &header, (int)sizeof(Header)) < 0 ||
                        send_all(active_file_target_fd, payload, header.length) < 0) {
                        fprintf(stderr, "[FILE ROUTER ERROR] Target FD %d write failed during FILE_CHUNK. Interrupted.\n",
                                active_file_target_fd);
                        active_file_target_fd = -1;
                    }
                }
                break;
            }

            case FILE_END: {
                if (active_file_target_fd > 0) {
                    if (send_all(active_file_target_fd, &header, (int)sizeof(Header)) < 0) {
                        fprintf(stderr, "[FILE ROUTER ERROR] Target FD %d write failed during FILE_END.\n",
                                active_file_target_fd);
                    } else {
                        printf("[FILE ROUTER] Forwarded FILE_END to target FD %d. Transfer completed!\n",
                               active_file_target_fd);
                    }
                    active_file_target_fd = -1;
                }
                break;
            }

            case USER_LEAVE: {
                printf("[USER_LEAVE] Client '%s' requested disconnect.\n", entry->username);
                break;
            }

            default:
                // Robustness 1: Log unknown header types and ignore without crashing
                printf("[SERVER WARNING] Unknown message type %d (%d bytes payload) received from '%s'. Message ignored.\n",
                       header.type, header.length, entry->username);
                break;
        }

        if (payload) free(payload);

        if (header.type == USER_LEAVE) {
            break;
        }
    }

    // Robustness 3: Handle client disconnect mid file-transfer and clean up server-side state
    char username_copy[MAX_USERNAME];
    strncpy(username_copy, entry->username, MAX_USERNAME - 1);
    username_copy[MAX_USERNAME - 1] = '\0';

    if (active_file_target_fd > 0) {
        Header end_hdr = { .type = FILE_END, .length = 0 };
        send_all(active_file_target_fd, &end_hdr, (int)sizeof(Header));
        printf("[FILE ROUTER WARNING] Sender '%s' disconnected mid-transfer. Sent FILE_END cleanup to target FD %d.\n",
               username_copy, active_file_target_fd);
        active_file_target_fd = -1;
    }

    printf("[BROADCAST EVENT] '%s' left the chat.\n", username_copy);
    broadcast_user_event(client_fd, USER_LEAVE, username_copy);

    remove_client(client_fd);
    close(client_fd);

    printf("[Worker Thread %lu] Removed '%s' & closed FD %d. Remaining connected clients: %d/%d.\n\n",
           (unsigned long)pthread_self(), username_copy, client_fd, get_client_count(), MAX_CLIENTS);

    return NULL;
}

void accept_clients_loop(int server_fd) {
    printf("=====================================================\n");
    printf("  Robust Structured TCP Server on port %d...\n", PORT);
    printf("  Features: Fault Isolation, Size Check, Mid-Transfer Cleanup\n");
    printf("=====================================================\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(struct sockaddr_in);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("[SERVER ERROR] Accept failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, sizeof(client_ip));
        int client_port = ntohs(client_addr.sin_port);

        printf("[Main Thread] Accepted connection from %s:%d (FD: %d). Spawning worker thread...\n",
               client_ip, client_port, client_fd);

        temp_client_info_t *temp_info = malloc(sizeof(temp_client_info_t));
        if (!temp_info) {
            fprintf(stderr, "[SERVER ERROR] Memory allocation failed for client connection info\n");
            close(client_fd);
            continue;
        }
        temp_info->client_fd = client_fd;
        temp_info->client_addr = client_addr;

        pthread_t tid;
        int rc = pthread_create(&tid, NULL, client_handler_thread, temp_info);
        if (rc != 0) {
            fprintf(stderr, "[SERVER ERROR] Failed to create worker thread: %s\n", strerror(rc));
            close(client_fd);
            free(temp_info);
            continue;
        }
    }
}

int main(void) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    // Ignore SIGPIPE so broken socket writes don't kill server process
    signal(SIGPIPE, SIG_IGN);

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
    printf("[Step 3] Server listening with backlog queue of %d\n\n", BACKLOG);

    accept_clients_loop(server_fd);

    pthread_mutex_destroy(&clients_mutex);
    close(server_fd);
    return 0;
}
