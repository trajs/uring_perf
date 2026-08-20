#include "worker.hpp"
#include "net_utils.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <unordered_map>

#ifndef IORING_CQE_F_NOTIF
#define IORING_CQE_F_NOTIF (1U << 3)
#endif

Worker::Worker(size_t thread_id, const Config& config, StatsCollector& stats, std::atomic<bool>& stop_signal)
    : thread_id_(thread_id), config_(config), stats_(stats), stop_signal_(stop_signal) {}

Worker::~Worker() {
    join();
}

void Worker::start() {
    if (config_.mode == Mode::CLIENT) {
        thread_ = std::thread(&Worker::run_client, this);
    } else {
        thread_ = std::thread(&Worker::run_server, this);
    }
}

void Worker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Worker::set_cpu_affinity() {
    if (thread_id_ < config_.cpu_affinity.size()) {
        int core_id = config_.cpu_affinity[thread_id_];
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        pthread_t current_thread = pthread_self();
        if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) == 0) {
            std::cout << "[Thread " << thread_id_ << "] Pinned to CPU core " << core_id << "\n";
        }
    }
}

void Worker::run_client() {
    set_cpu_affinity();

    int sockfd = net_utils::create_client_socket(config_.server_ip, config_.port, 
                                                 config_.protocol == Protocol::UDP, config_.zero_copy);
    if (sockfd < 0) {
        std::cerr << "[Thread " << thread_id_ << "] Failed to connect client socket\n";
        return;
    }

    UringEngine engine(config_.queue_depth, config_.sqpoll);

    uint32_t num_buffers = config_.in_flight;
    buffer_pool_.reserve(num_buffers);
    context_pool_.reserve(num_buffers);

    for (uint32_t i = 0; i < num_buffers; ++i) {
        void* buf = net_utils::allocate_aligned_buffer(config_.buf_size);
        buffer_pool_.push_back(buf);

        auto ctx = std::make_unique<IOContext>();
        ctx->type = IOContext::SEND;
        ctx->fd = sockfd;
        ctx->buf_idx = i;
        ctx->buf_ptr = buf;
        ctx->length = config_.buf_size;
        ctx->stream_id = static_cast<uint32_t>(thread_id_);
        context_pool_.push_back(std::move(ctx));
    }

    if (config_.fixed_buffers) {
        engine.register_buffers(buffer_pool_, config_.buf_size);
    }

    for (uint32_t i = 0; i < num_buffers; ++i) {
        if (config_.zero_copy) {
            engine.prep_send_zc(sockfd, buffer_pool_[i], config_.buf_size, i, context_pool_[i].get(), false);
        } else {
            engine.prep_send_standard(sockfd, buffer_pool_[i], config_.buf_size, context_pool_[i].get());
        }
    }
    engine.submit();

    std::vector<struct io_uring_cqe*> cqes;

    while (!stop_signal_.load(std::memory_order_relaxed)) {
        int reaped = engine.reap_completions(cqes, num_buffers);
        if (reaped <= 0) {
            engine.submit_and_wait(1);
            continue;
        }

        for (int i = 0; i < reaped; ++i) {
            struct io_uring_cqe* cqe = cqes[i];
            IOContext* ctx = reinterpret_cast<IOContext*>(io_uring_cqe_get_data(cqe));

            if (!ctx) {
                engine.cqe_seen(cqe);
                continue;
            }

            if (cqe->flags & IORING_CQE_F_NOTIF) {
                stats_.add_zc_notification(thread_id_);
                engine.cqe_seen(cqe);
                continue;
            }

            if (cqe->res > 0) {
                stats_.add_bytes(thread_id_, cqe->res, 1);
                
                if (!stop_signal_.load(std::memory_order_relaxed)) {
                    if (config_.zero_copy) {
                        engine.prep_send_zc(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx, false);
                    } else {
                        engine.prep_send_standard(ctx->fd, ctx->buf_ptr, ctx->length, ctx);
                    }
                }
            } else if (cqe->res == -EAGAIN || cqe->res == -EWOULDBLOCK) {
                if (!stop_signal_.load(std::memory_order_relaxed)) {
                    if (config_.zero_copy) {
                        engine.prep_send_zc(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx, false);
                    } else {
                        engine.prep_send_standard(ctx->fd, ctx->buf_ptr, ctx->length, ctx);
                    }
                }
            } else if (cqe->res < 0) {
                stats_.add_error(thread_id_);
            }

            engine.cqe_seen(cqe);
        }
        engine.submit();
    }

    close(sockfd);
    for (void* buf : buffer_pool_) {
        net_utils::free_aligned_buffer(buf);
    }
}

struct ClientSession {
    int client_fd;
    std::vector<void*> buffers;
    std::vector<std::unique_ptr<IOContext>> contexts;
};

void Worker::run_server() {
    set_cpu_affinity();

    int listen_fd = net_utils::create_listen_socket("0.0.0.0", config_.port, 
                                                   config_.protocol == Protocol::UDP, config_.zero_copy);
    if (listen_fd < 0) {
        std::cerr << "[Thread " << thread_id_ << "] Failed to create listen socket\n";
        return;
    }

    UringEngine engine(config_.queue_depth, config_.sqpoll);

    std::unordered_map<int, std::unique_ptr<ClientSession>> sessions;

    auto accept_ctx = std::make_unique<IOContext>();
    accept_ctx->type = IOContext::ACCEPT;
    accept_ctx->fd = listen_fd;

    struct sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    if (config_.protocol == Protocol::UDP) {
        // UDP receiver pool
        uint32_t num_buffers = config_.in_flight;
        buffer_pool_.reserve(num_buffers);
        context_pool_.reserve(num_buffers);

        for (uint32_t i = 0; i < num_buffers; ++i) {
            void* buf = net_utils::allocate_aligned_buffer(config_.buf_size);
            buffer_pool_.push_back(buf);

            auto ctx = std::make_unique<IOContext>();
            ctx->type = IOContext::RECV;
            ctx->fd = listen_fd;
            ctx->buf_idx = i;
            ctx->buf_ptr = buf;
            ctx->length = config_.buf_size;
            ctx->stream_id = static_cast<uint32_t>(thread_id_);
            context_pool_.push_back(std::move(ctx));

            engine.prep_recv(listen_fd, buf, config_.buf_size, i, context_pool_[i].get());
        }
    } else {
        engine.prep_accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len, accept_ctx.get());
    }
    engine.submit();

    std::vector<struct io_uring_cqe*> cqes;

    while (!stop_signal_.load(std::memory_order_relaxed)) {
        int reaped = engine.reap_completions(cqes, config_.in_flight);
        if (reaped <= 0) {
            engine.submit_and_wait(1);
            continue;
        }

        for (int i = 0; i < reaped; ++i) {
            struct io_uring_cqe* cqe = cqes[i];
            IOContext* ctx = reinterpret_cast<IOContext*>(io_uring_cqe_get_data(cqe));

            if (!ctx) {
                engine.cqe_seen(cqe);
                continue;
            }

            if (ctx->type == IOContext::ACCEPT) {
                int client_fd = cqe->res;
                if (client_fd >= 0) {
                    net_utils::tune_socket(client_fd, config_.zero_copy, false, false);
                    net_utils::set_nonblocking(client_fd);

                    auto session = std::make_unique<ClientSession>();
                    session->client_fd = client_fd;
                    uint32_t num_buffers = config_.in_flight;
                    session->buffers.reserve(num_buffers);
                    session->contexts.reserve(num_buffers);

                    for (uint32_t b = 0; b < num_buffers; ++b) {
                        void* buf = net_utils::allocate_aligned_buffer(config_.buf_size);
                        session->buffers.push_back(buf);

                        auto rctx = std::make_unique<IOContext>();
                        rctx->type = IOContext::RECV;
                        rctx->fd = client_fd;
                        rctx->buf_idx = b;
                        rctx->buf_ptr = buf;
                        rctx->length = config_.buf_size;
                        rctx->stream_id = static_cast<uint32_t>(thread_id_);

                        engine.prep_recv(client_fd, buf, config_.buf_size, b, rctx.get());
                        session->contexts.push_back(std::move(rctx));
                    }
                    sessions[client_fd] = std::move(session);
                }
                engine.prep_accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len, accept_ctx.get());
            } else if (ctx->type == IOContext::RECV) {
                if (cqe->res > 0) {
                    stats_.add_bytes(thread_id_, cqe->res, 1);
                    engine.prep_recv(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx);
                } else if (cqe->res == -EAGAIN || cqe->res == -EWOULDBLOCK) {
                    engine.prep_recv(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx);
                } else {
                    // EOF (0) or socket error: clean up connection session
                    if (cqe->res < 0) {
                        stats_.add_error(thread_id_);
                    }
                    auto it = sessions.find(ctx->fd);
                    if (it != sessions.end()) {
                        close(it->first);
                        for (void* buf : it->second->buffers) {
                            net_utils::free_aligned_buffer(buf);
                        }
                        sessions.erase(it);
                    }
                }
            }

            engine.cqe_seen(cqe);
        }
        engine.submit();
    }

    close(listen_fd);
    for (auto& kv : sessions) {
        close(kv.first);
        for (void* buf : kv.second->buffers) {
            net_utils::free_aligned_buffer(buf);
        }
    }
}
