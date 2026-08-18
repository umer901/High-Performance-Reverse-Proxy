# HPRP: High-Performance Reverse Proxy

HPRP is a Linux-focused reverse proxy and lightweight load balancer written primarily in modern C++. It adresses: networking, concurrency, health monitoring, backpressure, benchmarking, profiling, and measured optimization.

The current implementation is a deliberately simple but real first version:

- C++23 single-process proxy using Linux `epoll`
- HTTP/1.1 plaintext forwarding
- non-blocking client and upstream sockets
- round-robin and least-connections load balancing
- active backend health checks
- bounded per-session buffers
- configurable connection and timeout limits
- Prometheus-compatible `/metrics`
- Docker backend lab
- smoke integration test
- benchmark and profiling scripts
- CI with sanitizer test build

## Build

On WSL2/Ubuntu:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

Build with sanitizers:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHPRP_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

## Run Locally

Start test backends:

```bash
docker compose up --build -d backend-a backend-b backend-c
```

Start the proxy:

```bash
./build/hprp --config configs/example.yaml
```

Send traffic:

```bash
curl http://127.0.0.1:8080/
curl http://127.0.0.1:9090/metrics
```

Optional observability stack:

```bash
docker compose up -d prometheus grafana
```

Prometheus is exposed at `http://127.0.0.1:9091`. Grafana is exposed at `http://127.0.0.1:3000`.

## Configuration

Example:

```yaml
listen: "0.0.0.0:8080"
metrics_listen: "127.0.0.1:9090"

load_balancing:
  strategy: "round_robin" # round_robin | least_connections

limits:
  max_client_connections: 10000
  client_header_timeout_ms: 2000
  upstream_connect_timeout_ms: 500
  request_timeout_ms: 5000
  max_buffer_bytes: 1048576

backends:
  - name: "backend-a"
    url: "http://127.0.0.1:9001"
    max_connections: 1024
    health_check:
      path: "/healthz"
      interval_ms: 1000
      timeout_ms: 200
      unhealthy_threshold: 3
      healthy_threshold: 2
```

## Benchmarking

Install either `wrk` or `hey`, then run:

```bash
python3 benchmarks/run_benchmark.py --connections 128 --duration 30s
```

The script writes a JSON artifact under `benchmark-results/` with the git commit, kernel, command, duration, and raw benchmark output.

For every optimization, record before/after results in [docs/OPTIMIZATION_LOG.md](docs/OPTIMIZATION_LOG.md):

- throughput / requests per second
- p50, p95, p99, p99.9 latency
- CPU usage
- resident memory
- error and timeout rates
- backend health
- queue depth

## Profiling

During a benchmark:

```bash
pgrep hprp
scripts/profile_perf.sh <pid> 30
```

Use the generated `profiles/perf-report-<pid>.txt` to identify bottlenecks before optimizing.

## Roadmap

The first implementation intentionally favors clarity over heroic micro-optimization. Good next measured iterations:

1. Add a blocking baseline binary for direct comparison.
2. Replace string prefix erasure with reusable ring buffers.
3. Add upstream keep-alive connection pooling.
4. Shard metrics by reactor thread to reduce lock contention.
5. Add multi-worker accept with `SO_REUSEPORT`.
6. Expand reliability tests for slow backends, queue saturation, crashes, and recovery.
7. Add benchmark result tables and flamegraph images to docs.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/BENCHMARKING.md](docs/BENCHMARKING.md) for more detail.
