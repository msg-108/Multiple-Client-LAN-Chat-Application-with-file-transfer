# Multiple-Client LAN Chat Application with File Transfer

A multithreaded TCP client-server chat application written in standard C (C11, POSIX sockets, `pthreads`) featuring structured protocol headers, atomic duplicate username enforcement, non-blocking broadcasting, direct file transfer routing, text-based progress indicators, and fault isolation.

---

## Features

- **Structured Binary Protocol**: Uses fixed 8-byte `Header` (`type`, `length`) followed by explicit `length` bytes of payload.
- **Reliable Data Transfer**: All socket I/O is wrapped in `send_all()` and `recv_all()` to guarantee exact byte transfer across TCP streams.
- **Multithreaded Server**: Spawns dedicated POSIX worker threads per client (`pthread_create`, `pthread_detach`) to support up to 100 concurrent clients.
- **Atomic Username Enforcement**: Checks duplicate usernames under `clients_mutex` lock before registration to prevent race conditions.
- **Non-Blocking Broadcasting**: Local socket array snapshotting under mutex lock so slow/dead clients do not block other active connections.
- **File Transfer Routing**: Server routes `FILE_START`, `FILE_CHUNK`, and `FILE_END` packets directly from sender to target client without storing files on disk.
- **Binary File Safety**: File chunking operates strictly in binary mode (`"rb"` / `"wb"`) using `read_file_chunk()` and `write_file_chunk()` without string manipulation functions.
- **Progress Indicators**: Displays in-place (`\r`) percentage and byte count progress updates during file upload and download.
- **Robust Error Handling**:
  - Handles target file not found before sending `FILE_START`.
  - Rejects oversized or negative packet headers without allocating memory.
  - Automatically deletes corrupted partial files on the receiver end if a transfer is interrupted mid-stream.
  - Fault isolation (`SIGPIPE` ignored): One client's disconnect or failure never crashes the server.

---

## Project Structure

```text
.
├── protocol.h      # Protocol Header struct, message type enums, and FileStartPayload definition
├── utils.h         # Function declarations for binary file chunks, send_all, and recv_all
├── utils.c         # Implementations of read_file_chunk, write_file_chunk, send_all, and recv_all
├── server.c        # Multithreaded TCP chat server & binary file transfer router
├── client.c        # Multithreaded TCP chat client with interactive command parser & progress bar
├── Makefile        # Build instructions for server, client, and test_utils
├── README.md       # Project documentation, build/run instructions, and worked examples
└── TESTING.md      # Comprehensive Test Plan, Test Matrix, Phase 8 Report, and Demo Sequence
```

---

## Build Instructions

### Option 1: Using `gcc` Directly (Phase 8 Specification)

```bash
# 1. Compile and link the Server
gcc server.c utils.c -o server -lpthread

# 2. Compile and link the Client
gcc client.c utils.c -o client -lpthread
```

### Option 2: Using `make` (Recommended)

Run `make` in the terminal to compile all targets (`server`, `client`, `test_utils`):

```bash
make
```

To clean up compiled binaries and object files:

```bash
make clean
```

---

## Run Instructions

### Step 1: Start the Server First

Run the server executable on the host machine. By default, it listens on port `8080`:

```bash
./server
```

### Step 2: Connect Clients

You can start any number of client instances in separate terminal windows.

#### Usage Options:

1. **Interactive Mode (Defaults to `127.0.0.1:8080`)**:
   ```bash
   ./client
   ```
   *Prompt: `Enter your username (max 31 chars): `*

2. **Custom Host & Port**:
   ```bash
   ./client 192.168.1.50 8080
   ```

3. **Full Command-Line Mode**:
   ```bash
   ./client 127.0.0.1 8080 Alice
   ```

---

## Available Client Commands

Inside the client interface, you can use the following commands:

| Command | Action | Description |
| :--- | :--- | :--- |
| `/msg <message>` | Chat Message | Broadcasts a chat message to all connected clients. Plain text also sends as chat. |
| `/file <username> <filename>` | Send File | Initiates a direct binary file transfer to `<username>`. |
| `/quit` or `/exit` | Disconnect | Gracefully disconnects from the server and exits. |

---

## File Transfer Instructions & Worked Example

### Worked Example: `Alice` sends `sample.pdf` to `Bob`

1. **Start Server**:
   ```bash
   ./server
   ```

2. **Connect Receiver Client (`Bob`)**:
   ```bash
   ./client 127.0.0.1 8080 Bob
   ```
   *Output*:
   ```text
   Connected to server at 127.0.0.1:8080!
   [+] Registered successfully as 'Bob'!
   ```

3. **Connect Sender Client (`Alice`)**:
   ```bash
   ./client 127.0.0.1 8080 Alice
   ```
   *Output*:
   ```text
   Connected to server at 127.0.0.1:8080!
   [+] Registered successfully as 'Alice'!
   ```

4. **Initiate File Transfer (`Alice`'s terminal)**:
   ```bash
   /file Bob sample.pdf
   ```

   **Alice's Terminal**:
   ```text
   [CLIENT] Initiating file transfer: 'Alice' -> 'Bob' (File: sample.pdf, 1048576 bytes)...
   [SEND PROGRESS] Sending 'sample.pdf': 100% (1048576/1048576 bytes)
   [CLIENT] File transfer complete: 'sample.pdf' (1048576 bytes) sent successfully to 'Bob'!
   ```

   **Bob's Terminal**:
   ```text
   Receiving sample.pdf from Alice (1048576 bytes)...
   [RECV PROGRESS] Receiving 'sample.pdf': 100% (1048576/1048576 bytes)
   File transfer complete for 'sample.pdf' from 'Alice' (1048576 bytes)!
   ```

---

## Debugging & Architectural Reference

This section details critical network architectural decisions and root-cause fixes implemented in the codebase:

### 1. Partial or Cut-Off Messages
- **Root Cause**: Assuming a single `recv()` call returns all requested payload bytes. TCP streams do not preserve packet boundaries.
- **Implementation Fix**: All socket reads call `recv_all(sock, buf, len)` (`utils.c`), which loops until all `len` bytes are received or returns `-1` immediately on disconnection (`n <= 0`).

### 2. Server Crash on Client Disconnect
- **Root Cause**: Closing a disconnected socket without updating the global client registry under mutex lock causes subsequent broadcast calls to `send()` to a dead socket descriptor.
- **Implementation Fix**: `remove_client(client_fd)` removes the entry from the global `clients[]` list under `clients_mutex` lock before socket closure and thread termination.

### 3. File Corruption Safeguards
- **Root Cause**: Opening files in text mode (`"r"`/`"w"`), applying string functions (`strcpy`, `strlen`, `printf("%s")`) to raw binary chunk buffers, or assuming `CHUNK_SIZE` instead of trusting `Header.length`.
- **Implementation Fix**: All file I/O uses binary mode (`"rb"` / `"wb"`), raw memory operations (`write_file_chunk`), and trusts `Header.length` for every chunk without string function calls.

### 4. Duplicate Username Race Conditions
- **Root Cause**: Performing the username existence check and adding the new client in separate mutex-locked blocks creates race conditions where two clients registering simultaneously with identical names can both pass.
- **Implementation Fix**: `add_client()` performs both the duplicate check iteration and slot assignment inside the **same** `clients_mutex` critical section.

### 5. Server Freezing / Deadlocks Under Load
- **Root Cause**: Holding `clients_mutex` while executing blocking network `send()` calls during message broadcasting.
- **Implementation Fix**: `broadcast_packet()` copies destination socket descriptors into a local snapshot array under `clients_mutex`, releases `clients_mutex`, and executes `send_all()` outside the lock.

---

## Integration, Stress Testing & Verification

For full documentation, edge case results, stress test logs, and live demo presentation scripts, consult [TESTING.md](file:///wsl.localhost/Ubuntu/home/loq/Multiple-Client-LAN-Chat-Application-with-file-transfer/TESTING.md).

### Quick Unit Test Verification

To execute the unit test suite for binary file chunking:

```bash
./test_utils
```

### Test Suite Summary (`TESTING.md`)

| Test ID | Test Category | Description | Status |
| :---: | :--- | :--- | :---: |
| **TC-01** | Basic Chat | `/msg` chat message delivery | **PASS** |
| **TC-02** | Multi-Client Broadcast | Broadcast to multiple clients without blocking | **PASS** |
| **TC-03** | Username Rejection | Duplicate username rejection under mutex lock | **PASS** |
| **TC-04** | Small File Transfer | Text/binary transfer (< 1 KB) | **PASS** |
| **TC-05** | Large File Transfer | 8 MB binary file transfer with progress indicator | **PASS** |
| **TC-06** | Empty File Transfer | Boundary case (0-byte file) | **PASS** |
| **TC-07** | Disconnect Mid-Transfer | Mid-stream disconnect cleanup & partial file removal | **PASS** |
| **TC-08** | Target User Offline | File transfer attempt to offline target user | **PASS** |
| **TC-09** | Missing Local File | File transfer attempt for missing file path | **PASS** |
| **TC-10** | Invalid Commands | Command syntax parsing & malformed header protection | **PASS** |
| **TC-11** | Concurrency Stress Test | 5 concurrent clients sending rapid messages simultaneously | **PASS** |

---

## Known Limitations

1. **Maximum Connected Clients**: Limited to `100` (`MAX_CLIENTS`) concurrent connections. Rejections are sent gracefully if full.
2. **Maximum Username Length**: Usernames are constrained to `32` characters (`MAX_USERNAME`).
3. **Maximum Chunk Size**: File chunk payloads are transmitted in units up to `1024` bytes (`CHUNK_SIZE`).
4. **Online Routing Only**: The server operates strictly as an in-memory stream router. It does not store files on disk, so file transfers require the target user to be online.
