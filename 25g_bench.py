import subprocess
import time
import re

def run_cmd(cmd):
    p = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return p.stdout, p.stderr

def cleanup():
    run_cmd("pkill -9 uring_perf; pkill -9 iperf3")
    time.sleep(1)

def run_iperf3(threads):
    cleanup()
    subprocess.Popen(["ip", "netns", "exec", "ns_server", "iperf3", "-s", "-p", "5201"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    
    stdout, stderr = run_cmd(f"ip netns exec ns_client iperf3 -c 192.168.25.2 -p 5201 -P {threads} -t 5")
    cleanup()
    
    matches = re.findall(r"(\d+(?:\.\d+)?)\s+(Gbits|Mbits)/sec\s+receiver", stdout)
    if not matches:
        matches = re.findall(r"\[SUM\].*?(\d+(?:\.\d+)?)\s+(Gbits|Mbits)/sec\s+receiver", stdout, re.DOTALL)
    
    if matches:
        val, unit = matches[-1]
        gbps = float(val) if unit == "Gbits" else float(val) / 1000.0
        return gbps
    return 0.0

def run_uring_perf(threads):
    cleanup()
    m_flag = "-M" if threads > 1 else ""
    subprocess.Popen(f"ip netns exec ns_server /root/uring_perf/uring_perf -s -p 9000 -P {threads} {m_flag} -t 15", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    
    stdout, stderr = run_cmd(f"ip netns exec ns_client /root/uring_perf/uring_perf -c 192.168.25.2 -p 9000 -P {threads} {m_flag} -Z -t 5")
    cleanup()
    
    match = re.search(r"Average Throughput\s+:\s+(\d+(?:\.\d+)?)\s+Gbits/sec", stdout)
    if match:
        return float(match.group(1))
    return 0.0

def main():
    threads_list = [1, 2, 4, 8]
    results = []
    
    print("==========================================================================")
    print(" 25GbE Hardware Benchmark (Intel E810-XXV SFP28 Back-to-Back Ns)")
    print("==========================================================================")
    
    for t in threads_list:
        print(f"\n---> Benchmarking {t} Stream(s) over 25G NIC...")
        iperf_gbps = run_iperf3(t)
        print(f"     iperf3     (-P {t}) : {iperf_gbps:.2f} Gbits/sec")
        
        uring_gbps = run_uring_perf(t)
        print(f"     uring_perf (-P {t}) : {uring_gbps:.2f} Gbits/sec")
        
        diff_pct = ((uring_gbps - iperf_gbps) / iperf_gbps) * 100.0 if iperf_gbps > 0 else 0.0
        results.append({
            "threads": t,
            "iperf3_gbps": iperf_gbps,
            "uring_perf_gbps": uring_gbps,
            "diff_pct": diff_pct
        })
        
    print("\n\n==========================================================================")
    print(f"{'Streams (-P)':<12} | {'iperf3 Bitrate':<18} | {'uring_perf Bitrate':<20} | {'Performance Delta':<18}")
    print("==========================================================================")
    for r in results:
        delta_str = f"+{r['diff_pct']:.1f}%" if r['diff_pct'] >= 0 else f"{r['diff_pct']:.1f}%"
        print(f"-P {r['threads']:<9} | {r['iperf3_gbps']:>6.2f} Gbits/sec     | {r['uring_perf_gbps']:>8.2f} Gbits/sec       | {delta_str:>16}")
    print("==========================================================================")

if __name__ == "__main__":
    main()
