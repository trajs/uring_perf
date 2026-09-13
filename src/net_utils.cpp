#include "net_utils.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <poll.h>

#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif

namespace net_utils {

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int tune_socket(int fd, bool zero_copy, bool reuse_port, bool is_udp, uint32_t udp_segment_size) {
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
        // UDP_SEGMENT (send-side GSO): the kernel/NIC splits one large send
        // buffer into udp_segment_size datagrams in a single pass through the
        // stack, instead of one full stack traversal per datagram. Only pays
        // off when the buffer handed to send() is actually larger than
        // udp_segment_size -- see config.hpp's UDP default buf_size.
        int gso_size = static_cast<int>(udp_segment_size);
        setsockopt(fd, SOL_UDP, UDP_SEGMENT, &gso_size, sizeof(gso_size));

        // UDP_GRO (receive-side coalescing): the kernel merges consecutive
        // same-flow datagrams into one larger buffer before a single recv()
        // call picks them all up, the receive-side mirror of UDP_SEGMENT.
        // Harmless to set on a socket that only sends.
        int gro_on = 1;
        setsockopt(fd, SOL_UDP, UDP_GRO, &gro_on, sizeof(gro_on));
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

int create_listen_socket(const std::string& ip, uint16_t port, bool is_udp, bool zero_copy, uint32_t udp_segment_size) {
    int sock_type = is_udp ? SOCK_DGRAM : SOCK_STREAM;
    int fd = socket(AF_INET, sock_type, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    tune_socket(fd, zero_copy, true, is_udp, udp_segment_size);
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

int create_client_socket(const std::string& host, uint16_t port, bool is_udp, bool zero_copy, uint32_t udp_segment_size, uint16_t local_port) {
    int sock_type = is_udp ? SOCK_DGRAM : SOCK_STREAM;
    int fd = socket(AF_INET, sock_type, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    tune_socket(fd, zero_copy, false, is_udp, udp_segment_size);

    if (local_port != 0) {
        struct sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(local_port);
        local_addr.sin_addr.s_addr = INADDR_ANY;
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(fd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            perror("bind (local_port)");
            close(fd);
            return -1;
        }
    }

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

    // Connect for both TCP and UDP: the io_uring send path (prep_send_standard /
    // prep_send_zc) issues plain send()-style ops with no per-call destination,
    // so a UDP client socket needs a default peer just like a TCP socket does.
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
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

bool send_all(int fd, const void* buf, size_t len, int timeout_ms) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, timeout_ms) <= 0) return false; // timeout or poll error
            continue;
        }
        return false;
    }
    return true;
}

bool recv_all(int fd, void* buf, size_t len, int timeout_ms) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) return false; // peer closed before completing the transfer
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            struct pollfd pfd{fd, POLLIN, 0};
            if (poll(&pfd, 1, timeout_ms) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool send_handshake(int fd, bool reverse, uint32_t duration_sec) {
    uint8_t buf[5];
    buf[0] = reverse ? 1 : 0;
    uint32_t dnet = htonl(duration_sec);
    std::memcpy(buf + 1, &dnet, sizeof(dnet));
    return send_all(fd, buf, sizeof(buf));
}

bool recv_handshake(int fd, bool& reverse, uint32_t& duration_sec) {
    uint8_t buf[5];
    // 5s bounds how long a slow/stuck client can stall this worker's whole
    // event loop: the read happens synchronously inside accept handling,
    // before any io_uring op is set up for the new connection.
    if (!recv_all(fd, buf, sizeof(buf), 5000)) return false;
    reverse = buf[0] != 0;
    uint32_t dnet;
    std::memcpy(&dnet, buf + 1, sizeof(dnet));
    duration_sec = ntohl(dnet);
    return true;
}

} // namespace net_utils
