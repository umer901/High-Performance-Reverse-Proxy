#include "hprp/backend_pool.hpp"

#include <limits>

namespace hprp {

BackendPool::BackendPool(std::vector<BackendConfig> backends, LoadBalancingStrategy strategy, Metrics &metrics)
    : strategy_(strategy), metrics_(metrics) {
  backends_.reserve(backends.size());
  for (auto &backend : backends) {
    backends_.emplace_back(std::move(backend));
  }
  for (std::size_t i = 0; i < backends_.size(); ++i) {
    metrics_.set_backend_health(i, true);
    metrics_.set_backend_active(i, 0);
    metrics_.set_queue_depth(i, 0);
  }
}

bool BackendPool::can_accept(const BackendState &backend) const {
  return backend.healthy.load(std::memory_order_relaxed) &&
         backend.active_connections.load(std::memory_order_relaxed) < backend.config.max_connections;
}

std::optional<std::size_t> BackendPool::choose_backend() {
  if (backends_.empty()) {
    return std::nullopt;
  }

  if (strategy_ == LoadBalancingStrategy::RoundRobin) {
    const auto start = rr_cursor_.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t attempt = 0; attempt < backends_.size(); ++attempt) {
      const auto index = (start + attempt) % backends_.size();
      auto &backend = backends_[index];
      if (can_accept(backend)) {
        const auto active = backend.active_connections.fetch_add(1, std::memory_order_relaxed) + 1;
        metrics_.set_backend_active(index, active);
        return index;
      }
    }
    return std::nullopt;
  }

  std::optional<std::size_t> best;
  int best_active = std::numeric_limits<int>::max();
  for (std::size_t i = 0; i < backends_.size(); ++i) {
    auto &backend = backends_[i];
    const auto active = backend.active_connections.load(std::memory_order_relaxed);
    if (can_accept(backend) && active < best_active) {
      best = i;
      best_active = active;
    }
  }
  if (!best.has_value()) {
    return std::nullopt;
  }
  auto &backend = backends_[*best];
  const auto active = backend.active_connections.fetch_add(1, std::memory_order_relaxed) + 1;
  metrics_.set_backend_active(*best, active);
  return best;
}

void BackendPool::release_backend(std::size_t index) {
  if (index >= backends_.size()) {
    return;
  }
  auto &backend = backends_[index];
  const auto previous = backend.active_connections.fetch_sub(1, std::memory_order_relaxed);
  const auto active = previous > 0 ? previous - 1 : 0;
  metrics_.set_backend_active(index, active);
}

void BackendPool::mark_health(std::size_t index, bool probe_ok) {
  if (index >= backends_.size()) {
    return;
  }
  auto &backend = backends_[index];
  if (probe_ok) {
    backend.health_failures.store(0, std::memory_order_relaxed);
    const auto successes = backend.health_successes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (successes >= backend.config.health_check.healthy_threshold) {
      backend.healthy.store(true, std::memory_order_relaxed);
      metrics_.set_backend_health(index, true);
    }
  } else {
    backend.health_successes.store(0, std::memory_order_relaxed);
    const auto failures = backend.health_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failures >= backend.config.health_check.unhealthy_threshold) {
      backend.healthy.store(false, std::memory_order_relaxed);
      metrics_.set_backend_health(index, false);
    }
  }
}

} // namespace hprp
