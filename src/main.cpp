#include <iostream>
#include <vector>
#include <memory>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "config.hpp"
#include "stats.hpp"
#include "worker.hpp"

static std::atomic<bool> g_stop_signal{false};

void signal_handler(int signum) {
    (void)signum;
    g_stop_signal.store(true, std::memory_order_relaxed);
}

void raise_memlock_limit() {
    struct rlimit rl;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
            rl.rlim_cur = RLIM_INFINITY;
            rl.rlim_max = RLIM_INFINITY;
            setrlimit(RLIMIT_MEMLOCK, &rl);
        }
    }
}

void print_banner(const Config& cfg) {
    std::cout << "========================================================================\n"
              << " uring_perf - High-Performance io_uring Zero-Copy Traffic Generator\n"
              << "========================================================================\n"
              << " Mode             : " << (cfg.mode == Mode::CLIENT ? "Client (Sender)" : "Server (Receiver)") << "\n"
              << " Protocol         : " << (cfg.protocol == Protocol::TCP ? "TCP" : "UDP") << "\n"
              << " Target           : " << cfg.server_ip << ":" << cfg.port << "\n"
              << " Parallel Workers : " << cfg.threads << " (" << (cfg.multi_process ? "Multi-Process Fork" : "Multi-Threaded") << ")\n"
              << " Payload Size     : " << cfg.buf_size << " bytes (" << (cfg.buf_size / 1024) << " KB)\n"
              << " io_uring Queue   : " << cfg.queue_depth << " SQEs per worker\n"
              << " Zero-Copy Send   : " << (cfg.zero_copy ? "ENABLED (IORING_OP_SEND_ZC)" : "DISABLED") << "\n"
              << " Kernel SQPOLL    : " << (cfg.sqpoll ? "ENABLED (IORING_SETUP_SQPOLL)" : "DISABLED") << "\n"
              << " Fixed Buffers    : " << (cfg.fixed_buffers ? "ENABLED" : "DISABLED") << "\n"
              << " Test Duration    : " << cfg.duration_sec << " seconds\n"
              << "========================================================================\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    raise_memlock_limit();
    Config cfg = Config::parse_args(argc, argv);

    if (cfg.multi_process && cfg.threads > 1) {
        // Multi-process execution mode with mmap shared stats
        StatsCollector shared_stats(cfg.threads, cfg.interval_sec);

        std::vector<pid_t> pids;
        pids.reserve(cfg.threads);

        for (size_t i = 0; i < cfg.threads; ++i) {
            pid_t pid = fork();
            if (pid == 0) {
                // Child process gets its own independent RLIMIT_MEMLOCK & descriptor table
                raise_memlock_limit();
                Config child_cfg = cfg;
                child_cfg.threads = 1;

                Worker worker(i, child_cfg, shared_stats, g_stop_signal);
                worker.start();

                auto start_time = std::chrono::steady_clock::now();
                while (!g_stop_signal.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    auto elapsed = std::chrono::steady_clock::now() - start_time;
                    if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= cfg.duration_sec) {
                        g_stop_signal.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
                worker.join();
                std::exit(0);
            } else if (pid > 0) {
                pids.push_back(pid);
            }
        }

        // Parent process prints banner and manages timing & combined stats
        print_banner(cfg);
        shared_stats.start_reporting();

        auto start_time = std::chrono::steady_clock::now();
        while (!g_stop_signal.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= cfg.duration_sec) {
                g_stop_signal.store(true, std::memory_order_relaxed);
                break;
            }
        }

        for (pid_t pid : pids) {
            int status;
            waitpid(pid, &status, 0);
        }

        shared_stats.stop_reporting();
        shared_stats.print_summary(cfg.mode == Mode::CLIENT);
        return 0;
    }

    // Default multi-threaded execution mode
    print_banner(cfg);
    StatsCollector stats(cfg.threads, cfg.interval_sec);
    stats.start_reporting();

    std::vector<std::unique_ptr<Worker>> workers;
    workers.reserve(cfg.threads);

    for (size_t i = 0; i < cfg.threads; ++i) {
        workers.push_back(std::make_unique<Worker>(i, cfg, stats, g_stop_signal));
    }

    for (auto& worker : workers) {
        worker->start();
    }

    auto start_time = std::chrono::steady_clock::now();
    while (!g_stop_signal.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= cfg.duration_sec) {
            g_stop_signal.store(true, std::memory_order_relaxed);
            break;
        }
    }

    for (auto& worker : workers) {
        worker->join();
    }

    stats.stop_reporting();
    stats.print_summary(cfg.mode == Mode::CLIENT);

    return 0;
}
