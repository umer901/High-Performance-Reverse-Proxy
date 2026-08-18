#!/usr/bin/env python3

import argparse
import json
import shutil
import subprocess
import time
from pathlib import Path


def run(cmd):
    start = time.time()
    proc = subprocess.run(cmd, text=True, capture_output=True, check=False)
    return {
        "cmd": cmd,
        "returncode": proc.returncode,
        "seconds": time.time() - start,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
    }


def main():
    parser = argparse.ArgumentParser(description="Run a reproducible HPRP benchmark.")
    parser.add_argument("--url", default="http://127.0.0.1:8080/")
    parser.add_argument("--connections", type=int, default=128)
    parser.add_argument("--duration", default="30s")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--out", default="benchmark-results/latest.json")
    args = parser.parse_args()

    tool = shutil.which("wrk") or shutil.which("hey")
    if tool is None:
        raise SystemExit("install wrk or hey first")

    if Path(tool).name == "wrk":
        cmd = [tool, "-t", str(args.threads), "-c", str(args.connections), "-d", args.duration, args.url]
    else:
        seconds = int(args.duration.rstrip("s"))
        cmd = [tool, "-c", str(args.connections), "-z", f"{seconds}s", args.url]

    result = {
        "timestamp_unix": int(time.time()),
        "git_commit": run(["git", "rev-parse", "HEAD"])["stdout"].strip(),
        "kernel": run(["uname", "-a"])["stdout"].strip(),
        "benchmark": run(cmd),
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
