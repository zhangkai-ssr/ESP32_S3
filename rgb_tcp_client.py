#!/usr/bin/env python3
"""
rgb_tcp_client.py – Receive 500 KB/s fake RGB stream and verify time sync quality.

Packet format (525 bytes):
    [0]      0xCC        header
    [1]      0x01        version
    [2]      0x30        type  (RGB test)
    [3..4]   uint16 LE   sequence number
    [5..12]  int64  LE   mcu_ts_us  (MCU time_sync_now_us, µs)
    [13]     uint8       n_pixels (170)
    [14..]   170×3 B     R G B fake data
    [last]   0x55        footer

Usage:
    python rgb_tcp_client.py <ESP32_IP> [port]
    python rgb_tcp_client.py 192.168.4.100
    python rgb_tcp_client.py 192.168.4.100 3335

Output:
    - Per-second: throughput, packet rate, host-MCU delay average and span
    - On Ctrl+C: linear regression summary + JSONL log saved
      Run time_sync_postprocess.py on the JSONL for full analysis.
"""

import json
import math
import socket
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

HEADER       = 0xCC
VERSION      = 0x01
TYPE_RGB     = 0x30
FOOTER       = 0x55
N_PIXELS     = 170
DEFAULT_PORT = 3335

# 1(hdr)+1(ver)+1(type)+2(seq)+8(ts64)+1(n) + 170*3(rgb) + 1(ftr)
PACKET_SIZE  = 15 + N_PIXELS * 3   # 525

REPORT_INTERVAL = 1.0   # seconds


def perf_us() -> int:
    return time.perf_counter_ns() // 1000


def parse_packet(pkt: bytes):
    """Return (seq, mcu_ts_us) or None if packet is malformed."""
    if (len(pkt) != PACKET_SIZE
            or pkt[0] != HEADER
            or pkt[1] != VERSION
            or pkt[2] != TYPE_RGB
            or pkt[13] != N_PIXELS
            or pkt[-1] != FOOTER):
        return None
    seq       = struct.unpack_from('<H', pkt, 3)[0]
    mcu_ts_us = struct.unpack_from('<q', pkt, 5)[0]
    return seq, mcu_ts_us


def extract_packets(buf: bytearray):
    """Scan buf for valid packets. Returns (list_of_packet_bytes, skipped_byte_count)."""
    packets, skipped = [], 0
    while len(buf) >= PACKET_SIZE:
        if (buf[0] == HEADER and buf[1] == VERSION and buf[2] == TYPE_RGB
                and buf[13] == N_PIXELS and buf[PACKET_SIZE - 1] == FOOTER):
            packets.append(bytes(buf[:PACKET_SIZE]))
            del buf[:PACKET_SIZE]
            continue
        pos = buf.find(bytes([HEADER]), 1)
        if pos == -1:
            skipped += len(buf) - (PACKET_SIZE - 1)
            del buf[:len(buf) - (PACKET_SIZE - 1)]
            break
        skipped += pos
        del buf[:pos]
    return packets, skipped


def linear_regression(xs: list, ys: list):
    """Fit y = slope*x + intercept. Returns (slope, intercept, residual_std)."""
    n = len(xs)
    if n < 2:
        return 1.0, 0.0, float('inf')
    sx  = sum(xs);  sy  = sum(ys)
    sxx = sum(x * x for x in xs)
    sxy = sum(x * y for x, y in zip(xs, ys))
    denom = n * sxx - sx * sx
    if abs(denom) < 1e-9:
        return 1.0, sy / n - sx / n, float('inf')
    slope     = (n * sxy - sx * sy) / denom
    intercept = (sy - slope * sx) / n
    residuals = [y - (slope * x + intercept) for x, y in zip(xs, ys)]
    std = math.sqrt(sum(r * r for r in residuals) / n)
    return slope, intercept, std


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else input("ESP32 IP: ").strip()
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.settimeout(3.0)

    episode_id = datetime.now().strftime("rgb_test_%Y%m%d_%H%M%S")
    print(f"Connected to {host}:{port}  episode={episode_id}")
    print(f"Expecting {PACKET_SIZE}B packets · {N_PIXELS} pixels/pkt · ~952 pkt/s → ~500 KB/s")
    print(f"delay = host_rx_us − mcu_ts_us  (should be stable ≈ UDP offset + TCP latency)")
    print(f"Press Ctrl+C to stop.\n")
    print(f"{'rate(KB/s)':>10}  {'pkt/s':>7}  {'delay_avg(ms)':>13}  "
          f"{'delay_span(ms)':>14}  {'total_pkt':>10}  {'seq_err':>7}  bad_B")
    print("-" * 85)

    recv_buf    = bytearray()
    records     = []           # list of (seq, mcu_ts_us, host_rx_us)
    last_seq    = None
    seq_errors  = 0
    bad_bytes   = 0
    total_pkts  = 0
    total_bytes = 0

    last_report  = time.monotonic()
    rep_bytes    = 0
    rep_pkts     = 0
    delay_sum    = 0.0
    delay_min    = float('inf')
    delay_max    = float('-inf')

    try:
        while True:
            try:
                chunk = sock.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                print("Server closed connection.")
                break

            host_rx_us = perf_us()
            recv_buf.extend(chunk)
            total_bytes += len(chunk)
            rep_bytes   += len(chunk)

            pkts, skipped = extract_packets(recv_buf)
            bad_bytes += skipped

            for pkt in pkts:
                result = parse_packet(pkt)
                if result is None:
                    bad_bytes += PACKET_SIZE
                    continue
                seq, mcu_ts_us = result
                total_pkts += 1
                rep_pkts   += 1

                if last_seq is not None:
                    expected = (last_seq + 1) & 0xFFFF
                    if seq != expected:
                        seq_errors += 1
                        drop = (seq - expected) & 0xFFFF
                        print(f"  SEQ gap: expected={expected} got={seq} drop={drop}")
                last_seq = seq

                records.append((seq, mcu_ts_us, host_rx_us))

                delay_us = host_rx_us - mcu_ts_us
                delay_sum += delay_us
                if delay_us < delay_min:
                    delay_min = delay_us
                if delay_us > delay_max:
                    delay_max = delay_us

            now = time.monotonic()
            if now - last_report >= REPORT_INTERVAL:
                elapsed = now - last_report
                kbps      = rep_bytes / elapsed / 1000.0
                pps       = rep_pkts  / elapsed
                avg_d_ms  = delay_sum / rep_pkts / 1000.0 if rep_pkts else 0.0
                span_d_ms = (delay_max - delay_min) / 1000.0 if rep_pkts > 1 else 0.0
                print(
                    f"{kbps:10.1f}  {pps:7.1f}  {avg_d_ms:+13.2f}  "
                    f"{span_d_ms:14.2f}  {total_pkts:10d}  {seq_errors:7d}  {bad_bytes}"
                )
                rep_bytes  = 0;  rep_pkts   = 0
                delay_sum  = 0.0
                delay_min  = float('inf');  delay_max = float('-inf')
                last_report = now

    except KeyboardInterrupt:
        print("\nStopped by user.")
    finally:
        sock.close()

    print(f"\nTotal: {total_pkts} packets · {seq_errors} seq errors · {bad_bytes} bad bytes")

    if len(records) < 10:
        print("Not enough packets for regression (<10). Exiting.")
        return

    xs = [float(r[1]) for r in records]   # mcu_ts_us
    ys = [float(r[2]) for r in records]   # host_rx_us

    slope, intercept, std = linear_regression(xs, ys)
    drift_ppm   = (slope - 1.0) * 1e6
    span_s      = (xs[-1] - xs[0]) / 1e6
    cal_first   = slope * xs[0]  + intercept
    cal_last    = slope * xs[-1] + intercept

    print(f"\n{'=' * 62}")
    print(f"  Time-sync linear regression:  host_time = slope × mcu_ts + intercept")
    print(f"  n             = {len(records)}")
    print(f"  slope         = {slope:.9f}   (drift = {drift_ppm:+.1f} ppm)")
    print(f"  intercept     = {intercept:+.0f} µs  ({intercept / 1e6:+.3f} s)")
    print(f"  residual_std  = {std / 1000:.3f} ms  ({std:.0f} µs)")
    print(f"  data_span     = {span_s:.1f} s")
    print(f"  cal_first     = {cal_first:.0f} µs")
    print(f"  cal_last      = {cal_last:.0f} µs")
    if std < 2000:
        grade = "[EXCELLENT]  residual < 2 ms"
    elif std < 10000:
        grade = "[GOOD]       residual < 10 ms"
    elif std < 50000:
        grade = "[OK]         residual < 50 ms — time sync usable"
    else:
        grade = "[FAIL]       residual > 50 ms — check time_sync_server.py"
    print(f"  {grade}")
    print(f"{'=' * 62}")

    log_path = Path(f"{episode_id}.jsonl")
    with open(log_path, "w", encoding="utf-8") as fh:
        for seq, mcu_ts_us, host_rx_us in records:
            fh.write(json.dumps({
                "episode_id":  episode_id,
                "device_id":   1,
                "stream":      "rgb",
                "seq":         seq,
                "mcu_ts_us":   mcu_ts_us,
                "host_rx_us":  host_rx_us,
                "valid":       True,
            }, ensure_ascii=False) + "\n")
    print(f"\n  JSONL log saved → {log_path}")
    print(f"  Full analysis:  python time_sync_postprocess.py {log_path}")


if __name__ == "__main__":
    main()
