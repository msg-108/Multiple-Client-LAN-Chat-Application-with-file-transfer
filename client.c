#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdint.h>
#include "protocol.h"
#include "utils.h"

#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUFFER_SIZE  1024

// State flag to signal thread shutdown
static volatile int is_running = 1;

// Global active file reception handle & progress tracker for receiver thread
static FILE *active_recv_fp = NULL;
static char active_recv_filename[MAX_FILENAME + 32] = {0};
static char active_recv_sender[MAX_USERNAME] = {0};
static int64_t active_recv_total_size = 0;
static int64_t active_recv_current_bytes = 0;

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
 * Sends a file from client to target user with in-place (\r) progress indicator.
 * 
 * @param sock_fd Socket file descriptor.
 * @param sender_name Username of sender.
 * @param target_name Username of destination target user.
 * @param filepath Local path to file.
 * @return 0 on success, -1 on failure.
 */
int send_file_to_user(int sock_fd, const char *sender_name, const char *target_name, const char *filepath) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("[CLIENT ERROR] Failed to open file for reading");
        printf("[CLIENT ERROR] Aborting file transfer for '%s' (file not found or permission denied).\n", filepath);
        return -1;
    }

    // Get total file size using fseek / ftell
    fseek(fp, 0, SEEK_END);
    int64_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    const char *basename = strrchr(filepath, '/');
    if (!basename) basename = strrchr(filepath, '\\');
    basename = (basename) ? (basename + 1) : filepath;

    // Construct FileStartPayload with total file_size
    FileStartPayload meta;
    memset(&meta, 0, sizeof(meta));
    strncpy(meta.sender_username, sender_name, MAX_USERNAME - 1);
    strncpy(meta.target_username, target_name, MAX_USERNAME - 1);
    strncpy(meta.filename, basename, MAX_FILENAME - 1);
    meta.file_size = file_size;

    printf("[CLIENT] Initiating file transfer: '%s' -> '%s' (File: %s, %ld bytes)...\n",
           sender_name, target_name, basename, (long)file_size);

    if (send_packet(sock_fd, FILE_START, &meta, (int32_t)sizeof(FileStartPayload)) < 0) {
        perror("[CLIENT ERROR] Failed to send FILE_START packet");
        fclose(fp);
        return -1;
    }

    char chunk_buffer[CHUNK_SIZE];
    int bytes_read = 0;
    int64_t total_bytes_sent = 0;

    // Read and transmit binary chunks with in-place (\r) progress indicator
    while ((bytes_read = read_file_chunk(fp, chunk_buffer, CHUNK_SIZE)) > 0) {
        Header chunk_header = { .type = FILE_CHUNK, .length = bytes_read };
        if (send_all(sock_fd, &chunk_header, (int)sizeof(Header)) < 0 ||
            send_all(sock_fd, chunk_buffer, bytes_read) < 0) {
            perror("\n[CLIENT ERROR] Error sending FILE_CHUNK packet");
            fclose(fp);
            return -1;
        }
        total_bytes_sent += bytes_read;

        int percent = (file_size > 0) ? (int)((total_bytes_sent * 100) / file_size) : 100;
        printf("\r[SEND PROGRESS] Sending '%s': %d%% (%ld/%ld bytes)",
               basename, percent, (long)total_bytes_sent, (long)file_size);
        fflush(stdout);
    }

    printf("\n"); // Newline after in-place progress updates
    fclose(fp);

    Header end_header = { .type = FILE_END, .length = 0 };
    if (send_all(sock_fd, &end_header, (int)sizeof(Header)) < 0) {
        perror("[CLIENT ERROR] Failed to send FILE_END packet");
        return -1;
    }

    printf("[CLIENT] File transfer complete: '%s' (%ld bytes) sent successfully to '%s'!\n",
           basename, (long)total_bytes_sent, target_name);
    return 0;
}

/**
 * Receiver Worker Thread Routine with in-place (\r) progress indicator.
 */
void *receive_handler_thread(void *arg) {
    int sock_fd = (int)(intptr_t)arg;
    Header header;

    while (is_running) {
        int h_res = recv_all(sock_fd, &header, (int)sizeof(Header));
        if (h_res < 0) {
            if (is_running) {
                printf("\n[-] Server disconnected or read error.\n");
                is_running = 0;
            }
            break;
        }

        char *payload = NULL;
        if (header.length > 0) {
            payload = malloc((size_t)header.length + 1);
            if (!payload) break;

            if (recv_all(sock_fd, payload, header.length) < 0) {
                free(payload);
                if (is_running) {
                    printf("\n[-] Error reading payload from server.\n");
                    is_running = 0;
                }
                break;
            }
            payload[header.length] = '\0';
        }

        switch (header.type) {
            case CHAT:
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
                        active_recv_fp = NULL;
                    }

                    strncpy(active_recv_filename, meta->filename, sizeof(active_recv_filename) - 1);
                    strncpy(active_recv_sender, meta->sender_username, sizeof(active_recv_sender) - 1);
                    active_recv_total_size = meta->file_size;
                    active_recv_current_bytes = 0;

                    printf("Receiving %s from %s (%ld bytes)...\n",
                           meta->filename, meta->sender_username, (long)meta->file_size);
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
                        fprintf(stderr, "\n[CLIENT ERROR] Failed to write file chunk!\n");
                    } else {
                        active_recv_current_bytes += header.length;
                        int percent = (active_recv_total_size > 0) ? (int)((active_recv_current_bytes * 100) / active_recv_total_size) : 100;
                        printf("\r[RECV PROGRESS] Receiving '%s': %d%% (%ld/%ld bytes)",
                               active_recv_filename, percent, (long)active_recv_current_bytes, (long)active_recv_total_size);
                        fflush(stdout);
                    }
                }
                break;

            case FILE_END:
                if (active_recv_fp != NULL) {
                    fclose(active_recv_fp);
                    active_recv_fp = NULL;
                    printf("\nFile transfer complete for '%s' from '%s'!\n", active_recv_filename, active_recv_sender);
                    fflush(stdout);
                }
                break;

            default:
                if (payload) printf("%s\n", payload);
                fflush(stdout);
                break;
        }

        if (payload) free(payload);
    }

    if (active_recv_fp != NULL) {
        fclose(active_recv_fp);
        active_recv_fp = NULL;
        printf("\n[CLIENT ERROR] File transfer of '%s' from '%s' was interrupted due to connection drop.\n",
               active_recv_filename, active_recv_sender);
        fflush(stdout);
    }

    return NULL;
}

int create_client_socket(void) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation error");
        return -1;
    }
    return sock_fd;
}

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
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

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

    int sock_fd = create_client_socket();
    if (sock_fd < 0) {
        exit(EXIT_FAILURE);
    }

    if (connect_to_server(sock_fd, server_ip, port) < 0) {
        close(sock_fd);
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
            perror("[-] Error sending username registration packet");
            close(sock_fd);
            exit(EXIT_FAILURE);
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        Header resp_header;
        int h_res = recv_all(sock_fd, &resp_header, (int)sizeof(Header));

        tv.tv_sec = 0;
        tv.tv_usec = 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (h_res == (int)sizeof(Header)) {
            char *resp_payload = NULL;
            if (resp_header.length > 0) {
                resp_payload = malloc((size_t)resp_header.length + 1);
                if (resp_payload) {
                    recv_all(sock_fd, resp_payload, resp_header.length);
                    resp_payload[resp_header.length] = '\0';
                }
            }

            if (resp_payload && (strstr(resp_payload, "ERROR:") || strstr(resp_payload, "already taken") || strstr(resp_payload, "rejected"))) {
                printf("\n[REJECTED] %s", resp_payload);
                if (resp_payload[strlen(resp_payload) - 1] != '\n') printf("\n");
                free(resp_payload);

                cli_username = NULL;
                continue;
            }

            if (resp_payload) free(resp_payload);
        }

        registered = 1;
    }

    printf("[+] Registered successfully as '%s'!\n\n", username);

    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receive_handler_thread, (void *)(intptr_t)sock_fd) != 0) {
        perror("Failed to create receiving thread");
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
                    printf("[CLIENT] Usage: /msg <message>\n");
                } else {
                    char formatted_msg[BUFFER_SIZE + MAX_USERNAME + 16];
                    snprintf(formatted_msg, sizeof(formatted_msg), "[%s]: %s\n", username, msg_content);
                    if (send_packet(sock_fd, CHAT, formatted_msg, (int32_t)strlen(formatted_msg)) < 0) {
                        perror("[-] Send packet failed");
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
                    printf("[CLIENT] Usage: /file <username> <filename>\n");
                }
            }
            else {
                printf("[CLIENT] Unknown command: '%s'\n", input_buffer);
                printf("[CLIENT] Available commands:\n");
                printf("  /msg <message>              - Send chat message\n");
                printf("  /file <username> <filename> - Transfer file\n");
                printf("  /quit                       - Disconnect gracefully\n");
            }
        }
        else {
            char formatted_msg[BUFFER_SIZE + MAX_USERNAME + 16];
            snprintf(formatted_msg, sizeof(formatted_msg), "[%s]: %s\n", username, input_buffer);

            if (send_packet(sock_fd, CHAT, formatted_msg, (int32_t)strlen(formatted_msg)) < 0) {
                perror("[-] Send packet failed");
                break;
            }
        }
    }

    is_running = 0;
    close(sock_fd);
    pthread_join(recv_thread, NULL);

    printf("Connection closed gracefully.\n");
    return 0;
}
