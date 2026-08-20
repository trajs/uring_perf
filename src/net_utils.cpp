#include "net_utils.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>

#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

namespace net_utils {

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int tune_socket(int fd, bool zero_copy, bool reuse_port, bool is_udp) {
    int optval = 1;
    
    // Enable SO_REUSEADDR
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // Enable SO_REUSEPORT if requested (for multi-threaded listener)
    if (reuse_port) {
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
#endif
    }

    // Tune socket buffer sizes to 4MB for high bandwidth-delay product
    int sndbuf = 4 * 1024 * 1024;
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    if (!is_udp) {
        // Disable Nagle's algorithm for immediate TCP sends (enables TSO super-packets)
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
    } else {
        // Enable UDP Generic Segmentation Offload (UDP GSO / TSO)
#ifdef UDP_SEGMENT
        int gso_size = 1472;
        setsockopt(fd, SOL_UDP, UDP_SEGMENT, &gso_size, sizeof(gso_size));
#endif
    }

    // Enable socket level zero copy if requested
    if (zero_copy) {
#ifdef SO_ZEROCOPY
        if (setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &optval, sizeof(optval)) < 0) {
            std::cerr << "[Warning] SO_ZEROCOPY socket option failed: " 
                      << std::strerror(errno) << " (Continuing with kernel io_uring fallback)\n";
        }
#endif
    }

    return 0;
}

int create_listen_socket(const std::string& ip, uint16_t port, bool is_udp, bool zero_copy) {
    int sock_type = is_udp ? SOCK_DGRAM : SOCK_STREAM;
    int fd = socket(AF_INET, sock_type, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    tune_socket(fd, zero_copy, true, is_udp);
    set_nonblocking(fd);

    struct sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    }

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (!is_udp) {
        if (listen(fd, 1024) < 0) {
            perror("listen");
            close(fd);
            return -1;
        }
    }

    return fd;
}

int create_client_socket(const std::string& host, uint16_t port, bool is_udp, bool zero_copy) {
    int sock_type = is_udp ? SOCK_DGRAM : SOCK_STREAM;
    int fd = socket(AF_INET, sock_type, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    tune_socket(fd, zero_copy, false, is_udp);

    struct sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) {
            std::cerr << "Failed to resolve host: " << host << "\n";
            close(fd);
            return -1;
        }
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (!is_udp) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(fd);
            return -1;
        }
    }

    set_nonblocking(fd);
    return fd;
}

void* allocate_aligned_buffer(size_t size, size_t alignment) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    std::memset(ptr, 0xAB, size);
    return ptr;
}

void free_aligned_buffer(void* ptr) {
    if (ptr) {
        std::free(ptr);
    }
}

} // namespace net_utils
