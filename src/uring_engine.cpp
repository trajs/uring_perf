#include "uring_engine.hpp"
#include <iostream>
#include <cstring>
#include <cerrno>

UringEngine::UringEngine(uint32_t queue_depth, bool use_sqpoll)
    : queue_depth_(queue_depth), sqpoll_enabled_(use_sqpoll) {
    
    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));

    if (sqpoll_enabled_) {
#ifdef IORING_SETUP_SQPOLL
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000;
#else
        std::cerr << "[Warning] IORING_SETUP_SQPOLL not defined, disabling SQPOLL\n";
        sqpoll_enabled_ = false;
#endif
    }

#ifdef IORING_SETUP_SINGLE_ISSUER
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
#endif
#ifdef IORING_SETUP_COOP_TASKRUN
    params.flags |= IORING_SETUP_COOP_TASKRUN;
#endif

    int ret = io_uring_queue_init_params(queue_depth_, &ring_, &params);
    if (ret < 0) {
        std::memset(&params, 0, sizeof(params));
        ret = io_uring_queue_init(queue_depth_, &ring_, 0);
        if (ret < 0) {
            throw std::system_error(-ret, std::generic_category(), "io_uring_queue_init failed");
        }
        sqpoll_enabled_ = false;
    }
}

UringEngine::~UringEngine() {
    if (buffers_registered_) {
        io_uring_unregister_buffers(&ring_);
    }
    if (files_registered_) {
        io_uring_unregister_files(&ring_);
    }
    io_uring_queue_exit(&ring_);
}

bool UringEngine::register_buffers(const std::vector<void*>& buffers, size_t buf_size) {
    if (buffers.empty()) return false;

    iovecs_.clear();
    iovecs_.reserve(buffers.size());
    for (void* ptr : buffers) {
        struct iovec iov;
        iov.iov_base = ptr;
        iov.iov_len = buf_size;
        iovecs_.push_back(iov);
    }

    int ret = io_uring_register_buffers(&ring_, iovecs_.data(), static_cast<unsigned int>(iovecs_.size()));
    if (ret < 0) {
        std::cerr << "[Warning] io_uring_register_buffers failed: " << std::strerror(-ret) << "\n";
        buffers_registered_ = false;
        return false;
    }
    buffers_registered_ = true;
    return true;
}

bool UringEngine::register_files(const std::vector<int>& fds) {
    if (fds.empty()) return false;

    int ret = io_uring_register_files(&ring_, fds.data(), static_cast<unsigned int>(fds.size()));
    if (ret < 0) {
        std::cerr << "[Warning] io_uring_register_files failed: " << std::strerror(-ret) << "\n";
        files_registered_ = false;
        return false;
    }
    files_registered_ = true;
    return true;
}

bool UringEngine::prep_send_zc(int fd, void* buf, size_t len, uint32_t buf_idx, IOContext* ctx, bool use_fixed_file) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;

#ifdef IORING_OP_SEND_ZC
    if (zc_supported_) {
        if (buffers_registered_) {
            io_uring_prep_send_zc_fixed(sqe, fd, buf, len, 0, 0, buf_idx);
        } else {
            io_uring_prep_send_zc(sqe, fd, buf, len, 0, 0);
        }
        if (use_fixed_file && files_registered_) {
            sqe->flags |= IOSQE_FIXED_FILE;
        }
        io_uring_sqe_set_data(sqe, ctx);
        return true;
    }
#endif

    return prep_send_standard(fd, buf, len, ctx);
}

bool UringEngine::prep_send_standard(int fd, void* buf, size_t len, IOContext* ctx) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;

    io_uring_prep_send(sqe, fd, buf, len, 0);
    if (buffers_registered_) {
        sqe->buf_index = ctx->buf_idx;
    }
    io_uring_sqe_set_data(sqe, ctx);
    return true;
}

bool UringEngine::prep_recv(int fd, void* buf, size_t len, uint32_t buf_idx, IOContext* ctx) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;

    io_uring_prep_recv(sqe, fd, buf, len, 0);
    if (buffers_registered_) {
        sqe->buf_index = buf_idx;
    }
    io_uring_sqe_set_data(sqe, ctx);
    return true;
}

bool UringEngine::prep_accept(int listen_fd, struct sockaddr* client_addr, socklen_t* addr_len, IOContext* ctx) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;

    io_uring_prep_accept(sqe, listen_fd, client_addr, addr_len, 0);
    io_uring_sqe_set_data(sqe, ctx);
    return true;
}

int UringEngine::submit() {
    return io_uring_submit(&ring_);
}

int UringEngine::submit_and_wait(uint32_t wait_nr) {
    return io_uring_submit_and_wait(&ring_, wait_nr);
}

int UringEngine::reap_completions(std::vector<struct io_uring_cqe*>& cqes_out, uint32_t max_reap) {
    cqes_out.clear();
    cqes_out.resize(max_reap);

    unsigned head;
    struct io_uring_cqe* cqe;
    uint32_t count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe) {
        if (count >= max_reap) break;
        cqes_out[count++] = cqe;
    }
    cqes_out.resize(count);
    return count;
}

void UringEngine::cqe_seen(struct io_uring_cqe* cqe) {
    io_uring_cqe_seen(&ring_, cqe);
}
