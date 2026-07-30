# Test Plan & Test Log

This document presents the Test Plan and Test Results Log for the **Multiple-Client LAN Chat Application with File Transfer**. It outlines test scenarios covering message broadcasting, user registration, binary file transfer across small, large, and empty files, error handling, and robust fault recovery.

---

## 1. Test Overview & Objectives

The goal of testing is to verify that:
1. **Core Communication**: Messages are accurately routed and formatted as `[username]: message`.
2. **Multi-Client Broadcast**: Broadcasts deliver to all connected clients except the sender without thread blocking.
3. **Atomic Registration**: Username collisions are detected and rejected under `clients_mutex` lock.
4. **Binary File Integrity**: Binary files (small, 8MB large, and 0-byte empty) are transferred 100% byte-for-byte identical.
5. **Partial File Safety**: Disconnections mid-transfer cause partial files to be cleanly removed (`remove()`) on the receiving end.
6. **Command Parsing & Robustness**: Invalid commands, missing files, offline target users, and server disconnects do not crash the client or server.

---

## 2. Test Plan Matrix

| Test ID | Category | Target Component | Test Description |
| :---: | :--- | :--- | :--- |
| **TC-01** | Basic Chat | Client / Server | Send single chat message using `/msg` or plain text. |
| **TC-02** | Multi-Client Broadcast | Server Broadcast | Broadcast message to multiple connected clients simultaneously. |
| **TC-03** | Username Rejection | Mutex Registry | Attempt to register with a username already in use. |
| **TC-04** | Small File Transfer | Binary File Router | Transfer a small text file (< 1 KB) between two clients. |
| **TC-05** | Large File Transfer | Stream Routing & Progress | Transfer a large binary file (8 MB) with in-place (`\r`) progress updates. |
| **TC-06** | Empty File Transfer | Boundary Case | Transfer an empty 0-byte file (`empty.bin`). |
| **TC-07** | Disconnect Mid-Transfer | Error Handling / Safety | Abort sender process mid-transfer; verify partial file cleanup on receiver. |
| **TC-08** | Target User Offline | Error Handling | Attempt to send a file to a non-existent / offline target user. |
| **TC-09** | Local File Missing | Error Handling | Attempt to send a local file that does not exist on disk. |
| **TC-10** | Invalid Commands | Client Command Parser | Input unrecognized slash commands or incomplete argument parameters. |

---

## 3. Test Results Log

### TC-01: Basic Chat Messaging
- **Inputs**: `Alice` types `/msg Hello Server & Clients!`
- **Expected Result**: Server receives packet; broadcasts `[Alice]: Hello Server & Clients!` to other connected clients.
- **Actual Result**: `[Alice (127.0.0.1:54321)]: Hello Server & Clients!` logged on server and displayed on target clients.
- **Status**: **PASS**

---

### TC-02: Multi-Client Broadcast
- **Inputs**: 3 Clients connected (`Alice`, `Bob`, `Charlie`). `Bob` sends `Welcome team`.
- **Expected Result**: `Alice` and `Charlie` receive `[Bob]: Welcome team`. `Bob` does not receive an echo.
- **Actual Result**: Both `Alice` and `Charlie` terminals display `[Bob]: Welcome team` instantly.
- **Status**: **PASS**

---

### TC-03: Duplicate Username Rejection
- **Inputs**: `Client 1` connects as `Alice`. `Client 2` connects as `Alice`.
- **Expected Result**: Server rejects `Client 2` under `clients_mutex` with `ERROR: Username 'Alice' is already taken. Connection rejected.`. `Client 2` prompts user to enter a different name.
- **Actual Result**: `Client 2` outputs `[REJECTED] ERROR: Username 'Alice' is already taken.` and prompts `Enter your username (max 31 chars): `.
- **Status**: **PASS**

---

### TC-04: Small File Transfer (< 1 KB)
- **Inputs**: `Alice` sends `/file Bob small.txt` (512 bytes).
- **Expected Result**: `Bob` receives `Receiving small.txt from Alice (512 bytes)...` and saves `small.txt`. File binary comparison (`cmp`) passes 100%.
- **Actual Result**: File transferred in 1 chunk; `cmp` confirms 0 byte differences.
- **Status**: **PASS**

---

### TC-05: Large File Transfer (8 MB Binary File)
- **Inputs**: `Alice` sends `/file Bob large_8mb.bin` (8,388,608 bytes generated via `/dev/urandom`).
- **Expected Result**: Streams via 8192 `FILE_CHUNK` packets (1024 bytes each). In-place progress bar updates from 0% to 100%. `cmp` confirms byte-for-byte binary match.
- **Actual Result**: Progress updated smoothly: `[SEND PROGRESS] Sending 'large_8mb.bin': 100% (8388608/8388608 bytes)`. Binary hash comparison matched 100%.
- **Status**: **PASS**

---

### TC-06: Empty File Transfer (0 Bytes)
- **Inputs**: `Alice` sends `/file Bob empty.bin` (0 bytes).
- **Expected Result**: Sends `FILE_START` (0 bytes) followed by `FILE_END`. `Bob` creates `empty.bin` cleanly.
- **Actual Result**: `empty.bin` created on receiver with 0 bytes size. No crashes or read hangs.
- **Status**: **PASS**

---

### TC-07: Disconnect Mid-Transfer & Partial File Cleanup
- **Inputs**: `Alice` starts transferring `large_8mb.bin` to `Bob` and is terminated (`kill -9`) at 40% completion.
- **Expected Result**: Server detects disconnect, sends cleanup signal to `Bob`. `Bob` closes file pointer, deletes incomplete file using `remove()`, and logs error notice.
- **Actual Result**: `Bob` terminal printed `[CLIENT ERROR] File transfer of 'large_8mb.bin' from 'Alice' was interrupted prematurely (33554432/8388608 bytes received). Incomplete file removed.` Partial file was verified deleted from disk.
- **Status**: **PASS**

---

### TC-08: Target User Offline
- **Inputs**: `Alice` types `/file NonExistentUser test.bin`.
- **Expected Result**: Server returns `ERROR: Target user 'NonExistentUser' is not connected. File transfer canceled.`. Sender prints notice and does not crash.
- **Actual Result**: `Alice` received `ERROR: Target user 'NonExistentUser' is not connected.` and returned to command prompt safely.
- **Status**: **PASS**

---

### TC-09: Missing Local File
- **Inputs**: `Alice` types `/file Bob non_existent_file.xyz`.
- **Expected Result**: Client checks `fopen("non_existent_file.xyz", "rb")`, prints error, and aborts before sending `FILE_START`.
- **Actual Result**: `[CLIENT ERROR] Target file not found or permission denied` displayed. No packets sent to server.
- **Status**: **PASS**

---

### TC-10: Invalid Command Parsing
- **Inputs**: User types `/unknown_command`, `/msg`, or `/file` without arguments.
- **Expected Result**: Client catches invalid syntax, displays usage instructions, and does not send garbage packets to server.
- **Actual Result**: Printed `[CLIENT ERROR] Invalid command input` followed by usage guide. Client stayed connected.
- **Status**: **PASS**

---

## 4. Summary & Verification Matrix

- **Total Test Cases**: 10
- **Passed**: 10
- **Failed**: 0
- **Test Completion Status**: **100% PASS**
