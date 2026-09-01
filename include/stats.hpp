#ifndef STATS_HPP
#define STATS_HPP

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <string>
#include <sys/mman.h>

struct ThreadStats {
    alignas(64) std::atomic<uint64_t> bytes_transferred{0};
    alignas(64) std::atomic<uint64_t> packets_count{0};
    alignas(64) std::atomic<uint64_t> zc_notifications{0};
    alignas(64) std::atomic<uint64_t> errors{0};
};

class StatsCollector {
public:
    StatsCollector(size_t num_threads, uint32_t interval_sec);
    ~StatsCollector();

    void add_bytes(size_t thread_id, uint64_t bytes, uint64_t packets = 1);
    void add_zc_notification(size_t thread_id);
    void add_error(size_t thread_id);

    void start_reporting();
    void stop_reporting();
    // role_label appears verbatim as "SUMMARY (<role_label>)" -- e.g.
    // "Sender/Client", "Receiver/Client" (reverse mode), "Receiver/Server".
    void print_summary(const std::string& role_label);

private:
    void reporter_loop();

    size_t num_threads_;
    uint32_t interval_sec_;
    ThreadStats* shared_stats_{nullptr};
    
    std::atomic<bool> running_{false};
    std::thread reporter_thread_;
    std::chrono::steady_clock::time_point start_time_;
};

#endif // STATS_HPP
