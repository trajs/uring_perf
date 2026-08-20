#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <getopt.h>
#include <cstdlib>

enum class Mode {
    SERVER,
    CLIENT
};

enum class Protocol {
    TCP,
    UDP
};

struct Config {
    Mode mode = Mode::CLIENT;
    Protocol protocol = Protocol::TCP;
    std::string server_ip = "127.0.0.1";
    uint16_t port = 5201;
    
    uint32_t duration_sec = 10;      // -t, duration in seconds
    uint32_t threads = 1;            // -P, number of parallel worker threads/processes
    uint32_t buf_size = 128 * 1024;  // -l, payload buffer size (default 128KB)
    uint32_t queue_depth = 256;      // -q, io_uring queue depth per thread
    uint32_t in_flight = 32;         // pipeline depth (in-flight SQEs per connection)
    
    bool zero_copy = false;          // -Z, enable IORING_OP_SEND_ZC / MSG_ZEROCOPY
    bool sqpoll = false;             // -S, enable IORING_SETUP_SQPOLL
    bool fixed_buffers = true;       // enable io_uring_register_buffers
    bool fixed_files = true;         // enable io_uring_register_files
    bool multishot_recv = true;      // enable server multishot recv
    bool multi_process = false;      // -M, spawn child processes (fork) per worker like iperf3
    
    uint32_t interval_sec = 1;       // -i, reporting interval in seconds
    std::vector<int> cpu_affinity;   // -A, CPU pinning list
    
    static void print_usage(const char* prog_name) {
        std::cout << "Usage: " << prog_name << " [options]\n\n"
                  << "Client/Server Options:\n"
                  << "  -s, --server              Run in server mode\n"
                  << "  -c, --client <host>       Run in client mode connecting to <host>\n"
                  << "  -p, --port <port>         Server port to listen on/connect to (default: 5201)\n"
                  << "  -u, --udp                 Use UDP protocol instead of TCP\n\n"
                  << "Performance & Zero-Copy Options:\n"
                  << "  -Z, --zerocopy            Enable zero-copy send (IORING_OP_SEND_ZC)\n"
                  << "  -S, --sqpoll              Enable Kernel Submission Queue Polling (IORING_SETUP_SQPOLL)\n"
                  << "  -P, --parallel <workers>  Number of parallel worker threads/processes (default: 1)\n"
                  << "  -M, --multi-process       Spawn isolated OS child processes (fork) per worker like iperf3\n"
                  << "  -l, --len <bytes>         Payload buffer size in bytes (default: 131072 for TCP)\n"
                  << "  -q, --queue-depth <depth> io_uring queue depth per ring (default: 256)\n"
                  << "  -F, --no-fixed-buf        Disable io_uring fixed registered buffers\n"
                  << "  -A, --affinity <cpus>     Comma-separated CPU cores for thread pinning (e.g. 0,1,2,3)\n\n"
                  << "Timing & Reporting Options:\n"
                  << "  -t, --time <seconds>      Duration in seconds for test transmission (default: 10)\n"
                  << "  -i, --interval <seconds>  Reporting interval in seconds (default: 1)\n"
                  << "  -h, --help                Display this help menu\n";
    }

    static Config parse_args(int argc, char* argv[]) {
        Config cfg;
        bool len_user_set = false;
        
        static struct option long_options[] = {
            {"server",        no_argument,       0, 's'},
            {"client",        required_argument, 0, 'c'},
            {"port",          required_argument, 0, 'p'},
            {"udp",           no_argument,       0, 'u'},
            {"zerocopy",      no_argument,       0, 'Z'},
            {"sqpoll",        no_argument,       0, 'S'},
            {"parallel",      required_argument, 0, 'P'},
            {"multi-process", no_argument,       0, 'M'},
            {"len",           required_argument, 0, 'l'},
            {"queue-depth",   required_argument, 0, 'q'},
            {"no-fixed-buf",  no_argument,       0, 'F'},
            {"affinity",      required_argument, 0, 'A'},
            {"time",          required_argument, 0, 't'},
            {"interval",      required_argument, 0, 'i'},
            {"help",          no_argument,       0, 'h'},
            {0, 0, 0, 0}
        };

        int opt;
        int option_index = 0;
        while ((opt = getopt_long(argc, argv, "sc:p:uZSP:Ml:q:FA:t:i:h", long_options, &option_index)) != -1) {
            switch (opt) {
                case 's':
                    cfg.mode = Mode::SERVER;
                    break;
                case 'c':
                    cfg.mode = Mode::CLIENT;
                    cfg.server_ip = optarg;
                    break;
                case 'p':
                    cfg.port = static_cast<uint16_t>(std::atoi(optarg));
                    break;
                case 'u':
                    cfg.protocol = Protocol::UDP;
                    break;
                case 'Z':
                    cfg.zero_copy = true;
                    break;
                case 'S':
                    cfg.sqpoll = true;
                    break;
                case 'P':
                    cfg.threads = std::max(1, std::atoi(optarg));
                    break;
                case 'M':
                    cfg.multi_process = true;
                    break;
                case 'l':
                    cfg.buf_size = std::max(64, std::atoi(optarg));
                    len_user_set = true;
                    break;
                case 'q':
                    cfg.queue_depth = std::max(16, std::atoi(optarg));
                    break;
                case 'F':
                    cfg.fixed_buffers = false;
                    break;
                case 'A': {
                    std::string cpus = optarg;
                    size_t pos = 0;
                    while ((pos = cpus.find(',')) != std::string::npos) {
                        cfg.cpu_affinity.push_back(std::atoi(cpus.substr(0, pos).c_str()));
                        cpus.erase(0, pos + 1);
                    }
                    if (!cpus.empty()) {
                        cfg.cpu_affinity.push_back(std::atoi(cpus.c_str()));
                    }
                    break;
                }
                case 't':
                    cfg.duration_sec = std::max(1, std::atoi(optarg));
                    break;
                case 'i':
                    cfg.interval_sec = std::max(1, std::atoi(optarg));
                    break;
                case 'h':
                default:
                    print_usage(argv[0]);
                    std::exit(opt == 'h' ? 0 : 1);
            }
        }

        if (!len_user_set && cfg.protocol == Protocol::UDP) {
            cfg.buf_size = 1472;
        }

        return cfg;
    }
};

#endif // CONFIG_HPP
