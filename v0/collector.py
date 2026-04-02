#!/usr/bin/env python3
import argparse
import csv
import os
import signal
import sys
import time
from datetime import datetime, timezone

from pynvml import (
    nvmlInit,
    nvmlShutdown,
    nvmlDeviceGetCount,
    nvmlDeviceGetHandleByIndex,
    nvmlDeviceGetName,
    nvmlDeviceGetUUID,
    nvmlDeviceGetTemperature,
    nvmlDeviceGetUtilizationRates,
    nvmlDeviceGetPowerUsage,
    nvmlDeviceGetEnforcedPowerLimit,
    nvmlDeviceGetClockInfo,
    nvmlDeviceGetMemoryInfo,
    nvmlDeviceGetPerformanceState,
    NVMLError,
    NVML_TEMPERATURE_GPU,
    NVML_CLOCK_SM,
    NVML_CLOCK_MEM,
)

STOP = False


def _sig_handler(signum, frame):
    global STOP
    STOP = True


def safe_call(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def utc_now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def main():
    p = argparse.ArgumentParser(description="NVML GPU telemetry collector -> CSV")
    p.add_argument("--out", required=True, help="Output CSV path (e.g., data/run1.csv)")
    p.add_argument("--interval", type=float, default=1.0, help="Sampling interval seconds (default: 1.0)")
    p.add_argument("--gpu", type=int, default=0, help="GPU index (default: 0)")
    args = p.parse_args()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

    signal.signal(signal.SIGINT, _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    nvmlInit()
    try:
        count = nvmlDeviceGetCount()
        if args.gpu < 0 or args.gpu >= count:
            raise SystemExit(f"--gpu {args.gpu} out of range (0..{count-1})")

        h = nvmlDeviceGetHandleByIndex(args.gpu)

        gpu_name = safe_call(lambda: nvmlDeviceGetName(h).decode("utf-8", "ignore"), "")
        gpu_uuid = safe_call(lambda: nvmlDeviceGetUUID(h).decode("utf-8", "ignore"), "")

        fieldnames = [
            "ts_utc",
            "gpu_index",
            "gpu_name",
            "gpu_uuid",
            "temp_c",
            "power_w",
            "power_limit_w",
            "util_gpu_pct",
            "util_mem_pct",
            "sm_clock_mhz",
            "mem_clock_mhz",
            "mem_used_mb",
            "mem_total_mb",
            "pstate",
        ]

        with open(args.out, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fieldnames)
            w.writeheader()

            start = time.time()
            n = 0
            while not STOP:
                ts = utc_now_iso()

                temp_c = safe_call(lambda: nvmlDeviceGetTemperature(h, NVML_TEMPERATURE_GPU))
                util = safe_call(lambda: nvmlDeviceGetUtilizationRates(h))
                util_gpu = getattr(util, "gpu", None) if util is not None else None
                util_mem = getattr(util, "memory", None) if util is not None else None

                # NVML power values are in milliwatts
                power_mw = safe_call(lambda: nvmlDeviceGetPowerUsage(h))
                power_w = (power_mw / 1000.0) if power_mw is not None else None

                power_limit_mw = safe_call(lambda: nvmlDeviceGetEnforcedPowerLimit(h))
                power_limit_w = (power_limit_mw / 1000.0) if power_limit_mw is not None else None

                sm_clock = safe_call(lambda: nvmlDeviceGetClockInfo(h, NVML_CLOCK_SM))
                mem_clock = safe_call(lambda: nvmlDeviceGetClockInfo(h, NVML_CLOCK_MEM))

                mem = safe_call(lambda: nvmlDeviceGetMemoryInfo(h))
                mem_used_mb = (mem.used / (1024 * 1024)) if mem is not None else None
                mem_total_mb = (mem.total / (1024 * 1024)) if mem is not None else None

                pstate = safe_call(lambda: int(nvmlDeviceGetPerformanceState(h)))

                w.writerow(
                    dict(
                        ts_utc=ts,
                        gpu_index=args.gpu,
                        gpu_name=gpu_name,
                        gpu_uuid=gpu_uuid,
                        temp_c=temp_c,
                        power_w=power_w,
                        power_limit_w=power_limit_w,
                        util_gpu_pct=util_gpu,
                        util_mem_pct=util_mem,
                        sm_clock_mhz=sm_clock,
                        mem_clock_mhz=mem_clock,
                        mem_used_mb=mem_used_mb,
                        mem_total_mb=mem_total_mb,
                        pstate=pstate,
                    )
                )
                f.flush()

                n += 1
                # keep interval stable-ish even if loop takes time
                elapsed = time.time() - start
                target = n * args.interval
                sleep_for = max(0.0, target - elapsed)
                time.sleep(sleep_for)

        print(f"Wrote {args.out}")

    finally:
        try:
            nvmlShutdown()
        except NVMLError:
            pass


if __name__ == "__main__":
    main()
