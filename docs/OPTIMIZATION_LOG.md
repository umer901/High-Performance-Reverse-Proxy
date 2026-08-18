# Optimization Log

Use this file as the public performance diary for the project.

## Iteration Template

### YYYY-MM-DD: Change Name

- Commit:
- Config:
- Traffic:
- Baseline throughput:
- New throughput:
- Baseline latency:
- New latency:
- CPU delta:
- Memory delta:
- Tooling used:
- Decision:

## Initial Implementation

- C++23 Linux epoll proxy.
- Round-robin and least-connections backend selection.
- Active health checks.
- Prometheus metrics endpoint.
- Bounded per-session buffers and connection limits.
