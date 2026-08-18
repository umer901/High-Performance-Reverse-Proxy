#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace hprp {

enum class LoadBalancingStrategy {
  RoundRobin,
  LeastConnections,
};

struct Endpoint {
  std::string host{"127.0.0.1"};
  int port{0};
};

struct HealthCheckConfig {
  std::string path{"/healthz"};
  std::chrono::milliseconds interval{1000};
  std::chrono::milliseconds timeout{200};
  int unhealthy_threshold{3};
  int healthy_threshold{2};
};

struct BackendConfig {
  std::string name;
  Endpoint endpoint;
  int max_connections{1024};
  int max_pending{1024};
  HealthCheckConfig health_check;
};

struct LimitsConfig {
  int max_client_connections{10000};
  int max_pending_per_backend{1024};
  std::chrono::milliseconds client_header_timeout{2000};
  std::chrono::milliseconds upstream_connect_timeout{500};
  std::chrono::milliseconds request_timeout{5000};
  std::size_t max_buffer_bytes{1 << 20};
};

struct AppConfig {
  Endpoint listen{"0.0.0.0", 8080};
  Endpoint metrics_listen{"127.0.0.1", 9090};
  int workers{1};
  LoadBalancingStrategy strategy{LoadBalancingStrategy::RoundRobin};
  LimitsConfig limits;
  std::vector<BackendConfig> backends;
};

AppConfig load_config(const std::string &path);
std::string strategy_name(LoadBalancingStrategy strategy);
Endpoint parse_endpoint(const std::string &value, int default_port);

} // namespace hprp
