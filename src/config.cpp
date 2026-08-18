#include "hprp/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>

namespace hprp {
namespace {

std::string trim(std::string value) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) { return !is_space(c); }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) { return !is_space(c); }).base(), value.end());
  if ((value.size() >= 2) && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

std::string key_part(const std::string &line) {
  auto pos = line.find(':');
  if (pos == std::string::npos) {
    return {};
  }
  return trim(line.substr(0, pos));
}

std::string value_part(const std::string &line) {
  auto pos = line.find(':');
  if (pos == std::string::npos) {
    return {};
  }
  return trim(line.substr(pos + 1));
}

std::chrono::milliseconds ms_value(const std::string &value) {
  return std::chrono::milliseconds(std::stoi(value));
}

LoadBalancingStrategy parse_strategy(const std::string &value) {
  if (value == "round_robin") {
    return LoadBalancingStrategy::RoundRobin;
  }
  if (value == "least_connections") {
    return LoadBalancingStrategy::LeastConnections;
  }
  throw std::runtime_error("unknown load-balancing strategy: " + value);
}

BackendConfig parse_backend_url(std::string name, const std::string &url) {
  constexpr std::string_view prefix = "http://";
  if (!url.starts_with(prefix)) {
    throw std::runtime_error("backend url must start with http://: " + url);
  }
  auto rest = url.substr(prefix.size());
  auto slash = rest.find('/');
  if (slash != std::string::npos) {
    rest = rest.substr(0, slash);
  }
  auto endpoint = parse_endpoint(rest, 80);
  BackendConfig backend;
  backend.name = std::move(name);
  backend.endpoint = std::move(endpoint);
  return backend;
}

} // namespace

Endpoint parse_endpoint(const std::string &value, int default_port) {
  auto clean = trim(value);
  auto colon = clean.rfind(':');
  if (colon == std::string::npos) {
    return Endpoint{clean, default_port};
  }
  auto host = clean.substr(0, colon);
  auto port = std::stoi(clean.substr(colon + 1));
  if (host.empty() || port <= 0 || port > 65535) {
    throw std::runtime_error("invalid endpoint: " + value);
  }
  return Endpoint{host, port};
}

std::string strategy_name(LoadBalancingStrategy strategy) {
  switch (strategy) {
  case LoadBalancingStrategy::RoundRobin:
    return "round_robin";
  case LoadBalancingStrategy::LeastConnections:
    return "least_connections";
  }
  return "unknown";
}

AppConfig load_config(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open config: " + path);
  }

  AppConfig config;
  std::string section;
  bool in_backend = false;
  bool in_health_check = false;
  BackendConfig current_backend;

  auto finish_backend = [&]() {
    if (in_backend) {
      if (current_backend.name.empty()) {
        current_backend.name = "backend-" + std::to_string(config.backends.size());
      }
      if (current_backend.endpoint.port == 0) {
        throw std::runtime_error("backend is missing url/endpoint");
      }
      if (current_backend.max_pending == 1024) {
        current_backend.max_pending = config.limits.max_pending_per_backend;
      }
      config.backends.push_back(std::move(current_backend));
      current_backend = BackendConfig{};
    }
    in_backend = false;
    in_health_check = false;
  };

  std::string raw;
  while (std::getline(in, raw)) {
    auto hash = raw.find('#');
    if (hash != std::string::npos) {
      raw = raw.substr(0, hash);
    }
    auto line = trim(raw);
    if (line.empty()) {
      continue;
    }

    if (line == "load_balancing:" || line == "limits:" || line == "backends:") {
      if (line != "backends:") {
        finish_backend();
      }
      section = line.substr(0, line.size() - 1);
      continue;
    }

    if (line.starts_with("- ")) {
      finish_backend();
      in_backend = true;
      in_health_check = false;
      current_backend = BackendConfig{};
      line = trim(line.substr(2));
      if (!line.empty() && key_part(line) == "name") {
        current_backend.name = value_part(line);
      }
      continue;
    }

    auto key = key_part(line);
    auto value = value_part(line);
    if (key.empty()) {
      continue;
    }

    if (in_backend) {
      if (key == "health_check") {
        in_health_check = true;
        continue;
      }
      if (in_health_check) {
        if (key == "path") {
          current_backend.health_check.path = value;
        } else if (key == "interval_ms") {
          current_backend.health_check.interval = ms_value(value);
        } else if (key == "timeout_ms") {
          current_backend.health_check.timeout = ms_value(value);
        } else if (key == "unhealthy_threshold") {
          current_backend.health_check.unhealthy_threshold = std::stoi(value);
        } else if (key == "healthy_threshold") {
          current_backend.health_check.healthy_threshold = std::stoi(value);
        }
        continue;
      }
      if (key == "name") {
        current_backend.name = value;
      } else if (key == "url") {
        auto parsed = parse_backend_url(current_backend.name, value);
        current_backend.endpoint = parsed.endpoint;
      } else if (key == "endpoint") {
        current_backend.endpoint = parse_endpoint(value, 80);
      } else if (key == "max_connections") {
        current_backend.max_connections = std::stoi(value);
      } else if (key == "max_pending") {
        current_backend.max_pending = std::stoi(value);
      }
      continue;
    }

    if (section == "load_balancing" && key == "strategy") {
      config.strategy = parse_strategy(value);
    } else if (section == "limits") {
      if (key == "max_client_connections") {
        config.limits.max_client_connections = std::stoi(value);
      } else if (key == "max_pending_per_backend") {
        config.limits.max_pending_per_backend = std::stoi(value);
      } else if (key == "client_header_timeout_ms") {
        config.limits.client_header_timeout = ms_value(value);
      } else if (key == "upstream_connect_timeout_ms") {
        config.limits.upstream_connect_timeout = ms_value(value);
      } else if (key == "request_timeout_ms") {
        config.limits.request_timeout = ms_value(value);
      } else if (key == "max_buffer_bytes") {
        config.limits.max_buffer_bytes = static_cast<std::size_t>(std::stoull(value));
      }
    } else if (key == "listen") {
      config.listen = parse_endpoint(value, 8080);
    } else if (key == "metrics_listen") {
      config.metrics_listen = parse_endpoint(value, 9090);
    } else if (key == "workers") {
      config.workers = value == "auto" ? 1 : std::stoi(value);
    }
  }
  finish_backend();

  if (config.backends.empty()) {
    throw std::runtime_error("config must define at least one backend");
  }
  return config;
}

} // namespace hprp
