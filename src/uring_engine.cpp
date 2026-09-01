#include "uring_engine.hpp"
#include <iostream>
#include <cstring>
#include <cerrno>
#include <cstdlib>

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
    // IORING_SETUP_COOP_TASKRUN's deferred-notification model is specific to
    // a ring whose *application* thread calls io_uring_enter() to reap
    // completions; it does not apply once a kernel thread (SQPOLL) is the
    // one driving the ring, and the kernel rejects the combination outright
    // with -EINVAL. Setting both unconditionally silently broke -S on every
    // kernel where COOP_TASKRUN is available: io_uring_queue_init_params()
    // failed, and the fallback path below quietly dropped SQPOLL along with
    // every other flag, so -S was running as plain mode the entire time.
#ifdef IORING_SETUP_COOP_TASKRUN
    if (!sqpoll_enabled_) {
        params.flags |= IORING_SETUP_COOP_TASKRUN;
    }
#endif

    int ret = io_uring_queue_init_params(queue_depth_, &ring_, &params);
    if (ret < 0) {
        if (sqpoll_enabled_) {
            std::cerr << "[Warning] io_uring_queue_init_params with SQPOLL failed: "
                      << std::strerror(-ret) << ", falling back without it\n";
        }
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
    if (recv_buf_ring_) {
        io_uring_free_buf_ring(&ring_, recv_buf_ring_, recv_buf_count_, recv_bgid_);
        std::free(recv_buf_mem_);
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
    // NOTE: IORING_OP_SEND_ZC is a C enum member from <liburing/io_uring.h>, not a
    // preprocessor macro, so `#ifdef IORING_OP_SEND_ZC` is never true and previously
    // always fell through to prep_send_standard() -- but only after already pulling
    // an SQE via io_uring_get_sqe() that was then discarded unused. That wasted one
    // SQE/CQE slot per send; once the ring filled from the doubled consumption,
    // callers' unchecked prep_send_zc() return values meant resubmits silently
    // stopped, buffers fell out of rotation, and the client eventually blocked
    // forever in submit_and_wait() with nothing left in flight.
    if (!zc_supported_) {
        return prep_send_standard(fd, buf, len, ctx);
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;

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

bool UringEngine::prep_send_standard(int fd, void* buf, size_t len, IOContext* ctx, bool use_fixed_file) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;

    io_uring_prep_send(sqe, fd, buf, len, 0);
    if (buffers_registered_) {
        // NOTE: IORING_RECVSEND_FIXED_BUF is documented as generic to any
        // send/recv opcode, and it does work for IORING_OP_SEND_ZC (verified:
        // "Zero-Copy Ops" counts confirm it there) -- but empirically, adding
        // it here to plain IORING_OP_SEND makes every send fail silently on
        // this kernel/liburing (6.8.0-137 / liburing 2.5): no CQE ever comes
        // back with a nonzero result, the ring goes idle, and the worker hangs
        // in submit_and_wait() forever, needing SIGKILL. Confirmed by isolating
        // this single flag: removing it alone restores normal operation.
        // buf_index is still set (harmless without the flag) in case a future
        // kernel/liburing combination honors it for plain SEND.
        sqe->buf_index = ctx->buf_idx;
    }
    if (use_fixed_file && files_registered_) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    io_uring_sqe_set_data(sqe, ctx);
    return true;
}

bool UringEngine::prep_recv(int fd, void* buf, size_t len, uint32_t buf_idx, IOContext* ctx, bool use_fixed_file) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;

    io_uring_prep_recv(sqe, fd, buf, len, 0);
    if (buffers_registered_) {
        // See the matching note in prep_send_standard(): IORING_RECVSEND_FIXED_BUF
        // on plain IORING_OP_RECV hits the same silent-failure/hang behavior.
        sqe->buf_index = buf_idx;
    }
    if (use_fixed_file && files_registered_) {
        sqe->flags |= IOSQE_FIXED_FILE;
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

namespace {
uint32_t next_pow2(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}
} // namespace

bool UringEngine::setup_multishot_recv(uint32_t buf_count, uint32_t buf_size, uint16_t bgid) {
    buf_count = next_pow2(buf_count);

    int err = 0;
    recv_buf_ring_ = io_uring_setup_buf_ring(&ring_, buf_count, bgid, 0, &err);
    if (!recv_buf_ring_) {
        std::cerr << "[Warning] io_uring_setup_buf_ring failed: " << std::strerror(-err)
                  << " -- falling back to per-completion recv\n";
        return false;
    }

    if (posix_memalign(&recv_buf_mem_, 4096, static_cast<size_t>(buf_count) * buf_size) != 0) {
        std::cerr << "[Warning] failed to allocate multishot recv buffer pool\n";
        io_uring_free_buf_ring(&ring_, recv_buf_ring_, buf_count, bgid);
        recv_buf_ring_ = nullptr;
        return false;
    }

    recv_buf_count_ = buf_count;
    recv_buf_size_ = buf_size;
    recv_bgid_ = bgid;

    int mask = io_uring_buf_ring_mask(buf_count);
    char* base = static_cast<char*>(recv_buf_mem_);
    for (uint32_t i = 0; i < buf_count; ++i) {
        io_uring_buf_ring_add(recv_buf_ring_, base + static_cast<size_t>(i) * buf_size,
                               buf_size, static_cast<uint16_t>(i), mask, static_cast<int>(i));
    }
    io_uring_buf_ring_advance(recv_buf_ring_, static_cast<int>(buf_count));
    return true;
}

bool UringEngine::prep_recv_multishot(int fd, IOContext* ctx, bool use_fixed_file) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;

    io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = recv_bgid_;
    if (use_fixed_file && files_registered_) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    io_uring_sqe_set_data(sqe, ctx);
    return true;
}

void UringEngine::return_recv_buffer(uint16_t buf_id) {
    int mask = io_uring_buf_ring_mask(recv_buf_count_);
    char* base = static_cast<char*>(recv_buf_mem_);
    io_uring_buf_ring_add(recv_buf_ring_, base + static_cast<size_t>(buf_id) * recv_buf_size_,
                           recv_buf_size_, buf_id, mask, 0);
    io_uring_buf_ring_advance(recv_buf_ring_, 1);
}

void* UringEngine::recv_buffer_data(uint16_t buf_id) const {
    return static_cast<char*>(recv_buf_mem_) + static_cast<size_t>(buf_id) * recv_buf_size_;
}

int UringEngine::submit() {
    return io_uring_submit(&ring_);
}

int UringEngine::submit_and_wait(uint32_t wait_nr) {
    // A bounded wait, not io_uring_submit_and_wait()'s unconditional block:
    // if the peer stops sending/receiving before this side's own -t duration
    // ends (e.g. the other process already hit its deadline and closed up),
    // there may be nothing left to generate a completion at all -- an
    // unconditional wait would then block forever past this worker's own
    // stop_signal_ check, leaving the process hung needing a manual kill.
    // Callers already ignore this return value and just loop back around to
    // recheck stop_signal_, so returning -ETIME on a quiet 200ms is safe.
    struct __kernel_timespec ts = {0, 200000000}; // 200ms
    struct io_uring_cqe* cqe_ptr = nullptr;
    return io_uring_submit_and_wait_timeout(&ring_, &cqe_ptr, wait_nr, &ts, nullptr);
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
