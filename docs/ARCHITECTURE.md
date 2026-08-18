# Architecture

HPRP is a small Linux-focused HTTP/1.1 reverse proxy. The first implementation is intentionally compact: one epoll reactor owns client and upstream sockets, while separate lightweight threads expose metrics and probe backend health.

## Data Flow

1. The listener accepts a client socket.
2. `BackendPool` selects a healthy backend by round-robin or least-connections.
3. The proxy opens a non-blocking upstream socket.
4. Bytes are streamed client-to-backend and backend-to-client through bounded buffers.
5. Metrics are updated when the first response headers arrive.
6. The session closes on EOF, timeout, socket error, malformed request, or buffer saturation.

## Reliability Boundaries

The proxy uses bounded per-session buffers and configurable connection limits. If no backend is healthy or capacity is exhausted, the proxy returns `503` instead of accumulating unbounded work.

Health checks run independently and mark a backend unhealthy after configurable failures, then healthy again after configurable successes.

## Optimization Notes

The current hot path intentionally uses simple `std::string` buffers and prefix erasure. That is measurable and easy to understand, but it is also a good future optimization target. Later iterations should replace this with ring buffers or reusable fixed buffers only after benchmark data shows the cost.
