import socket
import random
import struct
import json
import time
import os
import csv
import argparse

#states
TEST_START = 1
TEST_RUNNING = 2
RESULT_REQUEST = 3
TEST_END = 4
STREAM_BEGIN = 5
STREAM_RUNNING = 6
STREAM_END = 7
ALL_STREAMS_END = 8
PARAM_EXCHANGE = 9
CREATE_STREAMS = 10
SERVER_TERMINATE = 11
CLIENT_TERMINATE = 12
EXCHANGE_RESULTS = 13
DISPLAY_RESULTS = 14
IPERF_START = 15
IPERF_DONE = 16
ACCESS_DENIED = (-1)
SERVER_ERROR = (-2)

SERVER_FILE = "listed_iperf3_servers.csv"


def make_cookie():
    chars = "abcdefghijklmnopqrstuvwxyz234567"
    return ("".join(random.choice(chars) for _ in range(36)) + "\0").encode()


def recv_n_bytes(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("server closed connection early")
        buf += chunk
    return buf


def recv_state(sock):
    return int.from_bytes(recv_n_bytes(sock, 1), "big", signed=True)


def parse_ports(port_field):
    if "-" in port_field:
        lo, hi = port_field.split("-")
        return list(range(int(lo), int(hi) + 1))
    return [int(port_field)]


def open_control_connection(host, port, timeout=10):
    cookie = make_cookie()
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    try:
        sock.sendall(cookie)
        state = recv_state(sock)
        if state == ACCESS_DENIED:
            raise ConnectionRefusedError("access denied")
        if state == SERVER_ERROR:
            raise ConnectionError("server error")
        if state != PARAM_EXCHANGE:
            raise ConnectionError(f"unexpected state {state}")
        return sock, cookie
    except Exception:
        sock.close()
        raise


def connect_any_port(host, port_field, timeout=10):
    for port in parse_ports(port_field):
        try:
            sock, cookie = open_control_connection(host, port, timeout)
            return sock, cookie, port
        except Exception as e:
            print(f"{host}:{port} failed: {e}")
    raise ConnectionError(f"all ports failed for {host}")


def send_json(sock, obj):
    data = json.dumps(obj).encode()
    sock.sendall(struct.pack("!I", len(data)) + data)


def recv_json(sock):
    (length,) = struct.unpack("!I", recv_n_bytes(sock, 4))
    return json.loads(recv_n_bytes(sock, length).decode())


def send_parameters(sock, duration, blksize=128 * 1024):
    params = {
        "tcp": True,
        "omit": 0,
        "time": duration,
        "num": 0,
        "blockcount": 0,
        "parallel": 1,
        "len": blksize,
        "pacing_timer": 1000,
        "client_version": "3.16",
    }
    send_json(sock, params)

    state = recv_state(sock)
    if state == ACCESS_DENIED:
        raise ConnectionRefusedError("access denied")
    if state == SERVER_ERROR:
        i_errno, errno = struct.unpack("!ii", recv_n_bytes(sock, 8))
        raise ConnectionError(f"i_errno={i_errno}, errno={errno}")
    if state != CREATE_STREAMS:
        raise ConnectionError(f"unexpected state {state}")


def open_data_connection(ctrl, cookie, congestion=None, timeout=10):
    server_ip, server_port = ctrl.getpeername()[:2]

    family = socket.AF_INET6 if ":" in server_ip else socket.AF_INET
    data = socket.socket(family, socket.SOCK_STREAM)
    try:
        data.settimeout(timeout)
        if congestion:
            data.setsockopt(socket.IPPROTO_TCP, socket.TCP_CONGESTION,
                            congestion.encode())
        data.connect((server_ip, server_port))
        data.sendall(cookie)
        return data
    except Exception:
        data.close()
        raise


def wait_for_test_running(ctrl):
    for expected in (TEST_START, TEST_RUNNING):
        state = recv_state(ctrl)
        if state != expected:
            raise ConnectionError(f"expected state {expected}, got {state}")


def get_congestion(sock):
    raw = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_CONGESTION, 16)
    return raw.split(b"\0", 1)[0].decode()


def send_data(data, duration, blksize=128 * 1024, interval=0.2):
    buf = memoryview(os.urandom(blksize))
    bytes_sent = 0
    samples = []

    start = time.monotonic()
    end = start + duration

    prev_time = start
    prev_acked = get_bytes_acked(data)
    next_sample = start + interval

    while True:
        now = time.monotonic()
        if now >= next_sample or now >= end:
            acked = get_bytes_acked(data)
            dt = now - prev_time
            if dt > 0:
                samples.append((now - start, (acked - prev_acked) * 8 / dt))
                prev_time = now
                prev_acked = acked
            next_sample = max(next_sample + interval, now + interval / 2)
        if now >= end:
            break
        try:
            n = data.send(buf)
        except socket.timeout:
            raise ConnectionError("send timed out")
        except (BrokenPipeError, ConnectionResetError) as e:
            raise ConnectionError(f"dropped data connection: {e}")
        bytes_sent += n

    return bytes_sent, time.monotonic() - start, samples


def get_total_retrans(sock):
    info = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 200)
    return struct.unpack_from("I", info, 100)[0]


def build_results(bytes_sent, retransmits, congestion, elapsed):
    return {
        "cpu_util_total": 0.0, "cpu_util_user": 0.0, "cpu_util_system": 0.0,
        "sender_has_retransmits": 1,
        "congestion_used": congestion,
        "streams": [{
            "id": 1, "bytes": bytes_sent, "retransmits": retransmits,
            "jitter": 0, "errors": 0, "omitted_errors": 0,
            "packets": 0, "omitted_packets": 0,
            "start_time": 0, "end_time": elapsed,
        }],
    }

def finish_test(ctrl, results):
    ctrl.sendall(struct.pack("!b", TEST_END))
    if recv_state(ctrl) != EXCHANGE_RESULTS:
        raise ConnectionError("expected EXCHANGE_RESULTS")
    send_json(ctrl, results)
    server_results = recv_json(ctrl)
    if recv_state(ctrl) != DISPLAY_RESULTS:
        raise ConnectionError("expected DISPLAY_RESULTS")
    ctrl.sendall(struct.pack("!b", IPERF_DONE))
    return server_results


def run_test(host, port_field, duration=10, congestion=None):
    ctrl = None
    data = None
    try:
        ctrl, cookie, port = connect_any_port(host, port_field)
        send_parameters(ctrl, duration)
        data = open_data_connection(ctrl, cookie, congestion)
        algo = get_congestion(data)
        wait_for_test_running(ctrl)

        bytes_sent, elapsed, samples = send_data(data, duration)
        retrans = get_total_retrans(data)

        results = build_results(bytes_sent, retrans, algo, elapsed)
        server_results = finish_test(ctrl, results)
        server_bytes = server_results["streams"][0].get("bytes")
        return port, algo, samples
    finally:
        if data:
            data.close()
        if ctrl:
            ctrl.close()


def run_destinations(servers, n, duration, congestion, seed=None, max_attempts=None):
    rng = random.Random(seed)
    pool = servers[:]
    rng.shuffle(pool)
    if max_attempts is None:
        max_attempts = 3 * n

    log = []
    successes = 0
    all_samples = []
    for host, ports in pool[:max_attempts]:
        if successes == n:
            break
        print(f"[{successes+1}/{n}] trying {host} ports {ports}")
        try:
            port, algo, samples = run_test(host, ports, duration, congestion)
            all_samples += [(host, port, algo, t, g) for t, g in samples]
            log.append((host, port, "success"))
            successes += 1
        except Exception as e:
            print(f"  FAILED: {e}")
            log.append((host, ports, "failure"))

    if successes < n:
        print(f"SHORTFALL: wanted {n} destinations, got {successes} "
              f"after {len(log)} attempts")
    return log, all_samples


def get_bytes_acked(sock):
    info = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 200)
    return struct.unpack_from("=Q", info, 120)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=10)
    ap.add_argument("-duration", type=int, default=10)
    ap.add_argument("-congestion", default=None)
    ap.add_argument("-max-attempts", type=int, default=None)
    ap.add_argument("-seed", type=int, default=None)
    args = ap.parse_args()

    with open(SERVER_FILE, newline="") as f:
        servers = [(r["IP/HOST"], r["PORT"]) for r in csv.DictReader(f)]
    
    log, samples = run_destinations(servers, args.n, args.duration, args.congestion, args.seed, args.max_attempts)

    with open("selected_destinations.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["address", "port", "status"])
        w.writerows(log)

    with open("samples.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["host", "port", "algo", "t_s", "goodput_bps"])
        w.writerows(samples)


if __name__ == "__main__":
    main()