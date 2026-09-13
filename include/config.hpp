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
    uint32_t udp_gso_size = 1472;    // UDP GSO/GRO wire-segment size (MTU-safe payload)

    bool zero_copy = false;          // -Z, enable IORING_OP_SEND_ZC / MSG_ZEROCOPY
    bool sqpoll = false;             // -S, enable IORING_SETUP_SQPOLL
    bool fixed_buffers = true;       // enable io_uring_register_buffers
    bool fixed_files = true;         // enable io_uring_register_files
    bool multishot_recv = true;      // use multishot IORING_OP_RECV + a provided
                                      // buffer ring instead of per-completion
                                      // resubmitted single-shot recv (--no-multishot-recv)
    bool multi_process = false;      // -M, spawn child processes (fork) per worker like iperf3

    // Reverse mode, like iperf3 -R: the client still initiates the TCP
    // connection, but a tiny handshake sent right after connect() tells the
    // server to send and the client to receive instead. TCP only; meaningful
    // only on the client (the server learns direction per-connection from
    // the handshake, not from its own CLI flags).
    bool reverse = false;            // -R, --reverse

    // Zero-copy RECEIVE (zcrx). Unlike -Z (send), this needs a dedicated NIC Rx
    // queue per worker thread -- steered there by an ethtool ntuple rule set up
    // outside this tool -- so each zcrx worker registers against a distinct
    // queue (zcrx_base_queue + thread_id).
    //
    // Normal mode: the server receives, so it listens on a distinct port
    // (cfg.port + thread_id) and the client just needs -Y too, to connect to
    // that matching per-thread port (no ifq registration on the client side).
    //
    // -R/--reverse mode: the client receives instead, on a single
    // already-connected socket, so it's the CLIENT that registers the ifq
    // here -- against --zcrx-if on the client's own NIC -- and binds its
    // local (source) port to cfg.port + thread_id before connecting, so an
    // ntuple filter on the client's NIC can steer each thread's inbound flow
    // by that same port.
    bool use_zcrx = false;           // -Y, --zcrx
    std::string zcrx_ifname;         // --zcrx-if <ifname>: server for normal mode, client for -R
    uint32_t zcrx_base_queue = 0;    // --zcrx-queue <id>, thread i uses queue base+i
    size_t zcrx_area_size = 256 * 1024 * 1024;  // mmap'd zero-copy buffer pool, per thread
    uint32_t zcrx_rq_entries = 8192; // refill queue depth
    uint32_t zcrx_cq_entries = 8192; // completion queue depth (CQE32 ring)

    uint32_t interval_sec = 1;       // -i, reporting interval in seconds
    std::vector<int> cpu_affinity;   // -A, CPU pinning list

    static void print_usage(const char* prog_name) {
        std::cout << "Usage: " << prog_name << " [options]\n\n"
                  << "Client/Server Options:\n"
                  << "  -s, --server              Run in server mode\n"
                  << "  -c, --client <host>       Run in client mode connecting to <host>\n"
                  << "  -p, --port <port>         Server port to listen on/connect to (default: 5201)\n"
                  << "  -u, --udp                 Use UDP protocol instead of TCP\n"
                  << "  -R, --reverse             Reverse mode: server sends, client receives (TCP only,\n"
                  << "                            client-side flag, like iperf3 -R). Combine with -Y so\n"
                  << "                            the client receives via zero-copy too (needs --zcrx-if\n"
                  << "                            on the client in this mode, not the server).\n\n"
                  << "Performance & Zero-Copy Options:\n"
                  << "  -Z, --zerocopy            Enable zero-copy send (IORING_OP_SEND_ZC)\n"
                  << "  -Y, --zcrx                Enable zero-copy receive (IORING_OP_RECV_ZC); TCP only.\n"
                  << "                            Normal mode: requires --zcrx-if on the server; each\n"
                  << "                            worker thread i uses port+i and NIC queue\n"
                  << "                            --zcrx-queue+i. With -R: requires --zcrx-if on the\n"
                  << "                            client instead, whose thread i binds local port+i.\n"
                  << "      --zcrx-if <ifname>    NIC interface to register zcrx against (server for\n"
                  << "                            normal mode, client for -R/--reverse)\n"
                  << "      --zcrx-queue <id>     Base hardware Rx queue index for thread 0 (default: 0)\n"
                  << "      --no-multishot-recv   Disable multishot IORING_OP_RECV (provided buffer ring);\n"
                  << "                            fall back to per-completion resubmitted recv\n"
                  << "  -S, --sqpoll              Enable Kernel Submission Queue Polling (IORING_SETUP_SQPOLL)\n"
                  << "  -P, --parallel <workers>  Number of parallel worker threads/processes (default: 1)\n"
                  << "  -M, --multi-process       Spawn isolated OS child processes (fork) per worker like iperf3\n"
                  << "  -l, --len <bytes>         Payload buffer size in bytes (default: 131072 for TCP)\n"
                  << "  -q, --queue-depth <depth> io_uring queue depth per ring (default: 256)\n"
                  << "  -F, --no-fixed-buf        Disable io_uring fixed registered buffers\n"
                  << "  -A, --affinity <cpus>     Comma-separated CPU cores for thread pinning (e.g. 0,1,2,3)\n\n"
                  << "Timing & Reporting Options:\n"
                  << "  -t, --time <seconds>      Client-side test duration in seconds (default: 10).\n"
                  << "                            Ignored by the server, which listens until interrupted\n"
                  << "                            (Ctrl+C), like `iperf3 -s`.\n"
                  << "  -i, --interval <seconds>  Reporting interval in seconds (default: 1)\n"
                  << "  -h, --help                Display this help menu\n";
    }

    static Config parse_args(int argc, char* argv[]) {
        Config cfg;
        bool len_user_set = false;

        enum { OPT_ZCRX_IF = 1000, OPT_ZCRX_QUEUE, OPT_NO_MULTISHOT_RECV };

        static struct option long_options[] = {
            {"server",        no_argument,       0, 's'},
            {"client",        required_argument, 0, 'c'},
            {"port",          required_argument, 0, 'p'},
            {"udp",           no_argument,       0, 'u'},
            {"reverse",       no_argument,       0, 'R'},
            {"zerocopy",      no_argument,       0, 'Z'},
            {"zcrx",          no_argument,       0, 'Y'},
            {"zcrx-if",       required_argument, 0, OPT_ZCRX_IF},
            {"zcrx-queue",    required_argument, 0, OPT_ZCRX_QUEUE},
            {"no-multishot-recv", no_argument,   0, OPT_NO_MULTISHOT_RECV},
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
        while ((opt = getopt_long(argc, argv, "sc:p:uRZYSP:Ml:q:FA:t:i:h", long_options, &option_index)) != -1) {
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
                case 'R':
                    cfg.reverse = true;
                    break;
                case 'Z':
                    cfg.zero_copy = true;
                    break;
                case 'Y':
                    cfg.use_zcrx = true;
                    break;
                case OPT_ZCRX_IF:
                    cfg.zcrx_ifname = optarg;
                    break;
                case OPT_ZCRX_QUEUE:
                    cfg.zcrx_base_queue = static_cast<uint32_t>(std::atoi(optarg));
                    break;
                case OPT_NO_MULTISHOT_RECV:
                    cfg.multishot_recv = false;
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
            // Default to a GSO/GRO batch buffer (40 x 1472B segments, ~57.5KB,
            // safely under the kernel's UDP_MAX_SEGMENTS=64 and the 65507B UDP
            // payload ceiling) instead of a single MTU-sized datagram. UDP_SEGMENT
            // (send) / UDP_GRO (recv) then let one io_uring op move ~40 datagrams
            // through the network stack instead of one -- see net_utils.cpp.
            cfg.buf_size = 40 * cfg.udp_gso_size;
        }

        // -Y needs --zcrx-if on whichever side actually registers an ifq:
        // the server in normal mode, or the client in -R/--reverse mode
        // (where the client is the one receiving).
        bool client_zcrx_recv = cfg.use_zcrx && cfg.reverse && cfg.mode == Mode::CLIENT;
        if (cfg.use_zcrx && cfg.zcrx_ifname.empty() &&
            (cfg.mode == Mode::SERVER || client_zcrx_recv)) {
            std::cerr << "[Error] -Y/--zcrx requires --zcrx-if <ifname> "
                         "(on the server for normal mode, on the client for -R/--reverse)\n";
            std::exit(1);
        }
        if (cfg.use_zcrx && cfg.protocol == Protocol::UDP) {
            std::cerr << "[Error] -Y/--zcrx only supports TCP\n";
            std::exit(1);
        }
        if (cfg.reverse && cfg.protocol == Protocol::UDP) {
            std::cerr << "[Error] -R/--reverse only supports TCP\n";
            std::exit(1);
        }
        if (cfg.reverse && cfg.mode == Mode::SERVER) {
            std::cerr << "[Warning] -R/--reverse is a client-side flag (like iperf3 -R); "
                         "ignoring it on the server, which learns direction per-connection\n";
            cfg.reverse = false;
        }

        return cfg;
    }
};

#endif // CONFIG_HPP
