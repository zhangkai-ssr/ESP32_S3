#!/usr/bin/env python3
"""
time_sync_postprocess.py – Post-processing linear regression for MCU→host time alignment.

For each (device_id, stream) pair found in a JSONL log, fits:
    host_time_us = slope * mcu_ts_us + intercept

Input JSONL format (one JSON object per line):
    {"episode_id":"...","device_id":1,"stream":"emg","seq":1024,
     "mcu_ts_us":183245678,"host_rx_us":987654321000,"valid":true}

Usage:
    python time_sync_postprocess.py  data.jsonl [data2.jsonl ...]
    python time_sync_postprocess.py  --help

Output:
    Per-(device_id, stream) regression report printed to stdout.
    Optionally writes calibrated JSONL alongside input files with
    an added "calibrated_time_us" field (--write-calibrated flag).
"""

import argparse
import json
import math
import sys
from collections import defaultdict
from pathlib import Path


# ---- Quality thresholds (from doc) ----
MIN_SAMPLES_FOR_REGRESSION = 10
MAX_RESIDUAL_STD_US        = 50_000   # 50 ms


def linear_regression(xs: list[float], ys: list[float]) -> tuple[float, float, float]:
    """
    Fit y = slope*x + intercept.
    Returns (slope, intercept, residual_std_us).
    """
    n = len(xs)
    if n < 2:
        return 1.0, 0.0, float("inf")

    sx  = sum(xs)
    sy  = sum(ys)
    sxx = sum(x * x for x in xs)
    sxy = sum(x * y for x, y in zip(xs, ys))

    denom = n * sxx - sx * sx
    if abs(denom) < 1e-9:
        return 1.0, float(sy / n - sx / n), float("inf")

    slope     = (n * sxy - sx * sy) / denom
    intercept = (sy - slope * sx) / n

    residuals = [y - (slope * x + intercept) for x, y in zip(xs, ys)]
    std = math.sqrt(sum(r * r for r in residuals) / n)

    return slope, intercept, std


def load_jsonl(path: Path) -> list[dict]:
    records = []
    with open(path, encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as exc:
                print(f"  [warn] {path}:{lineno}: {exc}", file=sys.stderr)
    return records


def analyse(records: list[dict]) -> None:
    # Group by (device_id, stream)
    groups: dict[tuple, list[dict]] = defaultdict(list)
    for r in records:
        if not r.get("valid", True):
            continue
        key = (r.get("device_id", "?"), r.get("stream", "?"))
        groups[key].append(r)

    if not groups:
        print("No valid records found.")
        return

    for (device_id, stream), recs in sorted(groups.items()):
        print(f"\n{'='*60}")
        print(f"  device_id={device_id}  stream={stream}  n={len(recs)}")
        print(f"{'='*60}")

        xs = [float(r["mcu_ts_us"])  for r in recs if "mcu_ts_us"  in r]
        ys = [float(r["host_rx_us"]) for r in recs if "host_rx_us" in r]

        if len(xs) != len(ys) or len(xs) == 0:
            print("  [skip] missing mcu_ts_us or host_rx_us fields")
            continue

        # Sanity checks
        if all(x == 0 for x in xs):
            print("  [skip] all mcu_ts_us == 0 — no valid MCU timestamps")
            continue

        if len(xs) < MIN_SAMPLES_FOR_REGRESSION:
            print(f"  [skip] only {len(xs)} samples < {MIN_SAMPLES_FOR_REGRESSION} minimum")
            continue

        # Monotonicity check
        jumps = sum(1 for a, b in zip(xs, xs[1:]) if b <= a)
        if jumps > 0:
            print(f"  [warn] {jumps} non-monotonic mcu_ts_us transitions")

        slope, intercept, std = linear_regression(xs, ys)

        print(f"  slope      = {slope:.9f}  (ideal 1.0, drift={(slope-1)*1e6:.1f} ppm)")
        print(f"  intercept  = {intercept:+.0f} µs")
        print(f"  residual_std = {std/1000:.3f} ms  ({std:.0f} µs)")

        if std > MAX_RESIDUAL_STD_US:
            print(f"  [FAIL] residual_std > {MAX_RESIDUAL_STD_US/1000:.0f} ms — regression unreliable")
        else:
            print(f"  [OK]   regression accepted")

        # Show first/last calibrated timestamps as a sanity check
        cal_first = slope * xs[0]  + intercept
        cal_last  = slope * xs[-1] + intercept
        span_s    = (xs[-1] - xs[0]) / 1e6
        print(f"  span       = {span_s:.1f} s")
        print(f"  calibrated_first = {cal_first:.0f} µs")
        print(f"  calibrated_last  = {cal_last:.0f} µs")


def write_calibrated(records: list[dict], out_path: Path) -> None:
    # Group by (device_id, stream) and build calibration maps
    groups: dict[tuple, list[dict]] = defaultdict(list)
    for r in records:
        key = (r.get("device_id", "?"), r.get("stream", "?"))
        groups[key].append(r)

    cal_params: dict[tuple, tuple[float, float]] = {}
    for key, recs in groups.items():
        xs = [float(r["mcu_ts_us"])  for r in recs if "mcu_ts_us"  in r and r.get("valid", True)]
        ys = [float(r["host_rx_us"]) for r in recs if "host_rx_us" in r and r.get("valid", True)]
        if len(xs) >= MIN_SAMPLES_FOR_REGRESSION:
            slope, intercept, std = linear_regression(xs, ys)
            if std <= MAX_RESIDUAL_STD_US:
                cal_params[key] = (slope, intercept)

    with open(out_path, "w", encoding="utf-8") as fh:
        for r in records:
            key = (r.get("device_id", "?"), r.get("stream", "?"))
            if key in cal_params and "mcu_ts_us" in r:
                slope, intercept = cal_params[key]
                r = dict(r)
                r["calibrated_time_us"] = int(slope * r["mcu_ts_us"] + intercept)
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")

    print(f"\nCalibrated JSONL written to: {out_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="+", help="Input JSONL log files")
    parser.add_argument("--write-calibrated", action="store_true",
                        help="Write a new JSONL alongside each input with calibrated_time_us added")
    args = parser.parse_args()

    for path_str in args.files:
        path = Path(path_str)
        if not path.exists():
            print(f"[error] file not found: {path}", file=sys.stderr)
            continue

        print(f"\nLoading: {path}")
        records = load_jsonl(path)
        print(f"  {len(records)} records loaded")
        analyse(records)

        if args.write_calibrated:
            out = path.with_stem(path.stem + "_calibrated")
            write_calibrated(records, out)


if __name__ == "__main__":
    main()
