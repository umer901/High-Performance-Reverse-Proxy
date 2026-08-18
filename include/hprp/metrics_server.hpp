#pragma once

#include "hprp/config.hpp"
#include "hprp/metrics.hpp"

#include <atomic>
#include <thread>

namespace hprp {

class MetricsServer {
public:
  MetricsServer(Endpoint listen, Metrics &metrics);
  ~MetricsServer();

  void start();
  void stop();

private:
  void run();

  Endpoint listen_;
  Metrics &metrics_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  int listen_fd_{-1};
};

} // namespace hprp
