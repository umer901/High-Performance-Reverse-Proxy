# Benchmarking Methodology

Benchmarks should be reproducible and tied to a git commit, config file, traffic profile, kernel, and machine.

Recommended local flow:

```bash
docker compose up --build -d backend-a backend-b backend-c
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hprp --config configs/example.yaml
python3 benchmarks/run_benchmark.py --connections 128 --duration 30s
curl http://127.0.0.1:9090/metrics
```

Record for every serious optimization:

- throughput / requests per second
- p50, p95, p99, p99.9 latency
- CPU usage
- resident memory
- error and timeout rates
- backend health and queue depth

Use `scripts/profile_perf.sh <pid>` during a benchmark run to collect a first-pass CPU profile.
