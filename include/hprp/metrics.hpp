#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hprp {

class Metrics {
public:
  struct ErrorCounter {
    std::string label;
    std::uint64_t value{0};
  };

  explicit Metrics(std::vector<std::string> backend_names);

  void inc_request(std::size_t backend, int status_class);
  void observe_latency(std::chrono::nanoseconds latency);
  void inc_error(const std::string &type);
  void inc_timeout(const std::string &phase);
  void set_active_connections(std::int64_t value);
  void set_backend_active(std::size_t backend, std::int64_t value);
  void set_backend_health(std::size_t backend, bool healthy);
  void set_queue_depth(std::size_t backend, std::int64_t value);

  std::string render_prometheus() const;

private:
  static constexpr std::array<double, 13> kLatencyBuckets{
      0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05,
      0.1,    0.25,  0.5,    1.0,   2.5,  5.0};

  std::vector<std::string> backend_names_;
  mutable std::mutex mutex_;
  std::vector<std::array<std::uint64_t, 5>> requests_by_backend_status_;
  std::array<std::uint64_t, kLatencyBuckets.size() + 1> latency_buckets_{};
  double latency_sum_seconds_{0.0};
  std::uint64_t latency_count_{0};
  std::vector<std::int64_t> backend_active_;
  std::vector<std::int64_t> backend_health_;
  std::vector<std::int64_t> queue_depth_;
  std::int64_t active_connections_{0};
  std::vector<ErrorCounter> errors_;
  std::vector<ErrorCounter> timeouts_;
};

} // namespace hprp
