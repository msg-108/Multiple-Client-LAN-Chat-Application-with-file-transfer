#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "protocol.h"
#include "utils.h"

#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUFFER_SIZE  1024

// Robustness payload limits client-side
#define MAX_PAYLOAD_CHAT        4096
#define MAX_PAYLOAD_FILE_CHUNK  (CHUNK_SIZE * 64) // 64 KB limit
#define MAX_PAYLOAD_FILE_START  (int32_t)(sizeof(FileStartPayload) + 128)
#define MAX_PAYLOAD_USER_EVENT  (MAX_USERNAME * 2)

// State flag to signal thread shutdown
static atomic_int is_running = 1;

// Global active file reception handle & progress tracker for receiver thread
static FILE *active_recv_fp = NULL;
static char active_recv_filename[MAX_FILENAME + 32] = {0};
static char active_recv_sender[MAX_USERNAME] = {0};
static int64_t active_recv_total_size = 0;
static int64_t active_recv_current_bytes = 0;

/**
 * Validates payload size limits for incoming packet types to prevent memory errors or segfaults.
 */
int is_client_payload_valid(int32_t type, int32_t length) {
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
 * Sends a structured message (Header + Payload) using send_all().
 */
int send_packet(int sock_fd, int32_t type, const void *payload, int32_t payload_len) {
    Header header;
    header.type = type;
    header.length = payload_len;

    if (send_all(sock_fd, &header, (int)sizeof(Header)) < 0) {
        return -1;
    }

    if (payload_len > 0 && payload != NULL) {
        if (send_all(sock_fd, (void *)payload, payload_len) < 0) {
            return -1;
        }
    }

    return 0;
}

/**
 * Sends a file from client to target user with full error handling for target file not found & mid-transfer disconnects.
 */
int send_file_to_user(int sock_fd, const char *sender_name, const char *target_name, const char *filepath) {
    // 1. Target file not found (sender side, before FILE_START)
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("[CLIENT ERROR] Target file not found or permission denied");
        printf("[CLIENT ERROR] Aborting file transfer: Could not open '%s'.\n", filepath);
        return -1;
    }

    // Get total file size using fseek / ftell
    fseek(fp, 0, SEEK_END);
    int64_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    const char *basename = strrchr(filepath, '/');
    if (!basename) basename = strrchr(filepath, '\\');
    basename = (basename) ? (basename + 1) : filepath;

    FileStartPayload meta;
    memset(&meta, 0, sizeof(meta));
    strncpy(meta.sender_username, sender_name, MAX_USERNAME - 1);
    strncpy(meta.target_username, target_name, MAX_USERNAME - 1);
    strncpy(meta.filename, basename, MAX_FILENAME - 1);
    meta.file_size = file_size;

    printf("[CLIENT] Initiating file transfer: '%s' -> '%s' (File: %s, %" PRId64 " bytes)...\n",
           sender_name, target_name, basename, file_size);

    if (send_packet(sock_fd, FILE_START, &meta, (int32_t)sizeof(FileStartPayload)) < 0) {
        perror("[CLIENT ERROR] Failed to send FILE_START packet");
        fclose(fp);
        return -1;
    }

    char chunk_buffer[CHUNK_SIZE];
    int bytes_read = 0;
    int64_t total_bytes_sent = 0;

    // 3. Connection interrupted mid-transfer (sender side handling)
    while ((bytes_read = read_file_chunk(fp, chunk_buffer, CHUNK_SIZE)) > 0) {
        Header chunk_header = { .type = FILE_CHUNK, .length = bytes_read };
        if (send_all(sock_fd, &chunk_header, (int)sizeof(Header)) < 0 ||
            send_all(sock_fd, chunk_buffer, bytes_read) < 0) {
            printf("\n[CLIENT ERROR] Connection interrupted mid-transfer while sending '%s'. Aborting.\n", basename);
            fclose(fp);
            return -1;
        }
        total_bytes_sent += bytes_read;

        int percent = (file_size > 0) ? (int)((total_bytes_sent * 100) / file_size) : 100;
        printf("\r[SEND PROGRESS] Sending '%s': %d%% (%" PRId64 "/%" PRId64 " bytes)",
               basename, percent, total_bytes_sent, file_size);
        fflush(stdout);
    }

    printf("\n");
    fclose(fp);

    Header end_header = { .type = FILE_END, .length = 0 };
    if (send_all(sock_fd, &end_header, (int)sizeof(Header)) < 0) {
        printf("[CLIENT ERROR] Connection interrupted while sending FILE_END for '%s'.\n", basename);
        return -1;
    }

    printf("[CLIENT] File transfer complete: '%s' (%" PRId64 " bytes) sent successfully to '%s'!\n",
           basename, total_bytes_sent, target_name);
    return 0;
}

/**
 * Receiver Worker Thread Routine with Partial File Cleanup on Error.
 */
void *receive_handler_thread(void *arg) {
    int sock_fd = (int)(intptr_t)arg;
    Header header;

    while (is_running) {
        int h_res = recv_all(sock_fd, &header, (int)sizeof(Header));
        if (h_res < 0) {
            if (is_running) {
                printf("\n[CLIENT ERROR] Server unexpectedly closed the connection.\n");
                is_running = 0;
            }
            break;
        }

        if (!is_client_payload_valid(header.type, header.length)) {
            printf("\n[CLIENT WARNING] Malformed packet received from server (type: %d, length: %d). Ignoring packet.\n",
                   header.type, header.length);
            if (header.length > 0 && header.length < 1048576) {
                char *discard = malloc((size_t)header.length);
                if (discard) {
                    recv_all(sock_fd, discard, header.length);
                    free(discard);
                }
            }
            continue;
        }

        char *payload = NULL;
        if (header.length > 0) {
            payload = malloc((size_t)header.length + 1);
            if (!payload) {
                printf("\n[CLIENT ERROR] Memory allocation failed for incoming payload (%d bytes).\n", header.length);
                break;
            }

            if (recv_all(sock_fd, payload, header.length) < 0) {
                free(payload);
                if (is_running) {
                    printf("\n[CLIENT ERROR] Server disconnected while reading payload.\n");
                    is_running = 0;
                }
                break;
            }
            payload[header.length] = '\0';
        }

        switch (header.type) {
            case CHAT:
                // Displays CHAT messages or Server Error Rejections (e.g. Target username not connected)
                printf("%s", payload ? payload : "");
                if (payload && header.length > 0 && payload[header.length - 1] != '\n') {
                    printf("\n");
                }
                fflush(stdout);
                break;

            case USER_JOIN:
                printf("[+] %s joined the chat\n", (payload && header.length > 0) ? payload : "Guest");
                fflush(stdout);
                break;

            case USER_LEAVE:
                printf("[-] %s left the chat\n", (payload && header.length > 0) ? payload : "Guest");
                fflush(stdout);
                break;

            case FILE_START:
                if (header.length >= (int32_t)sizeof(FileStartPayload) && payload != NULL) {
                    FileStartPayload *meta = (FileStartPayload *)payload;
                    
                    if (active_recv_fp != NULL) {
                        fclose(active_recv_fp);
                        remove(active_recv_filename); // Clean up any previous unclosed file
                        active_recv_fp = NULL;
                    }

                    strncpy(active_recv_filename, meta->filename, sizeof(active_recv_filename) - 1);
                    strncpy(active_recv_sender, meta->sender_username, sizeof(active_recv_sender) - 1);
                    active_recv_total_size = meta->file_size;
                    active_recv_current_bytes = 0;

                    printf("Receiving %s from %s (%" PRId64 " bytes)...\n",
                           meta->filename, meta->sender_username, meta->file_size);
                    fflush(stdout);

                    active_recv_fp = fopen(meta->filename, "wb");
                    if (!active_recv_fp) {
                        perror("[CLIENT ERROR] Failed to open file for writing");
                    }
                }
                break;

            case FILE_CHUNK:
                if (active_recv_fp != NULL && payload != NULL && header.length > 0) {
                    if (write_file_chunk(active_recv_fp, payload, header.length) != 0) {
                        fprintf(stderr, "\n[CLIENT ERROR] Failed to write file chunk to disk!\n");
                    } else {
                        active_recv_current_bytes += header.length;
                        int percent = (active_recv_total_size > 0) ? (int)((active_recv_current_bytes * 100) / active_recv_total_size) : 100;
                        printf("\r[RECV PROGRESS] Receiving '%s': %d%% (%" PRId64 "/%" PRId64 " bytes)",
                               active_recv_filename, percent, active_recv_current_bytes, active_recv_total_size);
                        fflush(stdout);
                    }
                }
                break;

            case FILE_END:
                if (active_recv_fp != NULL) {
                    fclose(active_recv_fp);
                    active_recv_fp = NULL;

                    // 3. Connection interrupted mid-transfer (incomplete file cleanup)
                    if (active_recv_total_size > 0 && active_recv_current_bytes < active_recv_total_size) {
                        remove(active_recv_filename); // Remove incomplete corrupt file
                        printf("\n[CLIENT ERROR] File transfer of '%s' from '%s' was interrupted prematurely (%" PRId64 "/%" PRId64 " bytes received). Incomplete file removed.\n",
                               active_recv_filename, active_recv_sender, active_recv_current_bytes, active_recv_total_size);
                    } else {
                        printf("\nFile transfer complete for '%s' from '%s' (%" PRId64 " bytes)!\n",
                               active_recv_filename, active_recv_sender, active_recv_current_bytes);
                    }
                    fflush(stdout);
                }
                break;

            default:
                printf("\n[CLIENT WARNING] Malformed or unknown message type %d (%d bytes) received from server. Ignored.\n",
                       header.type, header.length);
                fflush(stdout);
                break;
        }

        if (payload) free(payload);
    }

    // 3. Connection dropped mid-transfer cleanup
    if (active_recv_fp != NULL) {
        fclose(active_recv_fp);
        active_recv_fp = NULL;
        remove(active_recv_filename); // Delete incomplete partial file
        printf("\n[CLIENT ERROR] Connection dropped mid-transfer of '%s' from '%s'. Incomplete file removed.\n",
               active_recv_filename, active_recv_sender);
        fflush(stdout);
    }

    return NULL;
}

int create_client_socket(void) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("[CLIENT ERROR] Socket creation error");
        return -1;
    }
    set_socket_nosigpipe(sock_fd);
    return sock_fd;
}

int connect_to_server(int sock_fd, const char *ip, int port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("[CLIENT ERROR] Invalid or unsupported IP address format");
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[CLIENT ERROR] Connection to server failed");
        return -1;
    }

    return 0;
}

int establish_connection(const char *server_ip, int port) {
    int sock_fd = create_client_socket();
    if (sock_fd < 0) return -1;
    if (connect_to_server(sock_fd, server_ip, port) < 0) {
        close_socket(sock_fd);
        return -1;
    }
    return sock_fd;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (init_sockets() < 0) {
        exit(EXIT_FAILURE);
    }

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    const char *server_ip = DEFAULT_IP;
    int port = DEFAULT_PORT;
    const char *cli_username = NULL;

    if (argc == 2) {
        cli_username = argv[1];
    } else if (argc == 3) {
        server_ip = argv[1];
        port = atoi(argv[2]);
    } else if (argc >= 4) {
        server_ip = argv[1];
        port = atoi(argv[2]);
        cli_username = argv[3];
    }

    printf("Connecting to server at %s:%d...\n", server_ip, port);

    int sock_fd = establish_connection(server_ip, port);
    if (sock_fd < 0) {
        exit(EXIT_FAILURE);
    }

    printf("Connected to server at %s:%d!\n\n", server_ip, port);

    char username[MAX_USERNAME] = {0};
    int registered = 0;

    while (!registered) {
        if (cli_username != NULL && strlen(cli_username) > 0) {
            strncpy(username, cli_username, MAX_USERNAME - 1);
            username[MAX_USERNAME - 1] = '\0';
        } else {
            printf("Enter your username (max %d chars): ", MAX_USERNAME - 1);
            fflush(stdout);

            if (fgets(username, sizeof(username), stdin) == NULL) {
                printf("\nNo username entered. Exiting.\n");
                close(sock_fd);
                exit(EXIT_SUCCESS);
            }

            size_t len = strlen(username);
            while (len > 0 && (username[len - 1] == '\n' || username[len - 1] == '\r')) {
                username[--len] = '\0';
            }

            if (len == 0) {
                printf("Username cannot be empty. Please try again.\n");
                continue;
            }
        }

        if (send_packet(sock_fd, USER_JOIN, username, (int32_t)strlen(username)) < 0) {
            perror("[CLIENT ERROR] Error sending username registration packet");
            close(sock_fd);
            exit(EXIT_FAILURE);
        }

#ifdef _WIN32
        DWORD tv = 200;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif

        Header resp_header;
        int h_res = recv_all(sock_fd, &resp_header, (int)sizeof(Header));

#ifdef _WIN32
        DWORD tv_zero = 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_zero, sizeof(tv_zero));
#else
        struct timeval tv_zero;
        tv_zero.tv_sec = 0;
        tv_zero.tv_usec = 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_zero, sizeof(tv_zero));
#endif

        if (h_res == (int)sizeof(Header)) {
            char *resp_payload = NULL;
            if (resp_header.length > 0) {
                resp_payload = malloc((size_t)resp_header.length + 1);
                if (resp_payload) {
                    if (recv_all(sock_fd, resp_payload, resp_header.length) < 0) {
                        free(resp_payload);
                        resp_payload = NULL;
                    } else {
                        resp_payload[resp_header.length] = '\0';
                    }
                }
            }

            if (resp_payload && (strstr(resp_payload, "ERROR:") || strstr(resp_payload, "already taken") || strstr(resp_payload, "rejected"))) {
                printf("\n[REJECTED] %s", resp_payload);
                if (resp_payload[strlen(resp_payload) - 1] != '\n') printf("\n");
                free(resp_payload);

                cli_username = NULL;

                close(sock_fd);
                printf("\nReconnecting to server for another attempt...\n");
                sock_fd = establish_connection(server_ip, port);
                if (sock_fd < 0) {
                    printf("[CLIENT ERROR] Failed to reconnect to server after rejection.\n");
                    exit(EXIT_FAILURE);
                }

                continue;
            }

            if (resp_payload) free(resp_payload);
        }

        registered = 1;
    }

    printf("[+] Registered successfully as '%s'!\n\n", username);

    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receive_handler_thread, (void *)(intptr_t)sock_fd) != 0) {
        perror("[CLIENT ERROR] Failed to create receiving thread");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    printf("==================================================\n");
    printf("  Structured Protocol Chat & File Transfer Client\n");
    printf("  Logged in as: %s\n", username);
    printf("  Commands:\n");
    printf("    /msg <message>              - Send chat message\n");
    printf("    /file <username> <filename> - Transfer file\n");
    printf("    /quit                       - Exit application\n");
    printf("==================================================\n\n");

    char input_buffer[BUFFER_SIZE];
    while (is_running && fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
        size_t len = strlen(input_buffer);
        while (len > 0 && (input_buffer[len - 1] == '\n' || input_buffer[len - 1] == '\r')) {
            input_buffer[--len] = '\0';
        }
        if (len == 0) continue;

        if (input_buffer[0] == '/') {
            if (strcmp(input_buffer, "/quit") == 0 || strcmp(input_buffer, "/exit") == 0) {
                printf("[CLIENT] Sending USER_LEAVE packet and disconnecting...\n");
                send_packet(sock_fd, USER_LEAVE, username, (int32_t)strlen(username));
                break;
            }
            else if (strncmp(input_buffer, "/msg", 4) == 0 && (input_buffer[4] == ' ' || input_buffer[4] == '\0')) {
                const char *msg_content = input_buffer + 4;
                while (*msg_content == ' ') msg_content++;

                if (strlen(msg_content) == 0) {
                    printf("[CLIENT ERROR] Invalid command input. Usage: /msg <message>\n");
                } else {
                    char formatted_msg[BUFFER_SIZE + MAX_USERNAME + 16];
                    snprintf(formatted_msg, sizeof(formatted_msg), "[%s]: %s\n", username, msg_content);
                    if (send_packet(sock_fd, CHAT, formatted_msg, (int32_t)strlen(formatted_msg)) < 0) {
                        perror("[CLIENT ERROR] Send packet failed");
                        break;
                    }
                }
            }
            else if ((strncmp(input_buffer, "/file", 5) == 0 && (input_buffer[5] == ' ' || input_buffer[5] == '\0')) ||
                     (strncmp(input_buffer, "/sendfile", 9) == 0 && (input_buffer[9] == ' ' || input_buffer[9] == '\0'))) {
                char cmd[32] = {0};
                char target_user[MAX_USERNAME] = {0};
                char filepath[256] = {0};

                int count = sscanf(input_buffer, "%s %s %255s", cmd, target_user, filepath);
                if (count == 3 && strlen(target_user) > 0 && strlen(filepath) > 0) {
                    send_file_to_user(sock_fd, username, target_user, filepath);
                } else {
                    printf("[CLIENT ERROR] Invalid command input. Usage: /file <username> <filename>\n");
                }
            }
            else {
                printf("[CLIENT ERROR] Invalid command input: '%s'\n", input_buffer);
                printf("Available commands:\n");
                printf("  /msg <message>              - Send chat message\n");
                printf("  /file <username> <filename> - Transfer file\n");
                printf("  /quit                       - Disconnect gracefully\n");
            }
        }
        else {
            char formatted_msg[BUFFER_SIZE + MAX_USERNAME + 16];
            snprintf(formatted_msg, sizeof(formatted_msg), "[%s]: %s\n", username, input_buffer);

            if (send_packet(sock_fd, CHAT, formatted_msg, (int32_t)strlen(formatted_msg)) < 0) {
                perror("[CLIENT ERROR] Send packet failed");
                break;
            }
        }
    }

    is_running = 0;
    close_socket(sock_fd);
    pthread_join(recv_thread, NULL);
    cleanup_sockets();

    printf("[CLIENT] Connection closed gracefully. Goodbye!\n");
    return 0;
}
