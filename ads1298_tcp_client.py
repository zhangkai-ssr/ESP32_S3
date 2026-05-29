import socket
import sys
import time

HEADER = 0xAA
VERSION = 0x02
FOOTER = 0x55
DEVICE_COUNT = 2
PACKET_SIZE = 64
DEFAULT_PORT = 3333
REPORT_INTERVAL = 1.0
TIMESTAMP_WRAP_US = 1 << 32


def u16_le(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8)


def u32_le(data: bytes, offset: int) -> int:
    return (
        data[offset]
        | (data[offset + 1] << 8)
        | (data[offset + 2] << 16)
        | (data[offset + 3] << 24)
    )


def looks_like_packet(candidate: bytes) -> bool:
    return (
        len(candidate) == PACKET_SIZE
        and candidate[0] == HEADER
        and candidate[1] == VERSION
        and candidate[8] == DEVICE_COUNT
        and candidate[-1] == FOOTER
    )


def extract_packets(buffer: bytearray):
    packets = []
    skipped = 0
    while len(buffer) >= PACKET_SIZE:
        if looks_like_packet(buffer[:PACKET_SIZE]):
            packets.append(bytes(buffer[:PACKET_SIZE]))
            del buffer[:PACKET_SIZE]
            continue

        header_pos = buffer.find(bytes([HEADER]), 1)
        if header_pos == -1:
            skipped += len(buffer) - (PACKET_SIZE - 1)
            del buffer[: len(buffer) - (PACKET_SIZE - 1)]
            break

        skipped += header_pos
        del buffer[:header_pos]

    return packets, skipped


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else input("ESP32 IP: ").strip()
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.settimeout(2.0)

    print(f"Connected to {host}:{port}")
    print(
        f"Expecting {PACKET_SIZE}B packets, "
        f"header=0x{HEADER:02X}, version=0x{VERSION:02X}, footer=0x{FOOTER:02X}"
    )

    recv_buffer = bytearray()
    total_bytes = 0
    total_packets = 0
    bad_bytes = 0
    last_seq = None
    last_report = time.time()
    report_bytes = 0
    report_packets = 0
    report_gap_sum_ms = 0.0
    report_gap_max_ms = 0.0
    last_dev_ts = None
    last_host_ts = None

    try:
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                print("Server closed connection")
                break

            recv_time_us = time.perf_counter_ns() // 1000
            recv_buffer.extend(chunk)
            total_bytes += len(chunk)
            report_bytes += len(chunk)

            packets, skipped = extract_packets(recv_buffer)
            bad_bytes += skipped

            for packet in packets:
                total_packets += 1
                report_packets += 1

                seq = u16_le(packet, 2)
                if last_seq is not None and ((last_seq + 1) & 0xFFFF) != seq:
                    print(f"Sequence jump: prev={last_seq} now={seq}")
                last_seq = seq

                dev_ts_us = u32_le(packet, 4)
                if last_dev_ts is not None and last_host_ts is not None:
                    dev_delta_us = (dev_ts_us - last_dev_ts) & 0xFFFFFFFF
                    host_delta_us = recv_time_us - last_host_ts
                    gap_ms = (host_delta_us - dev_delta_us) / 1000.0
                    if gap_ms < 0:
                        gap_ms = 0.0
                    report_gap_sum_ms += gap_ms
                    if gap_ms > report_gap_max_ms:
                        report_gap_max_ms = gap_ms

                last_dev_ts = dev_ts_us
                last_host_ts = recv_time_us

            now = time.time()
            elapsed = now - last_report
            if elapsed >= REPORT_INTERVAL:
                kbps = report_bytes / elapsed / 1000.0
                mbps = report_bytes * 8 / elapsed / 1_000_000.0
                avg_gap_ms = report_gap_sum_ms / report_packets if report_packets else 0.0
                print(
                    f"rate={kbps:8.1f} KB/s  ({mbps:5.2f} Mbps)  "
                    f"packets/s={report_packets / elapsed:7.1f}  "
                    f"gap_avg={avg_gap_ms:7.3f} ms  gap_max={report_gap_max_ms:7.3f} ms  "
                    f"total_packets={total_packets:8d}  total_bytes={total_bytes:10d}  "
                    f"bad_bytes={bad_bytes}"
                )
                report_bytes = 0
                report_packets = 0
                report_gap_sum_ms = 0.0
                report_gap_max_ms = 0.0
                last_report = now

    except KeyboardInterrupt:
        print("\nStopped by user")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
