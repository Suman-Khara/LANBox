---

# 📘 LANBox

A **local-network encrypted file-sharing & syncing tool** written in C++.

This is the CLI version supporting **cross-platform** (Windows/Linux/macOS).

---

## 🚀 Build Instructions

### Prerequisites

* CMake ≥ 3.10
* C++17 compatible compiler:

  * **Windows** → MSVC or MinGW
  * **Linux/macOS** → GCC or Clang

### Steps

```bash
# Clone repository
git clone https://github.com/Suman-Khara/LANBox
cd lanbox

# Create build directory
mkdir build && cd build

# Configure project with CMake
cmake ..

# Build the executable
cmake --build .
```

After build, you’ll have an executable:

* On Windows → `lanbox.exe` (in `build/`)
* On Linux/macOS → `lanbox` (in `build/`)

---

## ▶️ Usage

### 1. Start Receiver (Server Mode)

Run on the machine that will **receive files**:

```bash
lanbox server
```

* This will open a socket on **port 5000** and wait for incoming files.
* By default, received files are saved in the current directory.

### 2. Send a File (Client Mode)

Run on the machine that will **send a file**:

```bash
lanbox send <server_ip> <filename>
```

Example:

```bash
lanbox send 192.168.1.42 video.mp4
```

* `192.168.1.42` → the LAN IP of the receiver
* `video.mp4` → the file to send

---