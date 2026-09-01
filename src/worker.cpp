#include "worker.hpp"
#include "net_utils.hpp"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <unordered_map>
#include <net/if.h>
#include <sys/mman.h>
#include <cerrno>

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

    // zcrx is a receive-side (server) feature with no client-side io_uring
    // change, but each zcrx server worker listens on a distinct per-thread
    // port (see run_server_zcrx()), so the client has to match that scheme.
    uint16_t connect_port = config_.port + (config_.use_zcrx ? static_cast<uint16_t>(thread_id_) : 0);

    int sockfd = net_utils::create_client_socket(config_.server_ip, connect_port,
                                                 config_.protocol == Protocol::UDP, config_.zero_copy,
                                                 config_.udp_gso_size);
    if (sockfd < 0) {
        std::cerr << "[Thread " << thread_id_ << "] Failed to connect client socket\n";
        return;
    }

    if (config_.protocol == Protocol::TCP) {
        // Tell the server which direction this connection should run: the
        // handshake is a tiny blocking exchange (well under the socket's send
        // buffer) that completes before either side sets up any io_uring op
        // on the connection, so it's safe to do synchronously here.
        if (!net_utils::send_handshake(sockfd, config_.reverse, config_.duration_sec)) {
            std::cerr << "[Thread " << thread_id_ << "] Failed to send handshake to server\n";
            close(sockfd);
            return;
        }
    }

    if (config_.reverse && config_.protocol == Protocol::TCP) {
        run_client_reverse_recv(sockfd);
        close(sockfd);
        return;
    }

    UringEngine engine(config_.queue_depth, config_.sqpoll);

    // The client only ever owns one socket, so it's safe to register it as a
    // fixed file (IORING_REGISTER_FILES): every send then references it by
    // index (0) with IOSQE_FIXED_FILE, skipping the kernel's per-op fd-table
    // lookup. `sockfd` itself is kept for close() regardless.
    bool use_fixed_file = config_.fixed_files && engine.register_files({sockfd});
    int send_fd = use_fixed_file ? 0 : sockfd;

    uint32_t num_buffers = config_.in_flight;
    buffer_pool_.reserve(num_buffers);
    context_pool_.reserve(num_buffers);

    for (uint32_t i = 0; i < num_buffers; ++i) {
        void* buf = net_utils::allocate_aligned_buffer(config_.buf_size);
        buffer_pool_.push_back(buf);

        auto ctx = std::make_unique<IOContext>();
        ctx->type = IOContext::SEND;
        ctx->fd = send_fd;
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
            engine.prep_send_zc(send_fd, buffer_pool_[i], config_.buf_size, i, context_pool_[i].get(), use_fixed_file);
        } else {
            engine.prep_send_standard(send_fd, buffer_pool_[i], config_.buf_size, context_pool_[i].get(), use_fixed_file);
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
                // For UDP with GSO batching, cqe->res is the total bytes
                // accepted across the whole batched send, not one datagram --
                // approximate the packet count from the wire segment size
                // instead of the pre-GSO assumption of "1 completion = 1 packet".
                uint64_t pkts = (config_.protocol == Protocol::UDP)
                    ? std::max<uint64_t>(1, static_cast<uint64_t>(cqe->res) / config_.udp_gso_size)
                    : 1;
                stats_.add_bytes(thread_id_, cqe->res, pkts);

                // NOTE: tried tracking partial sends correctly here (advance
                // buf_ptr/shrink length by cqe->res instead of always resubmitting
                // the full buffer) -- measured *worse*: cqe->res is capped at
                // min(requested, available), so requesting the full buffer every
                // time greedily claims whatever room happens to be available,
                // while requesting only the remainder can under-ask relative to
                // what actually freed up. Reverted; this is throughput-benchmark-
                // specific reasoning (content doesn't matter here) and would not
                // apply to an application that needs exact byte-accurate sends.
                if (!stop_signal_.load(std::memory_order_relaxed)) {
                    if (config_.zero_copy) {
                        engine.prep_send_zc(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx, use_fixed_file);
                    } else {
                        engine.prep_send_standard(ctx->fd, ctx->buf_ptr, ctx->length, ctx, use_fixed_file);
                    }
                }
            } else if (cqe->res == -EAGAIN || cqe->res == -EWOULDBLOCK) {
                if (!stop_signal_.load(std::memory_order_relaxed)) {
                    if (config_.zero_copy) {
                        engine.prep_send_zc(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx, use_fixed_file);
                    } else {
                        engine.prep_send_standard(ctx->fd, ctx->buf_ptr, ctx->length, ctx, use_fixed_file);
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

// -R/--reverse on the client: after the handshake tells the server to send
// instead of receive, the client runs the mirror image of run_server()'s
// per-connection receive logic (single connection, no accept/session map
// needed) on its own already-connected socket. Bounded by the same
// stop_signal_ / -t the send path would have used, plus EOF once the server
// closes at the end of its own per-session deadline (see run_server()).
void Worker::run_client_reverse_recv(int sockfd) {
    UringEngine engine(config_.queue_depth, config_.sqpoll);

    bool use_multishot = config_.multishot_recv &&
        engine.setup_multishot_recv(config_.in_flight, config_.buf_size);

    std::unique_ptr<IOContext> ms_ctx;
    std::vector<void*> recv_buffers;
    std::vector<std::unique_ptr<IOContext>> recv_contexts;

    if (use_multishot) {
        ms_ctx = std::make_unique<IOContext>();
        ms_ctx->type = IOContext::RECV;
        ms_ctx->fd = sockfd;
        ms_ctx->stream_id = static_cast<uint32_t>(thread_id_);
        engine.prep_recv_multishot(sockfd, ms_ctx.get());
    } else {
        uint32_t num_buffers = config_.in_flight;
        recv_buffers.reserve(num_buffers);
        recv_contexts.reserve(num_buffers);
        for (uint32_t i = 0; i < num_buffers; ++i) {
            void* buf = net_utils::allocate_aligned_buffer(config_.buf_size);
            recv_buffers.push_back(buf);

            auto ctx = std::make_unique<IOContext>();
            ctx->type = IOContext::RECV;
            ctx->fd = sockfd;
            ctx->buf_idx = i;
            ctx->buf_ptr = buf;
            ctx->length = config_.buf_size;
            ctx->stream_id = static_cast<uint32_t>(thread_id_);

            engine.prep_recv(sockfd, buf, config_.buf_size, i, ctx.get());
            recv_contexts.push_back(std::move(ctx));
        }
    }
    engine.submit();

    std::vector<struct io_uring_cqe*> cqes;
    bool peer_closed = false;

    while (!stop_signal_.load(std::memory_order_relaxed) && !peer_closed) {
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

            if (use_multishot) {
                if (cqe->res > 0 && (cqe->flags & IORING_CQE_F_BUFFER)) {
                    uint16_t buf_id = static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
                    stats_.add_bytes(thread_id_, cqe->res, 1);
                    engine.return_recv_buffer(buf_id);
                }
                if (!(cqe->flags & IORING_CQE_F_MORE)) {
                    if (cqe->res == -ENOBUFS) {
                        engine.prep_recv_multishot(sockfd, ctx);
                    } else {
                        if (cqe->res < 0) stats_.add_error(thread_id_);
                        peer_closed = true;
                    }
                }
            } else {
                if (cqe->res > 0) {
                    stats_.add_bytes(thread_id_, cqe->res, 1);
                    if (!stop_signal_.load(std::memory_order_relaxed)) {
                        engine.prep_recv(sockfd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx);
                    }
                } else if (cqe->res == -EAGAIN || cqe->res == -EWOULDBLOCK) {
                    if (!stop_signal_.load(std::memory_order_relaxed)) {
                        engine.prep_recv(sockfd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx);
                    }
                } else {
                    if (cqe->res < 0) stats_.add_error(thread_id_);
                    peer_closed = true;
                }
            }

            engine.cqe_seen(cqe);
        }
        engine.submit();
    }

    for (void* buf : recv_buffers) {
        net_utils::free_aligned_buffer(buf);
    }
}

struct ClientSession {
    int client_fd;
    bool is_reverse = false;                        // this session sends instead of receives
    std::chrono::steady_clock::time_point deadline{}; // only meaningful if is_reverse
    std::vector<void*> buffers;                    // single-shot fallback / reverse-send path
    std::vector<std::unique_ptr<IOContext>> contexts; // single-shot fallback / reverse-send path
    std::unique_ptr<IOContext> ms_ctx;              // multishot path only: one op per connection
};

void Worker::run_server() {
    if (config_.use_zcrx) {
        run_server_zcrx();
        return;
    }

    set_cpu_affinity();

    int listen_fd = net_utils::create_listen_socket("0.0.0.0", config_.port,
                                                   config_.protocol == Protocol::UDP, config_.zero_copy,
                                                   config_.udp_gso_size);
    if (listen_fd < 0) {
        std::cerr << "[Thread " << thread_id_ << "] Failed to create listen socket\n";
        return;
    }

    UringEngine engine(config_.queue_depth, config_.sqpoll);

    // Multishot IORING_OP_RECV against a kernel-managed provided buffer ring:
    // one submitted op keeps generating completions on its own as data
    // arrives, instead of the per-completion resubmit loop below needing to
    // pull a fresh SQE after every single receive. Falls back to that
    // per-completion path if the kernel/liburing combination doesn't support
    // provided buffer rings, or if disabled via --no-multishot-recv.
    bool use_multishot = config_.multishot_recv &&
        engine.setup_multishot_recv(config_.in_flight, config_.buf_size);

    std::unordered_map<int, std::unique_ptr<ClientSession>> sessions;

    auto accept_ctx = std::make_unique<IOContext>();
    accept_ctx->type = IOContext::ACCEPT;
    accept_ctx->fd = listen_fd;

    struct sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    // UDP has exactly one socket for the worker's lifetime (no accept()), so
    // -- unlike the TCP path below, where sockets arrive dynamically via
    // accept() and registering each one adds real bookkeeping complexity --
    // it's safe and simple to register it as a fixed file.
    bool udp_fixed_file = false;
    int udp_recv_fd = listen_fd;
    std::unique_ptr<IOContext> udp_ms_ctx;

    if (config_.protocol == Protocol::UDP) {
        udp_fixed_file = config_.fixed_files && engine.register_files({listen_fd});
        udp_recv_fd = udp_fixed_file ? 0 : listen_fd;

        if (use_multishot) {
            udp_ms_ctx = std::make_unique<IOContext>();
            udp_ms_ctx->type = IOContext::RECV;
            udp_ms_ctx->fd = udp_recv_fd;
            udp_ms_ctx->stream_id = static_cast<uint32_t>(thread_id_);
            engine.prep_recv_multishot(udp_recv_fd, udp_ms_ctx.get(), udp_fixed_file);
        } else {
            // UDP receiver pool (single-shot fallback)
            uint32_t num_buffers = config_.in_flight;
            buffer_pool_.reserve(num_buffers);
            context_pool_.reserve(num_buffers);

            for (uint32_t i = 0; i < num_buffers; ++i) {
                void* buf = net_utils::allocate_aligned_buffer(config_.buf_size);
                buffer_pool_.push_back(buf);

                auto ctx = std::make_unique<IOContext>();
                ctx->type = IOContext::RECV;
                ctx->fd = udp_recv_fd;
                ctx->buf_idx = i;
                ctx->buf_ptr = buf;
                ctx->length = config_.buf_size;
                ctx->stream_id = static_cast<uint32_t>(thread_id_);
                context_pool_.push_back(std::move(ctx));

                engine.prep_recv(udp_recv_fd, buf, config_.buf_size, i, context_pool_[i].get(), udp_fixed_file);
            }
        }
    } else {
        engine.prep_accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len, accept_ctx.get());
    }
    engine.submit();

    std::vector<struct io_uring_cqe*> cqes;

    while (!stop_signal_.load(std::memory_order_relaxed)) {
        // Reverse sessions have no natural EOF to trigger cleanup (the server
        // is the one sending) -- their end is instead this per-session
        // deadline, derived from the client's own -t and learned via the
        // handshake at accept time. Checked every loop iteration rather than
        // only after a completions batch, since submit_and_wait()'s 200ms
        // bound already guarantees regular wakeups even when idle.
        if (!sessions.empty()) {
            auto now = std::chrono::steady_clock::now();
            for (auto it = sessions.begin(); it != sessions.end(); ) {
                if (it->second->is_reverse && now >= it->second->deadline) {
                    close(it->first);
                    for (void* buf : it->second->buffers) {
                        net_utils::free_aligned_buffer(buf);
                    }
                    it = sessions.erase(it);
                } else {
                    ++it;
                }
            }
        }

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

                    // -R handshake: learn up front whether this connection
                    // sends or receives, and (if sending) for how long. Works
                    // regardless of the socket's blocking mode (see
                    // net_utils::recv_all), so it's fine to do this before or
                    // after set_nonblocking() below.
                    bool reverse = false;
                    uint32_t peer_duration = config_.duration_sec;
                    bool hs_ok = (config_.protocol != Protocol::TCP) ||
                        net_utils::recv_handshake(client_fd, reverse, peer_duration);

                    if (!hs_ok) {
                        close(client_fd);
                    } else {
                        net_utils::set_nonblocking(client_fd);

                        auto session = std::make_unique<ClientSession>();
                        session->client_fd = client_fd;

                        if (reverse) {
                            session->is_reverse = true;
                            session->deadline = std::chrono::steady_clock::now() +
                                                 std::chrono::seconds(peer_duration);

                            uint32_t num_buffers = config_.in_flight;
                            session->buffers.reserve(num_buffers);
                            session->contexts.reserve(num_buffers);

                            for (uint32_t b = 0; b < num_buffers; ++b) {
                                void* buf = net_utils::allocate_aligned_buffer(config_.buf_size);
                                session->buffers.push_back(buf);

                                auto sctx = std::make_unique<IOContext>();
                                sctx->type = IOContext::SEND;
                                sctx->fd = client_fd;
                                sctx->buf_idx = b;
                                sctx->buf_ptr = buf;
                                sctx->length = config_.buf_size;
                                sctx->stream_id = static_cast<uint32_t>(thread_id_);

                                if (config_.zero_copy) {
                                    engine.prep_send_zc(client_fd, buf, config_.buf_size, b, sctx.get());
                                } else {
                                    engine.prep_send_standard(client_fd, buf, config_.buf_size, sctx.get());
                                }
                                session->contexts.push_back(std::move(sctx));
                            }
                        } else if (use_multishot) {
                            session->ms_ctx = std::make_unique<IOContext>();
                            session->ms_ctx->type = IOContext::RECV;
                            session->ms_ctx->fd = client_fd;
                            session->ms_ctx->stream_id = static_cast<uint32_t>(thread_id_);
                            engine.prep_recv_multishot(client_fd, session->ms_ctx.get());
                        } else {
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
                        }
                        sessions[client_fd] = std::move(session);
                    }
                }
                engine.prep_accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len, accept_ctx.get());
            } else if (ctx->type == IOContext::SEND) {
                // Reverse-mode session: mirror run_client()'s send-completion
                // handling, but gated on this specific session still being
                // open and under its per-session deadline (checked above,
                // once per loop iteration) rather than a shared stop_signal_.
                auto it = sessions.find(ctx->fd);
                bool keep_sending = it != sessions.end() &&
                    std::chrono::steady_clock::now() < it->second->deadline &&
                    !stop_signal_.load(std::memory_order_relaxed);

                if (cqe->flags & IORING_CQE_F_NOTIF) {
                    stats_.add_zc_notification(thread_id_);
                } else if (cqe->res > 0) {
                    stats_.add_bytes(thread_id_, cqe->res, 1);
                    if (keep_sending) {
                        if (config_.zero_copy) {
                            engine.prep_send_zc(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx);
                        } else {
                            engine.prep_send_standard(ctx->fd, ctx->buf_ptr, ctx->length, ctx);
                        }
                    }
                } else if (cqe->res == -EAGAIN || cqe->res == -EWOULDBLOCK) {
                    if (keep_sending) {
                        if (config_.zero_copy) {
                            engine.prep_send_zc(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx);
                        } else {
                            engine.prep_send_standard(ctx->fd, ctx->buf_ptr, ctx->length, ctx);
                        }
                    }
                } else if (cqe->res < 0) {
                    stats_.add_error(thread_id_);
                }
            } else if (ctx->type == IOContext::RECV && use_multishot) {
                if (cqe->res > 0 && (cqe->flags & IORING_CQE_F_BUFFER)) {
                    uint16_t buf_id = static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
                    // Same GRO-coalescing correction as the client's send side:
                    // one completion can now carry many coalesced UDP datagrams.
                    uint64_t pkts = (config_.protocol == Protocol::UDP)
                        ? std::max<uint64_t>(1, static_cast<uint64_t>(cqe->res) / config_.udp_gso_size)
                        : 1;
                    stats_.add_bytes(thread_id_, cqe->res, pkts);
                    engine.return_recv_buffer(buf_id);
                }
                if (!(cqe->flags & IORING_CQE_F_MORE)) {
                    // The multishot op terminated. -ENOBUFS means the shared
                    // buffer ring was momentarily starved (all buffers
                    // outstanding, none returned yet) -- rearm to keep
                    // receiving rather than treating it as a real failure.
                    // Anything else for TCP means EOF/error on this specific
                    // connection; for UDP it's the shared listener itself,
                    // which stays alive (rearmed) until this worker stops.
                    if (cqe->res == -ENOBUFS) {
                        engine.prep_recv_multishot(ctx->fd, ctx, udp_fixed_file);
                    } else if (config_.protocol == Protocol::UDP) {
                        if (cqe->res < 0) stats_.add_error(thread_id_);
                        if (!stop_signal_.load(std::memory_order_relaxed)) {
                            engine.prep_recv_multishot(ctx->fd, ctx, udp_fixed_file);
                        }
                    } else {
                        if (cqe->res < 0) stats_.add_error(thread_id_);
                        auto it = sessions.find(ctx->fd);
                        if (it != sessions.end()) {
                            close(it->first);
                            sessions.erase(it);
                        }
                    }
                }
            } else if (ctx->type == IOContext::RECV) {
                if (cqe->res > 0) {
                    // Same GRO-coalescing correction as the client's send side:
                    // one completion can now carry many coalesced UDP datagrams.
                    uint64_t pkts = (config_.protocol == Protocol::UDP)
                        ? std::max<uint64_t>(1, static_cast<uint64_t>(cqe->res) / config_.udp_gso_size)
                        : 1;
                    stats_.add_bytes(thread_id_, cqe->res, pkts);
                    engine.prep_recv(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx, udp_fixed_file);
                } else if (cqe->res == -EAGAIN || cqe->res == -EWOULDBLOCK) {
                    engine.prep_recv(ctx->fd, ctx->buf_ptr, ctx->length, ctx->buf_idx, ctx, udp_fixed_file);
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

// ---------------------------------------------------------------------------
// Zero-copy receive (zcrx). io_uring's zcrx is a different shape than the
// standard recv path above: it needs its own ring (CQE32, since each zcrx
// completion carries an extra struct io_uring_zcrx_cqe alongside the normal
// io_uring_cqe), a registered NIC Rx queue via io_uring_register_ifq(), a
// registered mmap'd memory area the NIC DMAs directly into, and a refill
// queue the app uses to hand consumed buffers back to the kernel. None of
// that fits UringEngine's generic small-CQE model, so it gets its own
// self-contained ring and loop here, modeled directly on liburing's
// examples/zcrx.c reference server.
// ---------------------------------------------------------------------------
namespace {

constexpr uint64_t kZcrxReqTypeMask = 0x7;
constexpr uint64_t kZcrxReqAccept = 1;
constexpr uint64_t kZcrxReqRx = 2;

struct ZcrxConn {
    int fd;
};

struct ZcrxState {
    void* area_ptr = nullptr;
    void* ring_ptr = nullptr;
    size_t ring_size = 0;
    struct io_uring_zcrx_rq rq{};
    unsigned long area_token = 0;
    uint32_t zcrx_id = 0;
};

size_t zcrx_align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

bool zcrx_setup(struct io_uring* ring, const std::string& ifname, uint32_t queue_id,
                size_t area_size, uint32_t rq_entries, ZcrxState& st, size_t thread_id) {
    long page_size = sysconf(_SC_PAGESIZE);

    unsigned int ifindex = if_nametoindex(ifname.c_str());
    if (!ifindex) {
        std::cerr << "[Thread " << thread_id << "] zcrx: bad interface name '" << ifname << "'\n";
        return false;
    }

    st.ring_size = zcrx_align_up(rq_entries * sizeof(struct io_uring_zcrx_rqe) + page_size, page_size);
    st.ring_ptr = mmap(nullptr, st.ring_size, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (st.ring_ptr == MAP_FAILED) {
        std::cerr << "[Thread " << thread_id << "] zcrx: mmap(refill ring) failed: "
                  << std::strerror(errno) << "\n";
        st.ring_ptr = nullptr;
        return false;
    }

    st.area_ptr = mmap(nullptr, area_size, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (st.area_ptr == MAP_FAILED) {
        std::cerr << "[Thread " << thread_id << "] zcrx: mmap(area, " << area_size
                  << " bytes) failed: " << std::strerror(errno) << "\n";
        st.area_ptr = nullptr;
        munmap(st.ring_ptr, st.ring_size);
        st.ring_ptr = nullptr;
        return false;
    }

    struct io_uring_region_desc region_reg{};
    region_reg.size = st.ring_size;
    region_reg.user_addr = uring_ptr_to_u64(st.ring_ptr);
    region_reg.flags = IORING_MEM_REGION_TYPE_USER;

    struct io_uring_zcrx_area_reg area_reg{};
    area_reg.addr = uring_ptr_to_u64(st.area_ptr);
    area_reg.len = area_size;

    struct io_uring_zcrx_ifq_reg reg{};
    reg.if_idx = ifindex;
    reg.if_rxq = queue_id;
    reg.rq_entries = rq_entries;
    reg.area_ptr = uring_ptr_to_u64(&area_reg);
    reg.region_ptr = uring_ptr_to_u64(&region_reg);

    int ret = io_uring_register_ifq(ring, &reg);
    if (ret) {
        std::cerr << "[Thread " << thread_id << "] zcrx: io_uring_register_ifq(queue "
                  << queue_id << ") failed: " << ret << " (" << std::strerror(-ret) << ")\n";
        munmap(st.area_ptr, area_size);
        munmap(st.ring_ptr, st.ring_size);
        st.area_ptr = nullptr;
        st.ring_ptr = nullptr;
        return false;
    }

    st.rq.khead = reinterpret_cast<unsigned int*>(static_cast<char*>(st.ring_ptr) + reg.offsets.head);
    st.rq.ktail = reinterpret_cast<unsigned int*>(static_cast<char*>(st.ring_ptr) + reg.offsets.tail);
    st.rq.rqes = reinterpret_cast<struct io_uring_zcrx_rqe*>(static_cast<char*>(st.ring_ptr) + reg.offsets.rqes);
    st.rq.rq_tail = 0;
    st.rq.ring_entries = reg.rq_entries;

    st.zcrx_id = reg.zcrx_id;
    st.area_token = area_reg.rq_area_token;
    return true;
}

void zcrx_teardown(ZcrxState& st, size_t area_size) {
    if (st.area_ptr) munmap(st.area_ptr, area_size);
    if (st.ring_ptr) munmap(st.ring_ptr, st.ring_size);
}

unsigned zcrx_rq_nr_queued(const struct io_uring_zcrx_rq& rq) {
    return rq.rq_tail - io_uring_smp_load_acquire(rq.khead);
}

// Hands a consumed buffer back to the kernel via the refill queue so the NIC
// can DMA new data into it. Every successful recvzc CQE must be paired with
// exactly one of these, or the area's finite buffer pool starves.
void zcrx_return_buffer(ZcrxState& st, const struct io_uring_cqe* cqe) {
    if (zcrx_rq_nr_queued(st.rq) == st.rq.ring_entries) {
        return; // refill queue full; drop -- matches upstream reference behavior
    }
    const struct io_uring_zcrx_cqe* rcqe =
        reinterpret_cast<const struct io_uring_zcrx_cqe*>(cqe + 1);

    unsigned rq_mask = st.rq.ring_entries - 1;
    struct io_uring_zcrx_rqe* rqe = &st.rq.rqes[st.rq.rq_tail & rq_mask];
    rqe->off = (rcqe->off & ~IORING_ZCRX_AREA_MASK) | st.area_token;
    rqe->len = cqe->res;
    io_uring_smp_store_release(st.rq.ktail, ++st.rq.rq_tail);
}

void zcrx_submit_accept(struct io_uring* ring, int listen_fd) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
    sqe->user_data = kZcrxReqAccept;
}

void zcrx_submit_recv(struct io_uring* ring, ZcrxConn* conn, uint32_t zcrx_id) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    io_uring_prep_rw(IORING_OP_RECV_ZC, sqe, conn->fd, nullptr, 0, 0);
    sqe->ioprio |= IORING_RECV_MULTISHOT;
    sqe->zcrx_ifq_idx = zcrx_id;
    sqe->user_data = (reinterpret_cast<uint64_t>(conn)) | kZcrxReqRx;
}

} // namespace

void Worker::run_server_zcrx() {
    set_cpu_affinity();

    uint16_t listen_port = config_.port + static_cast<uint16_t>(thread_id_);
    int listen_fd = net_utils::create_listen_socket("0.0.0.0", listen_port, false, false, config_.udp_gso_size);
    if (listen_fd < 0) {
        std::cerr << "[Thread " << thread_id_ << "] zcrx: failed to create listen socket on port "
                  << listen_port << "\n";
        return;
    }

    struct io_uring_params params{};
    params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER |
                     IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL |
                     IORING_SETUP_CQE32 | IORING_SETUP_CQSIZE;
    params.cq_entries = config_.zcrx_cq_entries;

    struct io_uring ring;
    int ret = io_uring_queue_init_params(8, &ring, &params);
    if (ret < 0) {
        std::cerr << "[Thread " << thread_id_ << "] zcrx: ring init failed: "
                  << std::strerror(-ret) << "\n";
        close(listen_fd);
        return;
    }

    ZcrxState zst;
    uint32_t queue_id = config_.zcrx_base_queue + static_cast<uint32_t>(thread_id_);
    if (!zcrx_setup(&ring, config_.zcrx_ifname, queue_id, config_.zcrx_area_size,
                     config_.zcrx_rq_entries, zst, thread_id_)) {
        io_uring_queue_exit(&ring);
        close(listen_fd);
        return;
    }

    std::cout << "[Thread " << thread_id_ << "] zcrx ready: " << config_.zcrx_ifname
              << " queue " << queue_id << ", port " << listen_port << "\n";

    std::unordered_map<int, std::unique_ptr<ZcrxConn>> conns;
    zcrx_submit_accept(&ring, listen_fd);
    io_uring_submit(&ring);

    while (!stop_signal_.load(std::memory_order_relaxed)) {
        // A bounded wait, not io_uring_submit_and_wait()'s unconditional block:
        // the server now runs until SIGINT/SIGTERM (not -t), so it can sit
        // with nothing in flight but the standing accept SQE -- which only
        // ever completes on a *new* connection -- for arbitrarily long
        // stretches. Without a timeout, Ctrl+C wouldn't be noticed until the
        // next connection arrived, leaving the ifq registration (and the NIC
        // queue it owns) held by a process that won't exit.
        struct __kernel_timespec ts = {0, 200000000}; // 200ms
        struct io_uring_cqe* cqe_ptr = nullptr;
        int wret = io_uring_submit_and_wait_timeout(&ring, &cqe_ptr, 1, &ts, nullptr);
        if (wret < 0 && wret != -ETIME) {
            std::cerr << "[Thread " << thread_id_ << "] zcrx: submit_and_wait failed: "
                      << std::strerror(-wret) << "\n";
            break;
        }

        unsigned head;
        unsigned count = 0;
        struct io_uring_cqe* cqe;

        io_uring_for_each_cqe(&ring, head, cqe) {
            uint64_t tag = cqe->user_data & kZcrxReqTypeMask;

            if (tag == kZcrxReqAccept) {
                int client_fd = cqe->res;
                if (client_fd >= 0) {
                    net_utils::set_nonblocking(client_fd);
                    auto conn = std::make_unique<ZcrxConn>();
                    conn->fd = client_fd;
                    zcrx_submit_recv(&ring, conn.get(), zst.zcrx_id);
                    conns[client_fd] = std::move(conn);
                }
                if (!stop_signal_.load(std::memory_order_relaxed)) {
                    zcrx_submit_accept(&ring, listen_fd);
                }
            } else if (tag == kZcrxReqRx) {
                ZcrxConn* conn = reinterpret_cast<ZcrxConn*>(cqe->user_data & ~kZcrxReqTypeMask);

                if (!(cqe->flags & IORING_CQE_F_MORE)) {
                    // Multishot recvzc terminated: either ENOSPC (refill queue was
                    // starved -- rearm to keep the connection alive) or a real end
                    // (EOF/error) -- close and drop the connection either way.
                    if (cqe->res == -ENOSPC) {
                        zcrx_submit_recv(&ring, conn, zst.zcrx_id);
                    } else {
                        if (cqe->res < 0) {
                            stats_.add_error(thread_id_);
                        }
                        auto it = conns.find(conn->fd);
                        if (it != conns.end()) {
                            close(it->first);
                            conns.erase(it);
                        }
                    }
                } else if (cqe->res > 0) {
                    stats_.add_bytes(thread_id_, cqe->res, 1);
                    zcrx_return_buffer(zst, cqe);
                }
            }
            count++;
        }
        io_uring_cq_advance(&ring, count);
    }

    for (auto& kv : conns) {
        close(kv.first);
    }
    zcrx_teardown(zst, config_.zcrx_area_size);
    io_uring_queue_exit(&ring);
    close(listen_fd);
}
