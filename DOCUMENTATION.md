# Multiple-Client LAN Chat Application with File Transfer
**Project Documentation, Presentation, and Viva Reference**

---

## 1. Project Overview

This project is a multi-client Local Area Network (LAN) chat application that supports real-time broadcasting and peer-to-peer binary file transfers. At its core, the system allows multiple users to connect to a central server, register unique usernames, exchange chat messages instantly, and send files to one another. 

**High-Level Architecture**: 
The system operates on a client-server model over a single TCP connection per client. The server acts as a central router. When a client sends a chat message, the server reads it and broadcasts it to all other connected clients. When a client initiates a file transfer, the server inspects the destination username, finds the corresponding target socket, and forwards the file chunks directly to the recipient in real-time, without storing the file on the server's disk.

**Technologies Used**:
- **POSIX Sockets (C11)**: Used for the underlying TCP stream connections, ensuring reliable, ordered delivery of bytes across the network.
- **Pthreads**: Necessary because socket I/O calls (like `recv()` and `accept()`) are blocking. Multithreading allows the server to handle multiple clients simultaneously without one slow client freezing the entire server.
- **Mutexes (`pthread_mutex_t`)**: Essential for preventing race conditions. When multiple threads try to access or modify the shared global list of connected clients concurrently (e.g., during connection, disconnection, or username validation), mutexes ensure these operations are thread-safe.

---

## 2. Protocol Design

To ensure the server and clients correctly interpret the raw stream of bytes over TCP, a custom Application-Layer Protocol was designed. 

### `protocol.h` Breakdown
Every message sent over the wire begins with a fixed 8-byte `Header`, defined as:
```c
typedef struct {
    int32_t type;      // The type of message (4 bytes)
    int32_t length;    // The EXACT byte size of the payload (4 bytes)
} Header;
```

**Message Types (`enum`)**:
- `CHAT` (1): Payload is a null-terminated string containing the chat message.
- `USER_JOIN` (2): Payload is a string containing the registering username.
- `USER_LEAVE` (3): Payload is a string containing the disconnecting username.
- `FILE_START` (4): Payload is a fixed-size `FileStartPayload` struct.
- `FILE_CHUNK` (5): Payload is raw binary file data. Length varies (up to 1024).
- `FILE_END` (6): Payload is empty (`length` = 0). Signals the end of a transfer.

**The `FileStartPayload` Struct**:
```c
typedef struct {
    char sender_username[32];  // Sender's name for receiver's UI
    char target_username[32];  // Destination name for server routing
    char filename[256];        // The file's name to save on disk
    int64_t file_size;         // Total bytes for the progress bar
} FileStartPayload;
```

**Over-The-Wire Flow**:
When a client sends a chat message, it sends 8 bytes of Header (`type = 1`, `length = payload_size`), immediately followed by `length` bytes of the chat string. The receiver always reads exactly 8 bytes first, interprets the length, and then reads exactly `length` bytes.

**Design Justification**:
A fixed-Header/Payload-Length design was chosen over a delimiter-based protocol (like using `\r\n` to mark the end of a message). A viva examiner should note that delimiter protocols require scanning every incoming byte to find the boundary, which is highly inefficient. Furthermore, binary file transfers (`FILE_CHUNK`) naturally contain random bytes, meaning a `\r\n` sequence could appear naturally in a PDF or image file, causing premature message truncation. The length-prefix design allows precise memory allocation and immediate bulk reads (`recv_all`) without inspecting the payload contents.

---

## 3. Server Architecture (Server & Protocol Role)

**Thread-per-Client Lifecycle**:
The main server thread runs an infinite `accept()` loop. When a new TCP connection is established, the server dynamically allocates memory for the client's socket information and spawns a detached worker thread (`client_handler_thread`). This thread reads the initial `USER_JOIN` packet, validates the username, and enters an infinite `while(1)` loop waiting for incoming headers. When the client disconnects (or sends `USER_LEAVE`), the loop breaks, the thread removes the client from the registry, closes the socket, and exits, automatically cleaning up resources.

**The Client Registry and Atomic Duplicate Checking**:
The server maintains a global array `clients[MAX_CLIENTS]` protected by `clients_mutex`. When a client joins, the server checks if the username exists. This is done inside `add_client()`.
*The Race Condition*: If the server used a naive check-then-add approach across two lock pairs (Lock -> Check -> Unlock -> [Context Switch] -> Lock -> Add -> Unlock), two threads could check the registry at the same time. Both would see the name is available, and both would proceed to add the duplicate name. By putting the iteration check and the array insertion inside the *same* continuous mutex lock, the operation is made **atomic**.

**Broadcast Implementation**:
In `broadcast_packet()`, the server locks `clients_mutex`, copies the socket file descriptors of all targets into a local array (`target_sockets`), and then *unlocks* the mutex before sending.
*The Problem Solved*: `send()` is a blocking network call. If the server held the mutex while iterating and sending to 100 clients, and one client had a terrible network connection, the TCP buffer would fill up, and `send()` would block. This would hold the global mutex hostage, completely freezing the entire server for everyone. Copy-then-release isolates network I/O from state management.

**File Transfer Routing**:
When the server receives a `FILE_START` header, it casts the payload to a `FileStartPayload` struct, reads the `target_username`, and searches the registry for that user's socket descriptor. It caches this target FD in `active_file_target_fd`. Subsequent `FILE_CHUNK` packets are blindly forwarded directly to that FD using `send_all()`, bypassing disk I/O entirely.

**Capacity & Graceful Degradation**:
The server caps connections at `MAX_CLIENTS` (100). If full, it sends a clear rejection message to the client and drops the connection gracefully. Payload sizes are strictly validated via `is_payload_size_valid()` (e.g., rejecting chat payloads over 4096 bytes) to prevent memory exhaustion (OOM) attacks from malicious clients.

---

## 4. Client Architecture (Client Role)

**Dual-Thread Design**:
The client is split into two concurrent execution paths:
1. **Main Thread**: Blocks on `fgets()`, waiting for the user to type something into the terminal.
2. **Receiver Thread**: Blocks on `recv_all()`, waiting for incoming messages from the server.
*Necessity*: If the client was single-threaded, it would be trapped. If it waited for user input, it couldn't receive incoming chats or files. If it waited for the network, the user couldn't type. The dual-thread architecture solves this inherent blocking I/O deadlock.

**Command Parsing**:
The client implements specific slash commands:
- `/msg <message>`: Formats text and sends as `CHAT`.
- `/file <username> <filepath>` (or `/sendfile`): Initiates the file transfer logic.
- `/quit` (or `/exit`): Sends `USER_LEAVE` and cleanly shuts down the client thread.
Any input not starting with a known slash command is treated as a standard chat broadcast.

**Username Handling Flow**:
Upon launch, the client prompts for a username, sends a `USER_JOIN` packet, and explicitly sets a socket timeout (`SO_RCVTIMEO`) to wait for a response. If the server rejects the name (duplicate/full), the server sends a `CHAT` packet containing "ERROR:". The client parses this, displays the rejection, and re-prompts the user in a `while(!registered)` loop.

---

## 5. File Transfer Mechanism (File Transfer + Docs Role)

**Full Walkthrough**:
1. **Sender**: The client opens the file via `fopen(..., "rb")` and uses `fseek/ftell` to calculate total size. It constructs `FileStartPayload` and sends it via `FILE_START`.
2. **Streaming Loop**: The sender reads up to 1024 bytes using `fread()` (`read_file_chunk()`). It constructs a `FILE_CHUNK` header where `length` is the *actual* bytes read. It sends the header, then the bytes. This loops until EOF.
3. **Completion**: Sender sends a 0-byte `FILE_END` packet and closes its file pointer.
4. **Receiver**: Upon seeing `FILE_START`, the receiver thread opens a local file using `fopen(..., "wb")`. As `FILE_CHUNK`s arrive, it writes them directly using `fwrite()`. Upon receiving `FILE_END`, it closes the local file pointer.

**Handling the Final Chunk**:
The last chunk is rarely exactly 1024 bytes. Because the sender sets `Header.length` to the return value of `fread()`, the receiver only writes exactly `Header.length` bytes to disk. Trusting the header length prevents padding the target file with garbage buffer bytes.

**Binary Safety**:
The code explicitly uses `"rb"` (read binary) and `"wb"` (write binary) in `fopen()`. If text mode (`"r"`/`"w"`) was used, underlying C libraries (especially on Windows) would automatically translate Unix `\n` characters (0x0A) into DOS `\r\n` (0x0D 0x0A). If transferring a compiled binary, a PNG, or a zip file, this translation corrupts the file.

**Progress Indicator**:
The client utilizes carriage returns (`\r`) in `printf()` alongside calculated percentages `(current_bytes * 100) / total_bytes` to overwrite the current line in the terminal, creating a smooth, non-spamming progress bar.

---

## 6. Reliability Layer (`send_all` / `recv_all`)

**The TCP Stream Problem**:
TCP is a byte-stream protocol, not a packet protocol. If you call `send(sock, buffer, 1024)`, the OS network stack might only send 500 bytes before returning due to buffer limits or network congestion. A naive programmer assumes 1024 bytes were sent. Similarly, `recv(sock, buffer, 1024)` might return 100 bytes. This causes catastrophic misalignment in fixed-header protocols.

**The Solution**:
The project implements `send_all()` and `recv_all()` in `utils.c`. These functions use `while(total < len)` loops, advancing the buffer pointer `ptr + total` and recalculating remaining bytes `len - total` on every iteration until the exact quota is met. 
*Confirmation*: A review of `server.c` and `client.c` confirms these wrappers are used consistently across all header and payload reads/writes, ensuring latent partial-read bugs are eradicated.

---

## 7. Error Handling & Edge Cases

The following error paths are explicitly handled in the codebase:
- **Local File Not Found**: (Client) `fopen` returns `NULL`. Client aborts with an error printout *before* sending `FILE_START` to the server.
- **Target User Not Connected**: (Server) `find_client_socket_by_username` returns `-1`. Server intercepts the `FILE_START`, resets its router state, and sends a specific `CHAT` error packet back to the sender notifying them.
- **Disconnect Mid-Transfer**: (Server) If sender drops, server detects `active_file_target_fd > 0`, synthesizes a `FILE_END` packet, and sends it to the receiver to release the lock. (Client) Receiver sees `FILE_END`, checks if `current_bytes < total_size`, and if so, safely deletes the corrupted file using `remove(filename)`.
- **Malformed/Oversized Message**: (Server & Client) `is_payload_size_valid` checks the header before allocating memory. The server drops malicious clients. The client discards the garbage payload if it is small enough to safely flush.
- **Registry Full**: (Server) `add_client` checks `client_count >= MAX_CLIENTS`. If full, it sends a rejection string and gracefully closes the socket.

---

## 8. Build & Run Instructions

**Dependencies**: Standard GCC compiler and POSIX-compliant OS (Linux/Unix/macOS/WSL).

**Build**:
The included `Makefile` automates the build process.
```bash
make
```
*(This compiles `utils.c` into an object file, then links it with `server.c` to create `server`, and `client.c` to create `client`.)*

**Run Server**:
```bash
./server
```
*(Listens on port 8080 by default).*

**Run Clients**:
Open additional terminals and run:
```bash
./client 127.0.0.1 8080 Alice
./client 127.0.0.1 8080 Bob
```

---

## 9. Demo Script

**Preparation**: Have three terminal windows open side-by-side. Ensure a file named `demo.txt` exists in the directory.

1. **Terminal 1**: Type `./server` and press Enter.
   *(Point out the robust start log)*.
2. **Terminal 2**: Type `./client 127.0.0.1 8080 Alice` and press Enter.
3. **Terminal 3**: Type `./client 127.0.0.1 8080 Bob` and press Enter.
   *(Point out Terminal 2 seeing Bob join)*.
4. **Terminal 3**: Type `/msg Hello Alice, ready for the file?` and press Enter.
   *(Point out instant delivery to Terminal 2)*.
5. **Terminal 2**: Type `./client 127.0.0.1 8080 Bob` and press Enter.
   *(This intentionally attempts a duplicate username. Point out the "Username already taken" rejection. Hit Ctrl+C to close this terminal)*.
6. **Terminal 2 (Alice)**: Type `/file Bob demo.txt` and press Enter.
   *(Point out the instant progress bar completing in both terminals, and prove `demo.txt` arrived intact by typing `cat demo.txt` in a fresh terminal)*.
7. **Terminal 3 (Bob)**: Type `/quit` and press Enter.
   *(Point out Alice and Server gracefully acknowledging the disconnect)*.

---

## 10. Viva Preparation — Anticipated Questions

### Server & Protocol Role
1. **Q**: Why did you use a fixed 8-byte header with a length integer instead of just separating messages with `\n`?
   **A**: Delimiters require scanning every incoming byte, which is slow. More importantly, binary file data naturally contains random bytes. A `\n` byte could naturally occur inside a JPEG file chunk, which would break the protocol. Fixed length allows precise, bulk `recv_all` calls.
2. **Q**: What would happen if you removed `clients_mutex` from `add_client`?
   **A**: We would suffer a race condition. If two clients join at the exact same microsecond with the name "Admin", both threads might execute the `strcmp` check simultaneously, see the name is free, and both register it, corrupting the routing logic.
3. **Q**: Walk me through what happens when you broadcast a chat message in your code.
   **A**: The worker thread receives the message. It locks the mutex, copies all valid socket FDs (except the sender's) into a local array, and unlocks the mutex. It then iterates over the local array calling `send_all`. This prevents a slow network connection from holding the mutex and freezing the server.

### Client Architecture Role
4. **Q**: Why does the client require two threads (`pthread_create`), while simpler chat apps might just use one?
   **A**: Single threads block on I/O. If I use `fgets()` to wait for user keyboard input, the client cannot receive network messages until the user presses Enter. Two threads allow simultaneous blocking on `stdin` and the TCP socket.
5. **Q**: What would break if you used standard `recv()` instead of the `recv_all()` wrapper you implemented?
   **A**: TCP doesn't guarantee packet preservation. A `recv()` for 1024 bytes might only return 500 bytes. Our protocol expects exact length parsing. If it reads 500 bytes, the next loop iteration will misinterpret the remaining 524 bytes of payload as a new protocol Header, causing catastrophic misalignment.
6. **Q**: Walk me through what happens when a user types a duplicate username.
   **A**: The client sends `USER_JOIN`. It sets a socket read timeout and waits. The server rejects it and sends a `CHAT` packet starting with "ERROR:". The client parses this text, prints the rejection, resets the username variable, and stays in the registration `while` loop, asking the user to type again.

### File Transfer & Docs Role
7. **Q**: Why did you design the server to route file chunks in memory instead of saving the file to disk on the server and having the receiver download it?
   **A**: Direct in-memory routing is faster, requires zero disk I/O on the server, and prevents the server from filling up its hard drive if users transfer massive 10GB files. It preserves server resources.
8. **Q**: What would break if you used `fopen("file", "w")` instead of `"wb"`?
   **A**: Text mode file operations translate line endings. On Windows, writing a `\n` byte (0x0A) automatically converts it to `\r\n` (0x0D 0x0A). If we transferred a compiled binary executable, this automatic translation would permanently corrupt the binary structure.
9. **Q**: Walk me through what happens step-by-step when a sender forcefully closes their terminal (disconnects) when a file transfer is only 50% complete.
   **A**: The sender socket closes. The server `recv_all` fails, returning -1. The server thread notices `active_file_target_fd > 0`, meaning a transfer was interrupted. It synthesizes a `FILE_END` header and pushes it to the receiver. The receiver gets `FILE_END`, sees `current_bytes < total_size`, realizes the transfer is corrupted, closes the file, calls `remove(filename)` to delete the partial file, and prints a warning.

---

## 11. Known Limitations

1. **Concurrent Inbound File Transfers**: The client codebase uses a global state (`active_recv_fp`, `active_recv_filename`) for incoming files. If a client is currently receiving a file from Alice, and Bob attempts to send a file to the same client concurrently, the global state will overwrite, corrupting both transfers.
2. **Fixed Cap Limits**: Hardcoded limits restrict the system to `100` concurrent clients (`MAX_CLIENTS`), `32`-character usernames (`MAX_USERNAME`), and `1024`-byte file chunks.
3. **No Encryption**: Data is transmitted via plaintext over standard TCP sockets. It is susceptible to packet sniffing (e.g., via Wireshark) on the local network. 
4. **Synchronous Server Routing**: While broadcasting is non-blocking to the server thread pool, a slow receiver pulling a file chunk will throttle the sender, as the server thread blocks on `send_all` to the slow target before it can read the next chunk from the sender.
