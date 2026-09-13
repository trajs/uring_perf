#ifndef WORKER_HPP
#define WORKER_HPP

#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>
#include "config.hpp"
#include "stats.hpp"
#include "uring_engine.hpp"

class Worker {
public:
    Worker(size_t thread_id, const Config& config, StatsCollector& stats, std::atomic<bool>& stop_signal);
    ~Worker();

    void start();
    void join();

private:
    void run_client();
    void run_client_reverse_recv(int sockfd);
    void run_client_reverse_recv_zcrx(int sockfd);
    void run_server();
    void run_server_zcrx();

    void set_cpu_affinity();

    size_t thread_id_;
    Config config_;
    StatsCollector& stats_;
    std::atomic<bool>& stop_signal_;
    std::thread thread_;

    // Per-thread buffer pool and context pool
    std::vector<void*> buffer_pool_;
    std::vector<std::unique_ptr<IOContext>> context_pool_;
};

#endif // WORKER_HPP
