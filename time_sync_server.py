#!/usr/bin/env python3
"""
time_sync_server.py – Host-side UDP time synchronisation server for ESP32 wristbands.

Protocol  UDP port 3332
  Request  (18 B):  magic[4]='TSYN' ver=0x01 type=0x01 device_id rsvd seq[2LE] t1_us[8LE]
  Response (34 B):  magic[4]='TSYN' ver=0x01 type=0x02 device_id rsvd seq[2LE]
                    t1_us[8LE] t2_us[8LE] t3_us[8LE]

Where:
  T1 = MCU clock when request was sent     (echoed from request)
  T2 = host monotonic clock at rx          (time.perf_counter_ns // 1000)
  T3 = host monotonic clock just before tx

The MCU computes:
  rtt_us    = (T4 - T1) - (T3 - T2)
  offset_us = ((T2 - T1) + (T3 - T4)) / 2

Usage:
    python time_sync_server.py [bind_ip]
    python time_sync_server.py 0.0.0.0          # all interfaces (default)
    python time_sync_server.py 192.168.4.1       # SoftAP gateway interface
"""

import socket
import struct
import sys
import time

UDP_PORT = 3332
MAGIC    = b'TSYN'
VERSION  = 0x01
REQ_TYPE = 0x01
RSP_TYPE = 0x02
REQ_SIZE = 18
RSP_SIZE = 34


def perf_us() -> int:
    """Return monotonic host time in microseconds."""
    return time.perf_counter_ns() // 1000


def main() -> None:
    bind_ip = sys.argv[1] if len(sys.argv) > 1 else "0.0.0.0"

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind_ip, UDP_PORT))
    print(f"[time_sync_server] listening on {bind_ip}:{UDP_PORT}")

    counts: dict[int, int] = {}   # device_id → round count

    while True:
        try:
            data, addr = sock.recvfrom(64)
            t2_us = perf_us()

            # ---- Validate request ----
            if len(data) != REQ_SIZE:
                continue
            if data[:4] != MAGIC:
                continue
            if data[4] != VERSION or data[5] != REQ_TYPE:
                continue

            device_id = data[6]
            seq       = struct.unpack_from('<H', data, 8)[0]
            t1_us     = struct.unpack_from('<q', data, 10)[0]

            # ---- Build response (T3 captured just before sendto) ----
            hdr = struct.pack('<4sBBBBH', MAGIC, VERSION, RSP_TYPE, device_id, 0, seq)
            payload_partial = struct.pack('<qq', t1_us, t2_us)

            t3_us = perf_us()
            rsp = hdr + payload_partial + struct.pack('<q', t3_us)

            sock.sendto(rsp, addr)

            counts[device_id] = counts.get(device_id, 0) + 1
            offset_est   = t2_us - t1_us   # ≈ host_clock - mcu_clock (µs)
            server_proc  = t3_us - t2_us   # 服务端处理耗时 (µs)
            print(
                f"  dev={device_id}  seq={seq:5d}  "
                f"offset~={offset_est/1e6:+.3f} s  "
                f"srv_proc={server_proc} µs  "
                f"from {addr[0]}  (round #{counts[device_id]})"
            )

        except KeyboardInterrupt:
            print("\n[time_sync_server] stopped")
            break
        except Exception as exc:
            print(f"[time_sync_server] error: {exc}")

    sock.close()


if __name__ == "__main__":
    main()
