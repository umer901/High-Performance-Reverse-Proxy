#include "hprp/backend_pool.hpp"
#include "hprp/config.hpp"
#include "hprp/health_checker.hpp"
#include "hprp/metrics.hpp"
#include "hprp/metrics_server.hpp"
#include "hprp/proxy_server.hpp"

#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

hprp::ProxyServer *g_server = nullptr;

void handle_signal(int) {
  if (g_server != nullptr) {
    g_server->request_stop();
  }
}

void usage(const char *argv0) {
  std::cerr << "usage: " << argv0 << " --config configs/example.yaml\n";
}

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::string config_path = "configs/example.yaml";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  try {
    auto config = hprp::load_config(config_path);
    std::vector<std::string> backend_names;
    backend_names.reserve(config.backends.size());
    for (const auto &backend : config.backends) {
      backend_names.push_back(backend.name);
    }

    hprp::Metrics metrics(std::move(backend_names));
    hprp::BackendPool backend_pool(config.backends, config.strategy, metrics);
    hprp::MetricsServer metrics_server(config.metrics_listen, metrics);
    hprp::HealthChecker health_checker(backend_pool);
    hprp::ProxyServer proxy(config, backend_pool, metrics);

    metrics_server.start();
    health_checker.start();
    g_server = &proxy;
    proxy.run();
    g_server = nullptr;
    health_checker.stop();
    metrics_server.stop();
  } catch (const std::exception &ex) {
    std::cerr << "hprp failed: " << ex.what() << "\n";
    return 1;
  }

  return 0;
}
