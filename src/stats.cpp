#include "stats.hpp"

StatsCollector::StatsCollector(size_t num_threads, uint32_t interval_sec)
    : num_threads_(num_threads), interval_sec_(interval_sec) {
    
    size_t alloc_size = sizeof(ThreadStats) * num_threads_;
    void* ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        throw std::runtime_error("mmap failed for shared stats");
    }
    shared_stats_ = new(ptr) ThreadStats[num_threads_];
}

StatsCollector::~StatsCollector() {
    stop_reporting();
    if (shared_stats_) {
        munmap(shared_stats_, sizeof(ThreadStats) * num_threads_);
    }
}

void StatsCollector::add_bytes(size_t thread_id, uint64_t bytes, uint64_t packets) {
    if (thread_id < num_threads_ && shared_stats_) {
        shared_stats_[thread_id].bytes_transferred.fetch_add(bytes, std::memory_order_relaxed);
        shared_stats_[thread_id].packets_count.fetch_add(packets, std::memory_order_relaxed);
    }
}

void StatsCollector::add_zc_notification(size_t thread_id) {
    if (thread_id < num_threads_ && shared_stats_) {
        shared_stats_[thread_id].zc_notifications.fetch_add(1, std::memory_order_relaxed);
    }
}

void StatsCollector::add_error(size_t thread_id) {
    if (thread_id < num_threads_ && shared_stats_) {
        shared_stats_[thread_id].errors.fetch_add(1, std::memory_order_relaxed);
    }
}

void StatsCollector::start_reporting() {
    start_time_ = std::chrono::steady_clock::now();
    running_ = true;
    reporter_thread_ = std::thread(&StatsCollector::reporter_loop, this);
}

void StatsCollector::stop_reporting() {
    if (running_) {
        running_ = false;
        if (reporter_thread_.joinable()) {
            reporter_thread_.join();
        }
    }
}

void StatsCollector::reporter_loop() {
    uint64_t prev_bytes = 0;
    uint64_t prev_packets = 0;
    uint64_t prev_zc = 0;
    
    auto last_tick = start_time_;
    double interval_start_sec = 0.0;

    std::cout << "\n"
              << std::left << std::setw(18) << "[ Interval ]"
              << std::right << std::setw(14) << "Transfer"
              << std::setw(16) << "Bitrate"
              << std::setw(14) << "Packet Rate"
              << std::setw(14) << "Zero-Copy"
              << "\n"
              << std::string(76, '-') << "\n";

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_sec_));
        if (!running_) break;

        auto now = std::chrono::steady_clock::now();
        double delta_sec = std::chrono::duration<double>(now - last_tick).count();
        double current_total_sec = std::chrono::duration<double>(now - start_time_).count();

        uint64_t total_bytes = 0;
        uint64_t total_packets = 0;
        uint64_t total_zc = 0;

        for (size_t i = 0; i < num_threads_; ++i) {
            total_bytes += shared_stats_[i].bytes_transferred.load(std::memory_order_relaxed);
            total_packets += shared_stats_[i].packets_count.load(std::memory_order_relaxed);
            total_zc += shared_stats_[i].zc_notifications.load(std::memory_order_relaxed);
        }

        uint64_t interval_bytes = total_bytes - prev_bytes;
        uint64_t interval_packets = total_packets - prev_packets;
        uint64_t interval_zc = total_zc - prev_zc;

        double transfer_mbytes = static_cast<double>(interval_bytes) / (1024.0 * 1024.0);
        double transfer_gbytes = static_cast<double>(interval_bytes) / (1024.0 * 1024.0 * 1024.0);
        double gbits_per_sec = (static_cast<double>(interval_bytes) * 8.0) / (delta_sec * 1e9);
        double pps_k = (static_cast<double>(interval_packets) / delta_sec) / 1000.0;

        std::string interval_str = "[" + std::to_string(static_cast<int>(interval_start_sec)) + ".0-" +
                                   std::to_string(static_cast<int>(current_total_sec)) + ".0 sec]";

        std::cout << std::left << std::setw(18) << interval_str
                  << std::right << std::fixed << std::setprecision(2);
        
        if (transfer_gbytes >= 1.0) {
            std::cout << std::setw(10) << transfer_gbytes << " GB  ";
        } else {
            std::cout << std::setw(10) << transfer_mbytes << " MB  ";
        }

        std::cout << std::setw(11) << gbits_per_sec << " Gbits/s"
                  << std::setw(9) << pps_k << " Kpps"
                  << std::setw(14) << interval_zc
                  << std::endl;

        prev_bytes = total_bytes;
        prev_packets = total_packets;
        prev_zc = total_zc;
        last_tick = now;
        interval_start_sec = current_total_sec;
    }
}

void StatsCollector::print_summary(const std::string& role_label) {
    auto now = std::chrono::steady_clock::now();
    double total_sec = std::chrono::duration<double>(now - start_time_).count();
    if (total_sec <= 0.0) total_sec = 0.001;

    uint64_t total_bytes = 0;
    uint64_t total_packets = 0;
    uint64_t total_zc = 0;
    uint64_t total_errors = 0;

    for (size_t i = 0; i < num_threads_; ++i) {
        total_bytes += shared_stats_[i].bytes_transferred.load(std::memory_order_relaxed);
        total_packets += shared_stats_[i].packets_count.load(std::memory_order_relaxed);
        total_zc += shared_stats_[i].zc_notifications.load(std::memory_order_relaxed);
        total_errors += shared_stats_[i].errors.load(std::memory_order_relaxed);
    }

    double gbytes = static_cast<double>(total_bytes) / (1024.0 * 1024.0 * 1024.0);
    double gbits_per_sec = (static_cast<double>(total_bytes) * 8.0) / (total_sec * 1e9);
    double avg_pps_k = (static_cast<double>(total_packets) / total_sec) / 1000.0;

    std::cout << std::string(76, '=') << "\n"
              << " SUMMARY (" << role_label << ")\n"
              << std::string(76, '=') << "\n"
              << "  Test Duration       : " << std::fixed << std::setprecision(2) << total_sec << " sec\n"
              << "  Parallel Workers    : " << num_threads_ << "\n"
              << "  Total Data          : " << gbytes << " GBytes\n"
              << "  Average Throughput  : " << gbits_per_sec << " Gbits/sec\n"
              << "  Average Packet Rate : " << avg_pps_k << " Kpps\n"
              << "  Total Packets/IOs   : " << total_packets << "\n"
              << "  Zero-Copy Ops       : " << total_zc << "\n"
              << "  Total IO Errors     : " << total_errors << "\n"
              << std::string(76, '=') << "\n\n";
}
