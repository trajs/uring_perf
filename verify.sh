#!/usr/bin/env bash
# uring_perf feature verification suite.
#
# Exercises every major feature end-to-end on the real 25GbE back-to-back rig
# (ns_client/eno1np0 <-> ns_server/eno2np1) and reports PASS/FAIL for each.
# Re-run this any time after touching the code, or after a reboot -- it will
# also detect and repair the rig itself (patched ice driver + netns + ntuple
# rules), since all of that is lost on reboot and has to be rebuilt by hand
# otherwise.
#
# Usage: ./verify.sh [-v]   (-v: keep full client/server logs on success too)

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$REPO/uring_perf"
SCRATCH="$(mktemp -d /tmp/uring_perf_verify.XXXXXX)"
VERBOSE=0
[[ "${1:-}" == "-v" ]] && VERBOSE=1

PASS=0
FAIL=0
FAILED_NAMES=()

# Fixed ports/queues tied to pre-existing ntuple hardware filters -- do not
# change without also re-pointing the filters (see ensure_rig below).
FWD_ZCRX_PORT=9100   # server-side filter: eno2np1 dst-port 9100 -> queue 1
REV_ZCRX_PORT=9300   # client-side filter: eno1np0 dst-port 9300 -> queue 1

log()  { echo "  $*"; }
step() { echo; echo "== $* =="; }

cleanup() { pkill -9 uring_perf 2>/dev/null; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Environment: patched ice driver + netns rig + ntuple filters. All of this
# is lost across a reboot; detect and rebuild it if needed.
# ---------------------------------------------------------------------------
ensure_rig() {
  step "Checking rig (netns, driver, hardware filters)"

  local need_rebuild=0
  if ! ip netns list 2>/dev/null | grep -q '^ns_client'; then need_rebuild=1; fi
  if ! ip netns list 2>/dev/null | grep -q '^ns_server'; then need_rebuild=1; fi
  if [[ $need_rebuild -eq 0 ]] && ! ip netns exec ns_client ping -c1 -W1 192.168.25.2 >/dev/null 2>&1; then
    need_rebuild=1
  fi

  if [[ $need_rebuild -eq 1 ]]; then
    log "netns rig missing or unreachable -- rebuilding"
    ip netns del ns_client 2>/dev/null
    ip netns del ns_server 2>/dev/null

    log "reloading patched ice driver (this NEEDS /root/ice72 built for the running kernel)"
    rmmod irdma 2>/dev/null
    rmmod idpf 2>/dev/null
    rmmod ice 2>/dev/null
    rmmod libeth_xdp 2>/dev/null
    rmmod libeth 2>/dev/null
    insmod /root/ice72/drivers/net/ethernet/intel/libeth/libeth.ko || { echo "FATAL: could not load patched libeth"; exit 1; }
    insmod /root/ice72/drivers/net/ethernet/intel/libeth/libeth_xdp.ko || { echo "FATAL: could not load patched libeth_xdp"; exit 1; }
    insmod /root/ice72/drivers/net/ethernet/intel/ice/ice.ko || { echo "FATAL: could not load patched ice"; exit 1; }
    sleep 3

    log "recreating ns_client/ns_server"
    bash /root/recreate_netns.sh >/dev/null
  else
    log "netns rig OK (ns_client <-> ns_server reachable)"
  fi

  # zcrx prerequisites: tcp-data-split + the two hardware ntuple filters.
  # Cheap to re-assert every run regardless of whether a rebuild happened.
  ip netns exec ns_server ethtool -G eno2np1 tcp-data-split on >/dev/null 2>&1
  ip netns exec ns_client ethtool -G eno1np0 tcp-data-split on >/dev/null 2>&1

  if ! ip netns exec ns_server ethtool -n eno2np1 2>/dev/null | grep -q "Dest port: $FWD_ZCRX_PORT"; then
    log "adding server-side ntuple filter (port $FWD_ZCRX_PORT -> queue 1)"
    ip netns exec ns_server ethtool -N eno2np1 flow-type tcp4 dst-port $FWD_ZCRX_PORT action 1 >/dev/null
  fi
  if ! ip netns exec ns_client ethtool -n eno1np0 2>/dev/null | grep -q "Dest port: $REV_ZCRX_PORT"; then
    log "adding client-side ntuple filter (port $REV_ZCRX_PORT -> queue 1)"
    ip netns exec ns_client ethtool -N eno1np0 flow-type tcp4 dst-port $REV_ZCRX_PORT action 1 >/dev/null
  fi
  log "zcrx prerequisites OK (tcp-data-split on, hardware filters in place)"
}

# ---------------------------------------------------------------------------
build() {
  step "Building"
  ( cd "$REPO" && make clean >/dev/null 2>&1 && make ) > "$SCRATCH/build.log" 2>&1
  if [[ $? -ne 0 ]] || [[ ! -x "$BIN" ]]; then
    echo "  FAIL: build did not produce $BIN"
    cat "$SCRATCH/build.log"
    exit 1
  fi
  if grep -qiE "warning|error" "$SCRATCH/build.log"; then
    log "build produced warnings (see $SCRATCH/build.log):"
    grep -iE "warning|error" "$SCRATCH/build.log" | sed 's/^/    /'
  else
    log "clean build, no warnings"
  fi
}

# ---------------------------------------------------------------------------
# One test = start a server, run a client against it, tear down, check both
# sides' own summaries for zero IO errors and nonzero throughput.
#
# args: name  server_args  client_args  [min_gbps]
# (server_args/client_args are the uring_perf flags only -- "ip netns exec"
# and the binary path are added here. Passed unquoted-then-word-split into
# an array rather than eval'd, so $! reliably ends up as the real uring_perf
# PID -- eval/string-command backgrounding was leaving $! pointing at a
# wrapper shell instead, so SIGINT below never actually reached the process.)
# ---------------------------------------------------------------------------
run_test() {
  local name="$1" server_args="$2" client_args="$3" min_gbps="${4:-0.05}"
  local slog="$SCRATCH/${name}.server.log" clog="$SCRATCH/${name}.client.log"

  pkill -9 uring_perf 2>/dev/null; sleep 1

  local -a sarr=( $server_args )
  ip netns exec ns_server "$BIN" "${sarr[@]}" > "$slog" 2>&1 &
  local spid=$!
  sleep 1.5

  local -a carr=( $client_args )
  ip netns exec ns_client "$BIN" "${carr[@]}" > "$clog" 2>&1

  sleep 1
  # -M forks independent child processes that don't receive the parent's
  # signals automatically -- interrupt those directly too, not just $spid.
  kill -INT "$spid" 2>/dev/null
  pkill -INT -P "$spid" 2>/dev/null
  sleep 2
  kill -0 "$spid" 2>/dev/null && kill -9 "$spid" 2>/dev/null
  pkill -9 uring_perf 2>/dev/null

  local c_err s_err gbps
  # -M mode prints one summary block per forked process into the same log,
  # so sum every occurrence rather than taking the last one.
  c_err=$(grep -oP 'Total IO Errors\s*:\s*\K[0-9]+' "$clog" | awk '{s+=$1} END{print s+0}')
  s_err=$(grep -oP 'Total IO Errors\s*:\s*\K[0-9]+' "$slog" | awk '{s+=$1} END{print s+0}')
  gbps=$(grep -oP 'Average Throughput\s*:\s*\K[0-9.]+' "$clog" | awk '{s+=$1} END{print s+0}')

  local ok=1
  [[ "$c_err" == "0" ]] || ok=0
  [[ "$s_err" == "0" ]] || ok=0
  awk -v g="$gbps" -v m="$min_gbps" 'BEGIN{exit !(g>=m)}' || ok=0

  if [[ $ok -eq 1 ]]; then
    PASS=$((PASS+1))
    printf "  PASS  %-28s %6s Gbit/s   client_errors=%-3s server_errors=%s\n" "$name" "$gbps" "$c_err" "$s_err"
    [[ $VERBOSE -eq 1 ]] && { echo "    --- client log ---"; sed 's/^/    /' "$clog"; }
  else
    FAIL=$((FAIL+1))
    FAILED_NAMES+=("$name")
    printf "  FAIL  %-28s %6s Gbit/s   client_errors=%-3s server_errors=%s\n" "$name" "$gbps" "$c_err" "$s_err"
    echo "    --- client log ($clog) ---"
    tail -20 "$clog" | sed 's/^/    /'
    echo "    --- server log ($slog) ---"
    tail -20 "$slog" | sed 's/^/    /'
  fi
}

# ---------------------------------------------------------------------------
ensure_rig
build

step "Running feature tests"

run_test "plain-tcp" \
  "-s -p 15001 -t 6" \
  "-c 192.168.25.2 -p 15001 -t 3" \
  10

run_test "zero-copy-send" \
  "-s -p 15002 -t 6" \
  "-c 192.168.25.2 -p 15002 -Z -t 3" \
  10

run_test "zcrx-forward" \
  "-s -p $FWD_ZCRX_PORT -Y --zcrx-if eno2np1 --zcrx-queue 1 -t 6" \
  "-c 192.168.25.2 -p $FWD_ZCRX_PORT -Y -Z -t 3" \
  5

run_test "reverse-mode" \
  "-s -p 15003 -t 6" \
  "-c 192.168.25.2 -p 15003 -R -t 3" \
  10

run_test "reverse-zcrx" \
  "-s -p $REV_ZCRX_PORT -Z -t 6" \
  "-c 192.168.25.2 -p $REV_ZCRX_PORT -R -Y --zcrx-if eno1np0 --zcrx-queue 1 -t 3" \
  5

run_test "udp-gso-gro" \
  "-s -u -p 15004 -t 6" \
  "-c 192.168.25.2 -u -p 15004 -t 3" \
  5

run_test "multi-thread" \
  "-s -p 15005 -P 4 -t 6" \
  "-c 192.168.25.2 -p 15005 -P 4 -t 3" \
  10

run_test "multi-process" \
  "-s -p 15006 -P 4 -M -t 6" \
  "-c 192.168.25.2 -p 15006 -P 4 -M -t 3" \
  10

run_test "sqpoll" \
  "-s -p 15007 -S -t 6" \
  "-c 192.168.25.2 -p 15007 -S -Z -t 3" \
  5

# ---------------------------------------------------------------------------
step "Summary"
echo "  $PASS passed, $FAIL failed"
if [[ $FAIL -gt 0 ]]; then
  echo "  Failed: ${FAILED_NAMES[*]}"
  echo "  Full logs in $SCRATCH"
  exit 1
fi
echo "  Logs kept in $SCRATCH"
exit 0
