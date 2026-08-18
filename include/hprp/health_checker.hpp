#pragma once

#include "hprp/backend_pool.hpp"

#include <atomic>
#include <thread>

namespace hprp {

class HealthChecker {
public:
  explicit HealthChecker(BackendPool &pool);
  ~HealthChecker();

  void start();
  void stop();

private:
  void run();
  bool probe(const BackendState &backend);

  BackendPool &pool_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

} // namespace hprp
