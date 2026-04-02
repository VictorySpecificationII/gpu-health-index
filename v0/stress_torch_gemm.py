#!/usr/bin/env python3
import argparse
import time
from datetime import datetime, timezone

import torch


def utc_now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def main():
    p = argparse.ArgumentParser(description="Simple PyTorch GEMM stress loop with throughput logging")
    p.add_argument("--seconds", type=int, default=300, help="Duration of stress (default: 300s)")
    p.add_argument("--n", type=int, default=8192, help="Matrix size N for NxN GEMM (default: 8192)")
    p.add_argument("--dtype", type=str, default="bf16", choices=["fp16", "bf16", "fp32"], help="dtype")
    p.add_argument("--out", type=str, default="data/gemm_throughput.csv", help="Output CSV with it/s")
    args = p.parse_args()

    device = "cuda"
    torch.backends.cuda.matmul.allow_tf32 = True

    if args.dtype == "fp16":
        dtype = torch.float16
    elif args.dtype == "bf16":
        dtype = torch.bfloat16
    else:
        dtype = torch.float32

    n = args.n
    a = torch.randn((n, n), device=device, dtype=dtype)
    b = torch.randn((n, n), device=device, dtype=dtype)

    # Warmup
    for _ in range(10):
        c = a @ b
    torch.cuda.synchronize()

    t_end = time.time() + args.seconds
    iters = 0

    # Write a simple CSV: timestamp, iters_per_sec (sampled each second)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("ts_utc,iters_per_sec\n")
        last_t = time.time()
        last_iters = 0

        while time.time() < t_end:
            c = a @ b
            iters += 1

            # sample ~1Hz
            now = time.time()
            if now - last_t >= 1.0:
                torch.cuda.synchronize()
                dt = now - last_t
                ips = (iters - last_iters) / dt
                f.write(f"{utc_now_iso()},{ips:.4f}\n")
                f.flush()
                last_t = now
                last_iters = iters

    torch.cuda.synchronize()
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
