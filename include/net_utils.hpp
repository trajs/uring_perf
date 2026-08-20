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

// Sets socket options: TCP_NODELAY, SO_REUSEPORT, SO_ZEROCOPY, SNDBUF/RCVBUF
int tune_socket(int fd, bool zero_copy, bool reuse_port, bool is_udp);

// Creates and binds a server listening socket (with SO_REUSEPORT for multi-threading)
int create_listen_socket(const std::string& ip, uint16_t port, bool is_udp, bool zero_copy);

// Creates and connects a client socket to server host/port
int create_client_socket(const std::string& host, uint16_t port, bool is_udp, bool zero_copy);

// Page-aligned memory allocation helper (4KB aligned for direct DMA zero-copy)
void* allocate_aligned_buffer(size_t size, size_t alignment = 4096);

// Free page-aligned memory
void free_aligned_buffer(void* ptr);

} // namespace net_utils

#endif // NET_UTILS_HPP
