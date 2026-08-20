import subprocess
import time
import re
import json

def run_cmd(cmd):
    p = subprocess.run(["bash", "-c", cmd], capture_output=True, text=True)
    return p.stdout, p.stderr

def cleanup():
    run_cmd("pkill -9 uring_perf; pkill -9 iperf3")
    time.sleep(1)

def run_iperf3(threads):
    cleanup()
    # Start iperf3 server
    subprocess.Popen(["bash", "-c", "third_party/iperf/src/iperf3 -s -p 9002 > /dev/null 2>&1"])
    time.sleep(1)
    
    # Run iperf3 client
    stdout, stderr = run_cmd(f"third_party/iperf/src/iperf3 -c 127.0.0.1 -p 9002 -P {threads} -t 5")
    cleanup()
    
    # Parse receiver bitrate
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
    # Start uring_perf server
    m_flag = "-M" if threads > 1 else ""
    subprocess.Popen(["bash", "-c", f"./uring_perf -s -p 9000 -P {threads} {m_flag} -t 15 > /dev/null 2>&1"])
    time.sleep(1)
    
    # Run uring_perf client with zero-copy
    stdout, stderr = run_cmd(f"./uring_perf -c 127.0.0.1 -p 9000 -P {threads} {m_flag} -Z -t 5")
    cleanup()
    
    # Parse summary average throughput
    match = re.search(r"Average Throughput\s+:\s+(\d+(?:\.\d+)?)\s+Gbits/sec", stdout)
    if match:
        return float(match.group(1))
    return 0.0

def main():
    threads_list = [1, 2, 4, 8]
    results = []
    
    print("=========================================================")
    print(" Running Empirical Side-by-Side Benchmark Suite")
    print("=========================================================")
    
    for t in threads_list:
        print(f"\n---> Benchmarking {t} Worker Stream(s)...")
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
