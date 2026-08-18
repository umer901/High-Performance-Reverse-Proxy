#pragma once

#include "hprp/backend_pool.hpp"
#include "hprp/config.hpp"
#include "hprp/metrics.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hprp {

class ProxyServer {
public:
  ProxyServer(AppConfig config, BackendPool &backend_pool, Metrics &metrics);
  ~ProxyServer();

  void run();
  void stop();

private:
  enum class FdRole { Listener, Client, Backend };

  struct Session {
    int client_fd{-1};
    int backend_fd{-1};
    std::size_t backend_index{0};
    bool backend_connected{false};
    bool client_eof{false};
    bool backend_eof{false};
    bool request_seen{false};
    bool response_seen{false};
    bool release_pending{true};
    std::string client_to_backend;
    std::string backend_to_client;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point request_started_at;
  };

  struct FdInfo {
    FdRole role;
    Session *session{nullptr};
  };

  void setup_listener();
  void accept_ready();
  void client_ready(Session &session, std::uint32_t events);
  void backend_ready(Session &session, std::uint32_t events);
  void read_client(Session &session);
  void read_backend(Session &session);
  void flush_client(Session &session);
  void flush_backend(Session &session);
  void close_session(Session &session, const char *reason);
  void fail_client(int client_fd, int status, std::string_view reason, std::string_view body);
  void register_fd(int fd, FdRole role, Session *session, std::uint32_t events);
  void update_fd(int fd, std::uint32_t events);
  void unregister_fd(int fd);
  void rearm(Session &session);
  void check_timeouts();

  AppConfig config_;
  BackendPool &backend_pool_;
  Metrics &metrics_;
  bool running_{false};
  int listen_fd_{-1};
  int epoll_fd_{-1};
  int wake_fd_{-1};
  std::unordered_map<int, FdInfo> fd_info_;
  std::unordered_map<int, std::unique_ptr<Session>> sessions_;
  int active_connections_{0};
};

} // namespace hprp
