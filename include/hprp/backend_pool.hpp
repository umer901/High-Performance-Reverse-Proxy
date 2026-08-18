#pragma once

#include "hprp/config.hpp"
#include "hprp/metrics.hpp"

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hprp {

struct BackendState {
  BackendState() = default;
  explicit BackendState(BackendConfig backend_config) : config(std::move(backend_config)) {}
  BackendState(const BackendState &) = delete;
  BackendState &operator=(const BackendState &) = delete;
  BackendState(BackendState &&other) noexcept
      : config(std::move(other.config)),
        healthy(other.healthy.load()),
        active_connections(other.active_connections.load()),
        queued_connections(other.queued_connections.load()),
        health_failures(other.health_failures.load()),
        health_successes(other.health_successes.load()) {}
  BackendState &operator=(BackendState &&other) noexcept {
    config = std::move(other.config);
    healthy.store(other.healthy.load());
    active_connections.store(other.active_connections.load());
    queued_connections.store(other.queued_connections.load());
    health_failures.store(other.health_failures.load());
    health_successes.store(other.health_successes.load());
    return *this;
  }

  BackendConfig config;
  std::atomic<bool> healthy{true};
  std::atomic<int> active_connections{0};
  std::atomic<int> queued_connections{0};
  std::atomic<int> health_failures{0};
  std::atomic<int> health_successes{0};
};

class BackendPool {
public:
  BackendPool(std::vector<BackendConfig> backends, LoadBalancingStrategy strategy, Metrics &metrics);

  std::optional<std::size_t> choose_backend();
  void release_backend(std::size_t index);
  void mark_health(std::size_t index, bool probe_ok);

  BackendState &at(std::size_t index) { return backends_.at(index); }
  const BackendState &at(std::size_t index) const { return backends_.at(index); }
  std::size_t size() const { return backends_.size(); }

private:
  bool can_accept(const BackendState &backend) const;

  std::vector<BackendState> backends_;
  LoadBalancingStrategy strategy_;
  Metrics &metrics_;
  std::atomic<std::size_t> rr_cursor_{0};
};

} // namespace hprp
