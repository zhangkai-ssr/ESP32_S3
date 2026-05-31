"""
imu_tcp_client.py – Receive and validate ICM-42670-P raw IMU stream from SHTB-V01 board.

Packet format (131 bytes for 10 samples/packet):
    [0]      0xAA          header
    [1]      0x01          version
    [2]      0x20          type (IMU)
    [3..4]   uint16 LE     sequence number
    [5..8]   uint32 LE     timestamp of first sample (µs)
    [9]      uint8         n_samples
    [10..]   n × 12 bytes  ax ay az gx gy gz  (int16 LE each)
    [last]   0x55          footer

Usage:
    python imu_tcp_client.py <ESP32_IP> [port]
    python imu_tcp_client.py 192.168.1.100
    python imu_tcp_client.py 192.168.1.100 3334
"""

import socket
import struct
import sys
import time

HEADER      = 0xAA
VERSION     = 0x01
TYPE_IMU    = 0x20
FOOTER      = 0x55
N_SAMPLES   = 10
DEFAULT_PORT = 3334

SAMPLE_BYTES = 12   # ax ay az gx gy gz × int16
PACKET_SIZE  = 1 + 1 + 1 + 2 + 4 + 1 + N_SAMPLES * SAMPLE_BYTES + 1  # 131

REPORT_INTERVAL = 1.0

# Physical conversion factors (raw counts → physical units)
ACCEL_FS_G   = 4.0      # ±4 g
GYRO_FS_DPS  = 500.0    # ±500 dps
INT16_MAX    = 32768.0

ACCEL_SCALE = ACCEL_FS_G  / INT16_MAX   # counts → g
GYRO_SCALE  = GYRO_FS_DPS / INT16_MAX   # counts → dps


def i16_le(data: bytes, off: int) -> int:
    val = data[off] | (data[off + 1] << 8)
    return val if val < 0x8000 else val - 0x10000


def u16_le(data: bytes, off: int) -> int:
    return data[off] | (data[off + 1] << 8)


def u32_le(data: bytes, off: int) -> int:
    return (data[off]
            | (data[off + 1] << 8)
            | (data[off + 2] << 16)
            | (data[off + 3] << 24))


def is_valid_packet(pkt: bytes) -> bool:
    return (len(pkt) == PACKET_SIZE
            and pkt[0] == HEADER
            and pkt[1] == VERSION
            and pkt[2] == TYPE_IMU
            and pkt[9] == N_SAMPLES
            and pkt[-1] == FOOTER)


def extract_packets(buf: bytearray):
    packets = []
    skipped = 0
    while len(buf) >= PACKET_SIZE:
        if is_valid_packet(buf[:PACKET_SIZE]):
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


def decode_samples(pkt: bytes):
    """Return list of (ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps) tuples."""
    samples = []
    off = 10  # first sample byte
    for _ in range(N_SAMPLES):
        ax = i16_le(pkt, off +  0) * ACCEL_SCALE
        ay = i16_le(pkt, off +  2) * ACCEL_SCALE
        az = i16_le(pkt, off +  4) * ACCEL_SCALE
        gx = i16_le(pkt, off +  6) * GYRO_SCALE
        gy = i16_le(pkt, off +  8) * GYRO_SCALE
        gz = i16_le(pkt, off + 10) * GYRO_SCALE
        samples.append((ax, ay, az, gx, gy, gz))
        off += SAMPLE_BYTES
    return samples


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else input("ESP32 IP: ").strip()
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.settimeout(2.0)

    print(f"Connected to {host}:{port}")
    print(f"Expecting {PACKET_SIZE}B packets, {N_SAMPLES} samples/pkt, "
          f"~{200 // N_SAMPLES} pkt/s")
    print(f"Accel scale: {ACCEL_SCALE:.6f} g/count  "
          f"Gyro scale: {GYRO_SCALE:.6f} dps/count")
    print()

    recv_buf  = bytearray()
    total_bytes   = 0
    total_packets = 0
    bad_bytes     = 0
    last_seq      = None
    seq_errors    = 0
    last_report   = time.time()
    report_bytes  = 0
    report_pkts   = 0
    last_dev_ts   = None
    last_host_ts  = None
    gap_sum_ms    = 0.0
    gap_max_ms    = 0.0

    try:
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                print("Server closed connection")
                break

            host_ts_us = time.perf_counter_ns() // 1000
            recv_buf.extend(chunk)
            total_bytes  += len(chunk)
            report_bytes += len(chunk)

            packets, skipped = extract_packets(recv_buf)
            bad_bytes += skipped

            for pkt in packets:
                total_packets += 1
                report_pkts   += 1

                seq    = u16_le(pkt, 3)
                dev_ts = u32_le(pkt, 5)

                if last_seq is not None:
                    expected = (last_seq + 1) & 0xFFFF
                    if seq != expected:
                        seq_errors += 1
                        print(f"  SEQ gap: expected={expected} got={seq} "
                              f"(drop={(seq - expected) & 0xFFFF})")
                last_seq = seq

                if last_dev_ts is not None and last_host_ts is not None:
                    dev_delta  = (dev_ts - last_dev_ts) & 0xFFFFFFFF
                    host_delta = host_ts_us - last_host_ts
                    gap_ms = (host_delta - dev_delta) / 1000.0
                    if gap_ms < 0:
                        gap_ms = 0.0
                    gap_sum_ms += gap_ms
                    if gap_ms > gap_max_ms:
                        gap_max_ms = gap_ms

                last_dev_ts  = dev_ts
                last_host_ts = host_ts_us

            now     = time.time()
            elapsed = now - last_report
            if elapsed >= REPORT_INTERVAL:
                kbps    = report_bytes / elapsed / 1000.0
                pps     = report_pkts  / elapsed
                sps     = pps * N_SAMPLES
                avg_gap = gap_sum_ms / report_pkts if report_pkts else 0.0
                print(
                    f"rate={kbps:6.1f} KB/s  pkt/s={pps:5.1f}  "
                    f"samples/s={sps:6.0f}  "
                    f"gap_avg={avg_gap:6.2f}ms  gap_max={gap_max_ms:6.2f}ms  "
                    f"total_pkt={total_packets:7d}  "
                    f"seq_err={seq_errors:4d}  bad_bytes={bad_bytes}"
                )
                report_bytes = 0
                report_pkts  = 0
                gap_sum_ms   = 0.0
                gap_max_ms   = 0.0
                last_report  = now

    except KeyboardInterrupt:
        print("\nStopped by user")
    finally:
        sock.close()
        print(f"\nSummary: total_packets={total_packets}  "
              f"seq_errors={seq_errors}  bad_bytes={bad_bytes}")


if __name__ == "__main__":
    main()
