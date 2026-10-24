# Assignment 3, Option 1: run bdpcc, CUBIC, Reno and BBR against the same
# destinations, with the algorithm order rotated each run (interleaved).
import argparse
import csv
import os
import random
import time

import iperf_client

ALGOS = ["bdpcc", "cubic", "reno", "bbr"]


def check_algos():
    with open("/proc/sys/net/ipv4/tcp_available_congestion_control") as f:
        available = f.read().split()
    missing = [a for a in ALGOS if a not in available]
    if missing:
        raise SystemExit(f"not available: {missing} (run: sudo bpf/load.sh)")


def pick_destinations(servers, n, seed, interval):
    # 1 s probe test; keep the first n servers that complete one
    pool = servers[:]
    random.Random(seed).shuffle(pool)
    dests = []
    for host, ports in pool[:3 * n]:
        if len(dests) == n:
            break
        try:
            port, _, _ = iperf_client.run_test(host, ports, 1, "cubic", interval)
            dests.append((host, ports))   # keep all listed ports for busy servers
            print(f"picked {host}:{port}")
        except Exception as e:
            print(f"skip {host}: {e}")
    if len(dests) < n:
        print(f"SHORTFALL: wanted {n} destinations, got {len(dests)}")
    return dests


def run_once(host, port, algo, duration, interval):
    # one retry after 10 s, since public iperf3 servers are often busy
    for attempt in range(2):
        if attempt:
            time.sleep(10)
        try:
            return iperf_client.run_test(host, port, duration, algo, interval)
        except Exception as e:
            print(f"  FAILED ({attempt + 1}/2): {e}")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-servers", default=iperf_client.SERVER_FILE)
    ap.add_argument("-n", type=int, default=5, help="number of destinations")
    ap.add_argument("-runs", type=int, default=4,
                    help="repetitions per algorithm (a multiple of 4 puts each algorithm in each position equally often)")
    ap.add_argument("-duration", type=int, default=10, help="seconds per test")
    ap.add_argument("-interval", type=float, default=0.2, help="sampling interval (s)")
    ap.add_argument("-seed", type=int, default=None)
    ap.add_argument("-out", default="results_a3")
    args = ap.parse_args()

    check_algos()
    seed = args.seed if args.seed is not None else random.randrange(2**32)
    os.makedirs(args.out, exist_ok=True)

    with open(args.servers, newline="") as f:
        servers = [(r["IP/HOST"], r["PORT"]) for r in csv.DictReader(f)]
    dests = pick_destinations(servers, args.n, seed, args.interval)

    with open(f"{args.out}/config.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["seed", "runs", "duration_s", "interval_s", "kernel"])
        w.writerow([seed, args.runs, args.duration, args.interval, os.uname().release])

    samples = []
    order_log = []
    seq = 0
    for run in range(1, args.runs + 1):
        k = (run - 1) % len(ALGOS)
        order = ALGOS[k:] + ALGOS[:k]
        print(f"\n=== run {run}/{args.runs}, order {order} ===")
        for algo in order:
            for host, port in dests:
                seq += 1
                print(f"[{seq}] run {run} {algo} -> {host}:{port}")
                result = run_once(host, port, algo, args.duration, args.interval)
                if result is None:
                    order_log.append((seq, run, algo, host, port, "", "failure"))
                    continue
                used_port, algo_used, s = result
                order_log.append((seq, run, algo, host, used_port, algo_used, "success"))
                samples += [(run, host, used_port, algo_used) + row for row in s]

    with open(f"{args.out}/test_order.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["seq", "run", "algo_requested", "host", "port", "algo_used", "status"])
        w.writerows(order_log)

    with open(f"{args.out}/samples.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["run", "host", "port", "algo", "t_s", "goodput_bps"]
                   + iperf_client.TCP_STATS_COLUMNS)
        w.writerows(samples)

    print(f"\nwrote {args.out}/samples.csv and {args.out}/test_order.csv")


if __name__ == "__main__":
    main()
