import socket
import struct
import sys
import time

HEADER = 0xAA
FOOTER = 0x55
DEVICE_COUNT = 2
DEFAULT_PORT = 3333
REPORT_INTERVAL = 1.0

# Protocol versions
# v0x02: 64-byte packet, 32-bit timestamp at [4..7], device_count at [8]
# v0x03: 68-byte packet, 64-bit mcu_ts_us at [4..11], device_count at [12]
V2_VERSION   = 0x02
V2_SIZE      = 64
V2_DC_OFFSET = 8

V3_VERSION   = 0x03
V3_SIZE      = 68
V3_DC_OFFSET = 12

# Active version detected from first valid packet
_version    = V3_VERSION
_pkt_size   = V3_SIZE
_dc_offset  = V3_DC_OFFSET


def u16_le(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8)


def u32_le(data: bytes, offset: int) -> int:
    return (
        data[offset]
        | (data[offset + 1] << 8)
        | (data[offset + 2] << 16)
        | (data[offset + 3] << 24)
    )


def u64_le(data: bytes, offset: int) -> int:
    return struct.unpack_from('<Q', data, offset)[0]


def detect_version(candidate: bytes) -> bool:
    """Try to detect protocol version from first candidate. Returns True if recognised."""
    global _version, _pkt_size, _dc_offset
    for ver, sz, dc in (
        (V3_VERSION, V3_SIZE, V3_DC_OFFSET),
        (V2_VERSION, V2_SIZE, V2_DC_OFFSET),
    ):
        if (
            len(candidate) >= sz
            and candidate[0] == HEADER
            and candidate[1] == ver
            and candidate[sz - 1] == FOOTER
            and candidate[dc] == DEVICE_COUNT
        ):
            _version   = ver
            _pkt_size  = sz
            _dc_offset = dc
            print(f"Detected protocol v0x{ver:02X}: {sz}-byte packets")
            return True
    return False


def looks_like_packet(candidate: bytes) -> bool:
    return (
        len(candidate) == _pkt_size
        and candidate[0] == HEADER
        and candidate[1] == _version
        and candidate[_dc_offset] == DEVICE_COUNT
        and candidate[-1] == FOOTER
    )


def extract_packets(buffer: bytearray):
    packets = []
    skipped = 0
    while len(buffer) >= _pkt_size:
        if looks_like_packet(buffer[:_pkt_size]):
            packets.append(bytes(buffer[:_pkt_size]))
            del buffer[:_pkt_size]
            continue

        header_pos = buffer.find(bytes([HEADER]), 1)
        if header_pos == -1:
            skipped += len(buffer) - (_pkt_size - 1)
            del buffer[: len(buffer) - (_pkt_size - 1)]
            break

        skipped += header_pos
        del buffer[:header_pos]

    return packets, skipped


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else input("ESP32 IP: ").strip()
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.settimeout(5.0)

    print(f"Connected to {host}:{port}")
    print(f"Auto-detecting protocol version (v0x02=64B or v0x03=68B)...")

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

    consecutive_timeouts = 0
    try:
        while True:
            try:
                chunk = sock.recv(8192)
            except TimeoutError:
                consecutive_timeouts += 1
                print(f"recv timeout #{consecutive_timeouts}: no data from ESP32 (ADS1298 streaming?)")
                if consecutive_timeouts >= 3:
                    print("3 consecutive timeouts — giving up")
                    break
                continue
            consecutive_timeouts = 0
            if not chunk:
                print("Server closed connection")
                break

            recv_time_us = time.perf_counter_ns() // 1000
            recv_buffer.extend(chunk)
            total_bytes += len(chunk)
            report_bytes += len(chunk)

            # Version auto-detect on first usable chunk
            if total_packets == 0 and len(recv_buffer) >= V3_SIZE:
                detect_version(recv_buffer[:max(V3_SIZE, V2_SIZE)])

            packets, skipped = extract_packets(recv_buffer)
            bad_bytes += skipped

            for packet in packets:
                total_packets += 1
                report_packets += 1

                seq = u16_le(packet, 2)
                if last_seq is not None and ((last_seq + 1) & 0xFFFF) != seq:
                    print(f"Sequence jump: prev={last_seq} now={seq}")
                last_seq = seq

                if _version == V3_VERSION:
                    dev_ts_us = u64_le(packet, 4)
                    ts_wrap = 1 << 64
                else:
                    dev_ts_us = u32_le(packet, 4)
                    ts_wrap = 1 << 32

                if last_dev_ts is not None and last_host_ts is not None:
                    dev_delta_us = (dev_ts_us - last_dev_ts) % ts_wrap
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
