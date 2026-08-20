# uring_perf - High-Performance io_uring Zero-Copy Traffic Generator

`uring_perf` is an `iperf3`-inspired network benchmark and traffic generator built from the ground up for modern Linux kernels using `io_uring`, per-thread lockless submission queues, zero-copy primitives (`IORING_OP_SEND_ZC`), and registered buffer memory page pre-pinning.

---

## Key Features

1. **Lockless Shared-Nothing Architecture**: Each parallel worker thread owns an isolated `io_uring` ring, completely eliminating mutex lock contention and cross-core cache invalidation.
2. **Zero-Copy Network Sends (`IORING_OP_SEND_ZC`)**: Network payload transmission bypasses kernel memory copying by pinning user-space buffer pages directly for NIC DMA transmission.
3. **Fixed Buffer & File Descriptor Pre-Registration**:
   - `IORING_REGISTER_BUFFERS`: Pre-pins memory pages (`posix_memalign`) into the kernel page table.
   - `IORING_REGISTER_FILES`: Avoids kernel file table lookups per submission.
4. **Kernel SQ Polling (`IORING_SETUP_SQPOLL`)**: Offloads submission queue polling to dedicated kernel threads, enabling zero-syscall network transmission.
5. **Multi-Thread Listening (`SO_REUSEPORT`)**: Allows server worker threads to listen on the same TCP port with kernel flow hashing.
6. **Real-time Live Metrics**: Reports interval transfer rate (Gbits/sec), packet throughput (Kpps/Mpps), zero-copy completion counts, and error tracking.

---

## Build Instructions

### Prerequisites
- Linux Kernel 6.0+ (Tested on Linux Kernel 6.18+ / WSL2)
- `g++` (C++17 support) or `clang++`
- `liburing-dev` (version 2.2+)
- `cmake` (version 3.14+) or `make`

### Building with Makefile
```bash
cd scratch/uring_perf
make -j$(nproc)
```

### Building with CMake
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## Usage Examples

### 1. Basic Server Mode
Listen on default port 5201:
```bash
./uring_perf -s
```

### 2. Standard Client Test
Send traffic to server for 10 seconds using 2 parallel threads:
```bash
./uring_perf -c 127.0.0.1 -P 2 -t 10
```

### 3. Maximum Throughput Zero-Copy Test (`-Z`)
Enable `IORING_OP_SEND_ZC` zero-copy transmission with 4 parallel threads and 128KB payload size:
```bash
./uring_perf -c 127.0.0.1 -P 4 -l 131072 -Z -t 10
```

### 4. Zero-Syscall Kernel SQPOLL Mode (`-Z -S`)
Enable zero-copy sending combined with Kernel Submission Queue Polling (`IORING_SETUP_SQPOLL`):
```bash
./uring_perf -c 127.0.0.1 -P 2 -Z -S -t 10
```

### 5. Core Pinning (`-A`)
Pin worker threads 0, 1, 2, 3 to CPU cores 0, 1, 2, 3:
```bash
./uring_perf -c 10.0.0.2 -P 4 -A 0,1,2,3 -Z -t 30
```

---

## Command Line Options

```text
Usage: ./uring_perf [options]

Client/Server Options:
  -s, --server              Run in server mode
  -c, --client <host>       Run in client mode connecting to <host>
  -p, --port <port>         Server port to listen on/connect to (default: 5201)
  -u, --udp                 Use UDP protocol instead of TCP

Performance & Zero-Copy Options:
  -Z, --zerocopy            Enable zero-copy send (IORING_OP_SEND_ZC)
  -S, --sqpoll              Enable Kernel Submission Queue Polling (IORING_SETUP_SQPOLL)
  -P, --parallel <threads>  Number of parallel worker threads (default: 1)
  -l, --len <bytes>         Payload buffer size in bytes (default: 131072 for TCP)
  -q, --queue-depth <depth> io_uring queue depth per ring (default: 128)
  -F, --no-fixed-buf        Disable io_uring fixed registered buffers
  -A, --affinity <cpus>     Comma-separated CPU cores for thread pinning (e.g. 0,1,2,3)

Timing & Reporting Options:
  -t, --time <seconds>      Duration in seconds for test transmission (default: 10)
  -i, --interval <seconds>  Reporting interval in seconds (default: 1)
  -h, --help                Display help menu
```
