## Slide 1: Title

- **Project:** Multiple-Client LAN Chat Application with File Transfer
- **Course:** Network Programming
- **Team:**
  - Madhusudhan Gharti – Server & Protocol
  - Aman Joshi – Client
  - Siddhanta Chhetri – File Transfer & Docs

---

## Slide 2: Architecture Overview

- Central server coordinates multiple TCP client connections.
- Server broadcasts chat messages to all other clients.
- File transfers use point-to-point routing directly to recipients.

**Diagram Suggestion for Canva:**
A central "Server" box in the middle. Three "Client" boxes surrounding it with bi-directional arrows connecting each to the Server. One arrow is labeled "Broadcast (Chat)" pointing to multiple clients. Another arrow is labeled "Point-to-Point (File)" going from one client, through the server, directly to another specific client.

---

## Slide 3: Server & Protocol

- Protocol uses a fixed 8-byte Header with payload length.
- Atomic duplicate-username checks occur inside a single mutex lock.
- Broadcasts use a copy-then-release pattern to prevent locking.

**Speaker Notes:**
We designed a fixed-header protocol rather than using delimiters because scanning bytes is slow and binary files naturally contain characters like newlines. To prevent race conditions, the server checks for duplicate usernames and registers new ones under a single, continuous mutex lock. For broadcasting, the server copies target sockets locally and releases the mutex before sending, ensuring a slow client's network connection cannot freeze the entire server.

---

## Slide 4: Client Architecture

- Clients use a dual-thread design (input and receive threads).
- Separate threads prevent blocking I/O from causing deadlocks.
- Ensures simultaneous keyboard typing and network message receiving.

**Speaker Notes:**
If the client operated on a single thread, it would deadlock. It would be stuck waiting for user keyboard input on `stdin` and unable to receive network messages, or vice versa. By utilizing two POSIX threads, one handles blocking terminal input while the other continuously listens on the TCP socket, allowing seamless real-time interaction.

---

## Slide 5: File Transfer

- Chunked binary transfer strictly trusts Header length per chunk.
- Explicit binary-mode file I/O (`"rb"`/`"wb"`) prevents corruption.
- Disconnections mid-transfer trigger partial file cleanup via `remove()`.

**Speaker Notes:**
Files are sent in up to 1024-byte chunks. The receiver only writes the exact payload length specified by the Header, preventing garbage padding on the final chunk. We explicitly open files in binary mode to stop OS-level newline translations from destroying compiled binaries or images. If a sender disconnects prematurely, the server synthesizes a file end signal, prompting the receiver to safely delete the corrupted partial file.

---

## Slide 6: Live Demo

- Multi-client chat broadcasting.
- Duplicate username rejection (interactive prompt without crashing).
- Direct file transfer with 100% integrity verification (`cmp`).

---

## Slide 7: Known Limitations

- **Max Client Cap:** Limited to 100 concurrent connections.
- **Username Length:** Constrained to maximum 32 characters.
- **Chunk Size:** Payloads transmitted in up to 1024 bytes.
- **No Encryption:** Plaintext TCP sockets are susceptible to packet sniffing.
- **Online Routing:** Target users must be online for file transfers.

---

## Slide 8: Closing

- Thank You!
- Questions?
- **Team:**
  - Madhusudhan Gharti – Server & Protocol
  - Aman Joshi – Client
  - Siddhanta Chhetri – File Transfer & Docs
