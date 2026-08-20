#ifndef URING_ENGINE_HPP
#define URING_ENGINE_HPP

#include <liburing.h>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <system_error>
#include "config.hpp"

// Data tag attached to io_uring SQEs to track completion context
struct IOContext {
    enum Type { ACCEPT, SEND, RECV, TIMER } type;
    int fd;
    uint32_t buf_idx;
    void* buf_ptr;
    size_t length;
    uint32_t stream_id;
};

class UringEngine {
public:
    UringEngine(uint32_t queue_depth, bool use_sqpoll);
    ~UringEngine();

    // Disable copy
    UringEngine(const UringEngine&) = delete;
    UringEngine& operator=(const UringEngine&) = delete;

    // Registers page-aligned buffer array with kernel for zero-copy fixed memory access
    bool register_buffers(const std::vector<void*>& buffers, size_t buf_size);

    // Pre-registers file descriptor list with io_uring ring
    bool register_files(const std::vector<int>& fds);

    // SQE submission builders
    bool prep_send_zc(int fd_idx_or_fd, void* buf, size_t len, uint32_t buf_idx, IOContext* ctx, bool use_fixed_file = false);
    bool prep_send_standard(int fd, void* buf, size_t len, IOContext* ctx);
    bool prep_recv(int fd, void* buf, size_t len, uint32_t buf_idx, IOContext* ctx);
    bool prep_accept(int listen_fd, struct sockaddr* client_addr, socklen_t* addr_len, IOContext* ctx);

    // Submits queued SQEs to kernel
    int submit();
    int submit_and_wait(uint32_t wait_nr);

    // Harvest completions
    int reap_completions(std::vector<struct io_uring_cqe*>& cqes_out, uint32_t max_reap = 64);
    void cqe_seen(struct io_uring_cqe* cqe);

    struct io_uring* get_ring() { return &ring_; }
    bool is_zc_supported() const { return zc_supported_; }

private:
    struct io_uring ring_;
    uint32_t queue_depth_;
    bool sqpoll_enabled_;
    bool buffers_registered_{false};
    bool files_registered_{false};
    bool zc_supported_{true};
    std::vector<struct iovec> iovecs_;
};

#endif // URING_ENGINE_HPP
