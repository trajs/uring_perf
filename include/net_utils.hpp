#ifndef NET_UTILS_HPP
#define NET_UTILS_HPP

#include <string>
#include <cstdint>
#include <cstddef>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <system_error>

namespace net_utils {

// Sets socket to non-blocking mode
int set_nonblocking(int fd);

// Sets socket options: TCP_NODELAY, SO_REUSEPORT, SO_ZEROCOPY, SNDBUF/RCVBUF,
// and for UDP, UDP_SEGMENT (GSO, send) + UDP_GRO (receive) at udp_segment_size.
int tune_socket(int fd, bool zero_copy, bool reuse_port, bool is_udp, uint32_t udp_segment_size = 1472);

// Creates and binds a server listening socket (with SO_REUSEPORT for multi-threading)
int create_listen_socket(const std::string& ip, uint16_t port, bool is_udp, bool zero_copy, uint32_t udp_segment_size = 1472);

// Creates and connects a client socket to server host/port
int create_client_socket(const std::string& host, uint16_t port, bool is_udp, bool zero_copy, uint32_t udp_segment_size = 1472);

// Page-aligned memory allocation helper (4KB aligned for direct DMA zero-copy)
void* allocate_aligned_buffer(size_t size, size_t alignment = 4096);

// Free page-aligned memory
void free_aligned_buffer(void* ptr);

// Blocking (retry-on-EAGAIN via poll) send/recv of the full buffer, regardless
// of the socket's own blocking mode. Used only for the tiny one-time -R
// handshake exchanged right after TCP connect, before any io_uring op is
// submitted on the connection. timeout_ms < 0 waits indefinitely.
bool send_all(int fd, const void* buf, size_t len, int timeout_ms = -1);
bool recv_all(int fd, void* buf, size_t len, int timeout_ms = -1);

// Reverse-mode handshake: the client sends its role choice and its own -t
// duration immediately after connect(); the server reads it before deciding
// whether this connection receives (normal) or sends (reverse) data. TCP only.
bool send_handshake(int fd, bool reverse, uint32_t duration_sec);
bool recv_handshake(int fd, bool& reverse, uint32_t& duration_sec);

} // namespace net_utils

#endif // NET_UTILS_HPP
