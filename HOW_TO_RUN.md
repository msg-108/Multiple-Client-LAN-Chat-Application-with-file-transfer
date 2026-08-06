# How to Run the LAN Chat & File Transfer Application

This guide provides step-by-step instructions to compile and run the **Multiple-Client LAN Chat Application with File Transfer** on **macOS**, **Windows**, and **Linux**.

---

## 📋 Table of Contents

1. [Prerequisites](#-prerequisites)
   - [macOS](#1-macos)
   - [Windows](#2-windows)
   - [Linux](#3-linux)
2. [Compilation Instructions](#%EF%B8%8F-compilation-instructions)
   - [Method A: Using `make` (Recommended)](#method-a-using-make-recommended)
   - [Method B: Using `gcc` or `clang` Directly](#method-b-using-gcc-or-clang-directly)
3. [Running the Application](#-running-the-application)
   - [Step 1: Start the Server](#step-1-start-the-server)
   - [Step 2: Connect Clients](#step-2-connect-clients)
4. [Finding Your LAN IP Address](#-finding-your-lan-ip-address)
5. [Interactive Commands & Controls](#-interactive-commands--controls)
6. [Complete Worked Example](#-complete-worked-example-alice--bob)
7. [Troubleshooting & Firewall Settings](#-troubleshooting--firewall-settings)

---

## 🛠️ Prerequisites

### 1. macOS

On macOS, install the free **Xcode Command Line Tools** which include `clang`, `gcc`, and `make`:

Open **Terminal** and run:
```bash
xcode-select --install
```
*Follow the on-screen prompt to complete the installation.*

### 2. Windows

Choose **one** of the following environment options:

#### Option A: WSL (Windows Subsystem for Linux) — *Recommended*
1. Open PowerShell as Administrator and run:
   ```powershell
   wsl --install
   ```
2. Restart your PC if prompted, open the **Ubuntu** terminal app, and install build tools:
   ```bash
   sudo apt update && sudo apt install -y build-essential
   ```

#### Option B: MinGW-w64 / MSYS2 / Git Bash (Native Windows `.exe`)
1. Install [MSYS2](https://www.msys2.org/) or [MinGW-w64](https://www.mingw-w64.org/).
2. In the MSYS2 MinGW 64-bit terminal, install `gcc` and `make`:
   ```bash
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make
   ```
3. Add `C:\msys64\mingw64\bin` to your Windows System `PATH`.

#### Option C: Visual Studio Developer Command Prompt (MSVC)
Open **Developer Command Prompt for Visual Studio** and compile using `cl.exe`.

### 3. Linux (Ubuntu / Debian / Fedora / Arch)

On Debian/Ubuntu:
```bash
sudo apt update && sudo apt install -y build-essential
```
On Fedora/RHEL:
```bash
sudo dnf groupinstall "Development Tools"
```

---

## ⚙️ Compilation Instructions

Navigate to the project root directory in your terminal:

```bash
cd /path/to/Multiple-Client-LAN-Chat-Application-with-file-transfer
```

### Method A: Using `make` (Recommended)

Run `make` to compile all targets (`server`, `client`, `test_utils`):

```bash
make
```

- **macOS / Linux output**: `server`, `client`, `test_utils`
- **Windows output**: `server.exe`, `client.exe`, `test_utils.exe`

To clean up compiled binaries and object files:
```bash
make clean
```

### Method B: Using `gcc` or `clang` Directly

#### macOS & Linux:
```bash
# 1. Compile Server
gcc -Wall -Wextra -std=c11 -pthread server.c utils.c -o server

# 2. Compile Client
gcc -Wall -Wextra -std=c11 -pthread client.c utils.c -o client

# 3. Compile Unit Test Utility
gcc -Wall -Wextra -std=c11 -pthread -DTEST_UTILS utils.c -o test_utils
```
*(On macOS, `clang` can be substituted for `gcc` if desired)*

#### Windows (MinGW / GCC):
```bash
gcc -Wall -Wextra -std=c11 -pthread server.c utils.c -o server.exe -lws2_32
gcc -Wall -Wextra -std=c11 -pthread client.c utils.c -o client.exe -lws2_32
```

---

## 🚀 Running the Application

### Step 1: Start the Server

Launch the server process first. By default, it listens on port **`8080`**.

#### macOS / Linux:
```bash
./server
```

#### Windows:
```cmd
server.exe
```

*Server console output:*
```text
[Step 1] Socket created successfully (FD: 3)
[Step 2] Bound successfully to port 8080
[Step 3] Server listening with backlog queue of 10

=====================================================
  Robust Structured TCP Server on port 8080...
  Features: Fault Isolation, Size Check, Mid-Transfer Cleanup
=====================================================
```

---

### Step 2: Connect Clients

Open separate terminal windows or tabs for each client.

#### 1. Interactive Connection (Localhost `127.0.0.1:8080`)
```bash
./client
```
*Prompt:* `Enter your username (max 31 chars): `

#### 2. Connect via Custom Host IP and Port (LAN Mode)
```bash
./client <SERVER_IP> 8080
```
*Example:* `./client 192.168.1.50 8080`

#### 3. Direct Command-Line Arguments
```bash
./client <SERVER_IP> <PORT> <USERNAME>
```
*Example:*
```bash
./client 127.0.0.1 8080 Alice
```

---

## 🌐 Finding Your LAN IP Address

To run clients across different computers on the same Local Area Network (LAN):

### macOS
Open Terminal and run:
```bash
ipconfig getifaddr en0
```
*(Or check Wi-Fi details in System Settings -> Network)*

### Windows
Open Command Prompt / PowerShell and run:
```cmd
ipconfig
```
Look for **IPv4 Address** under your active Wi-Fi or Ethernet adapter (e.g., `192.168.1.105`).

### Linux
Open Terminal and run:
```bash
hostname -I
```

---

## 💬 Interactive Commands & Controls

Once logged into the client CLI, the following commands are available:

| Command Syntax | Action | Description |
| :--- | :--- | :--- |
| `/msg <text>` | Chat Message | Broadcasts a text message to all connected clients. |
| `<text>` | Plain Text | Typing plain text without `/` also broadcasts as a chat message. |
| `/file <target_user> <filepath>` | File Transfer | Initiates direct binary file transfer to `<target_user>`. |
| `/quit` or `/exit` | Graceful Exit | Disconnects cleanly from server and terminates client. |

---

## 🧪 Complete Worked Example (Alice & Bob)

Here is a full demonstration sequence sending a document from `Alice` to `Bob`:

### 1. Terminal 1 — Server (macOS / Windows / Linux)
```bash
./server
```

### 2. Terminal 2 — Receiver Client (`Bob`)
```bash
./client 127.0.0.1 8080 Bob
```
*Output:*
```text
Connecting to server at 127.0.0.1:8080...
Connected to server at 127.0.0.1:8080!
[+] Registered successfully as 'Bob'!
```

### 3. Terminal 3 — Sender Client (`Alice`)
```bash
./client 127.0.0.1 8080 Alice
```
*Output:*
```text
Connecting to server at 127.0.0.1:8080...
Connected to server at 127.0.0.1:8080!
[+] Registered successfully as 'Alice'!
```

### 4. Transfer File (`Alice`'s Terminal)
Alice types:
```text
/file Bob document.pdf
```

**Alice's Screen:**
```text
[CLIENT] Initiating file transfer: 'Alice' -> 'Bob' (File: document.pdf, 2048000 bytes)...
[SEND PROGRESS] Sending 'document.pdf': 100% (2048000/2048000 bytes)
[CLIENT] File transfer complete: 'document.pdf' (2048000 bytes) sent successfully to 'Bob'!
```

**Bob's Screen:**
```text
Receiving document.pdf from Alice (2048000 bytes)...
[RECV PROGRESS] Receiving 'document.pdf': 100% (2048000/2048000 bytes)
File transfer complete for 'document.pdf' from 'Alice' (2048000 bytes)!
```

---

## 🛡️ Troubleshooting & Firewall Settings

### macOS Firewall Prompts
When running `./server` for the first time, macOS may display a popup:
> *"Do you want the application server to accept incoming network connections?"*

Click **Allow**. If blocked, go to **System Settings -> Privacy & Security -> Firewall** and allow `./server`.

### Windows Defender Firewall Prompts
When starting `server.exe`, Windows Defender Firewall will prompt to allow access on Private / Public networks. Ensure **Private networks** is checked and click **Allow access**.

### Common Error Solutions

| Issue | Cause | Solution |
| :--- | :--- | :--- |
| `Bind failed: Address already in use` | Port 8080 is currently occupied. | Wait a few seconds for TCP socket cleanup, or kill any existing server process (`killall server` / `taskkill /F /IM server.exe`). |
| `Connection refused` | Server is not running or listening on a different port. | Ensure `./server` is started before running `./client`. |
| `Username already taken` | Another client registered with the same name. | Relaunch client with a unique username (e.g. `Alice_2`). |
| `Target file not found` | The specified file path does not exist. | Check spelling and verify the file is in your current working directory. |
