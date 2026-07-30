#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define CHUNK_SIZE      1024
#define MAX_USERNAME    32
#define MAX_FILENAME    256
#define MAX_CLIENTS     100

typedef struct {
    int32_t type;      // one of the enum values below
    int32_t length;    // EXACT byte size of the payload that follows this header
} Header;

enum {
    CHAT = 1,
    USER_JOIN,
    USER_LEAVE,
    FILE_START,
    FILE_CHUNK,
    FILE_END
};

// FILE_START payload — fixed-size struct, so there's no ambiguity between
// what the client sends and what the server routes on.
// Header.length for FILE_START == sizeof(FileStartPayload)
typedef struct {
    char sender_username[MAX_USERNAME];  // who is sending (for the receiver's UI)
    char target_username[MAX_USERNAME];  // who the server should route this to
    char filename[MAX_FILENAME];         // original filename, receiver saves under this
} FileStartPayload;

// FILE_CHUNK payload is raw binary data. Header.length is the ACTUAL size
// of THIS chunk. The last chunk of any file will almost never be exactly
// CHUNK_SIZE — trust Header.length, never assume CHUNK_SIZE on read.

// FILE_END payload is empty — Header.length == 0. It only signals "close
// the file now."

#endif