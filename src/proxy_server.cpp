#include "hprp/proxy_server.hpp"

#include "hprp/http_utils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <array>
#include <iostream>
#include <netdb.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hprp {
namespace {

constexpr int kMaxEvents = 256;
constexpr std::size_t kIoChunk = 64 * 1024;

int make_listener(const Endpoint &endpoint) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    throw std::runtime_error("socket failed");
  }
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(endpoint.port));
  if (::inet_pton(AF_INET, endpoint.host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    throw std::runtime_error("listen host must be an IPv4 address: " + endpoint.host);
  }
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, SOMAXCONN) != 0) {
    const auto error = std::strerror(errno);
    ::close(fd);
    throw std::runtime_error(std::string("failed to bind proxy listener: ") + error);
  }
  return fd;
}

int connect_nonblocking(const Endpoint &endpoint) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo *results = nullptr;
  const auto port = std::to_string(endpoint.port);
  if (::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0) {
    return -1;
  }

  int fd = -1;
  for (auto *rp = results; rp != nullptr; rp = rp->ai_next) {
    fd = ::socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
    if (fd < 0) {
      continue;
    }
    const int rc = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
    if (rc == 0 || errno == EINPROGRESS) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(results);
  return fd;
}

std::uint32_t base_events() {
  return EPOLLERR | EPOLLHUP | EPOLLRDHUP;
}

} // namespace

ProxyServer::ProxyServer(AppConfig config, BackendPool &backend_pool, Metrics &metrics)
    : config_(std::move(config)), backend_pool_(backend_pool), metrics_(metrics) {}

ProxyServer::~ProxyServer() {
  stop();
}

void ProxyServer::setup_listener() {
  listen_fd_ = make_listener(config_.listen);
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    throw std::runtime_error("epoll_create1 failed");
  }
  wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) {
    throw std::runtime_error("eventfd failed");
  }
  register_fd(listen_fd_, FdRole::Listener, nullptr, EPOLLIN);
  register_fd(wake_fd_, FdRole::Listener, nullptr, EPOLLIN);
  std::cerr << "proxy listening on " << endpoint_to_string(config_.listen)
            << ", strategy=" << strategy_name(config_.strategy) << "\n";
}

void ProxyServer::run() {
  setup_listener();
  running_.store(true, std::memory_order_relaxed);
  std::array<epoll_event, kMaxEvents> events{};

  while (running_.load(std::memory_order_relaxed)) {
    const int n = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), 100);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("epoll_wait failed");
    }
    for (int i = 0; i < n; ++i) {
      const int fd = events[static_cast<std::size_t>(i)].data.fd;
      auto found = fd_info_.find(fd);
      if (found == fd_info_.end()) {
        continue;
      }
      if (found->second.role == FdRole::Listener) {
        if (fd == listen_fd_) {
          accept_ready();
        } else {
          std::uint64_t value = 0;
          (void)::read(wake_fd_, &value, sizeof(value));
        }
      } else if (found->second.role == FdRole::Client && found->second.session != nullptr) {
        client_ready(*found->second.session, events[static_cast<std::size_t>(i)].events);
      } else if (found->second.role == FdRole::Backend && found->second.session != nullptr) {
        backend_ready(*found->second.session, events[static_cast<std::size_t>(i)].events);
      }
    }
    check_timeouts();
  }
}

void ProxyServer::stop() {
  running_.store(false, std::memory_order_relaxed);
  if (wake_fd_ >= 0) {
    std::uint64_t one = 1;
    (void)::write(wake_fd_, &one, sizeof(one));
  }
  for (auto &[client_fd, session] : sessions_) {
    if (session->client_fd >= 0) {
      ::close(session->client_fd);
    }
    if (session->backend_fd >= 0) {
      ::close(session->backend_fd);
    }
    if (session->release_pending) {
      backend_pool_.release_backend(session->backend_index);
    }
  }
  sessions_.clear();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (wake_fd_ >= 0) {
    ::close(wake_fd_);
    wake_fd_ = -1;
  }
  if (epoll_fd_ >= 0) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
  }
}

void ProxyServer::request_stop() {
  running_.store(false, std::memory_order_relaxed);
  if (wake_fd_ >= 0) {
    std::uint64_t one = 1;
    (void)::write(wake_fd_, &one, sizeof(one));
  }
}

void ProxyServer::accept_ready() {
  while (true) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    const int client_fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      metrics_.inc_error("accept");
      break;
    }

    if (active_connections_ >= config_.limits.max_client_connections) {
      fail_client(client_fd, 503, "Service Unavailable", "proxy overloaded\n");
      metrics_.inc_error("overloaded");
      continue;
    }

    auto backend_index = backend_pool_.choose_backend();
    if (!backend_index.has_value()) {
      fail_client(client_fd, 503, "Service Unavailable", "no healthy backend available\n");
      metrics_.inc_error("no_backend");
      continue;
    }

    auto &backend = backend_pool_.at(*backend_index);
    const int backend_fd = connect_nonblocking(backend.config.endpoint);
    if (backend_fd < 0) {
      backend_pool_.release_backend(*backend_index);
      fail_client(client_fd, 503, "Service Unavailable", "upstream connect failed\n");
      metrics_.inc_error("connect");
      continue;
    }

    auto session = std::make_unique<Session>();
    session->client_fd = client_fd;
    session->backend_fd = backend_fd;
    session->backend_index = *backend_index;
    session->created_at = std::chrono::steady_clock::now();
    session->last_activity = session->created_at;
    session->request_started_at = session->created_at;
    auto *raw_session = session.get();
    sessions_.emplace(client_fd, std::move(session));
    ++active_connections_;
    metrics_.set_active_connections(active_connections_);

    register_fd(client_fd, FdRole::Client, raw_session, EPOLLIN | base_events());
    register_fd(backend_fd, FdRole::Backend, raw_session, EPOLLOUT | base_events());
  }
}

void ProxyServer::client_ready(Session &session, std::uint32_t events) {
  const int client_fd = session.client_fd;
  if ((events & EPOLLIN) != 0U) {
    read_client(session);
    if (sessions_.find(client_fd) == sessions_.end()) {
      return;
    }
  }
  if ((events & EPOLLOUT) != 0U) {
    flush_client(session);
    if (sessions_.find(client_fd) == sessions_.end()) {
      return;
    }
  }
  if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U && session.client_to_backend.empty()) {
    close_session(session, "client_closed");
    return;
  }
  rearm(session);
}

void ProxyServer::backend_ready(Session &session, std::uint32_t events) {
  const int client_fd = session.client_fd;
  if (!session.backend_connected && (events & EPOLLOUT) != 0U) {
    int error = 0;
    socklen_t len = sizeof(error);
    if (::getsockopt(session.backend_fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
      metrics_.inc_error("connect");
      close_session(session, "connect_failed");
      return;
    }
    session.backend_connected = true;
  }
  if ((events & EPOLLOUT) != 0U) {
    flush_backend(session);
    if (sessions_.find(client_fd) == sessions_.end()) {
      return;
    }
  }
  if ((events & EPOLLIN) != 0U) {
    read_backend(session);
    if (sessions_.find(client_fd) == sessions_.end()) {
      return;
    }
  }
  if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U && session.backend_to_client.empty()) {
    close_session(session, "backend_closed");
    return;
  }
  rearm(session);
}

void ProxyServer::read_client(Session &session) {
  char buffer[kIoChunk];
  while (true) {
    const auto n = ::recv(session.client_fd, buffer, sizeof(buffer), 0);
    if (n > 0) {
      session.last_activity = std::chrono::steady_clock::now();
      session.client_to_backend.append(buffer, static_cast<std::size_t>(n));
      if (!session.request_seen && has_complete_http_headers(session.client_to_backend)) {
        if (!looks_like_http_request(session.client_to_backend)) {
          metrics_.inc_error("malformed_request");
          session.backend_to_client = make_response(400, "Bad Request", "malformed request\n");
          if (session.backend_fd >= 0) {
            unregister_fd(session.backend_fd);
            ::close(session.backend_fd);
            session.backend_fd = -1;
          }
          if (session.release_pending) {
            backend_pool_.release_backend(session.backend_index);
            session.release_pending = false;
          }
          session.client_to_backend.clear();
          session.client_eof = true;
          session.backend_eof = true;
          return;
        }
        session.request_seen = true;
        session.request_started_at = std::chrono::steady_clock::now();
      }
      if (session.client_to_backend.size() > config_.limits.max_buffer_bytes) {
        metrics_.inc_error("client_buffer_overflow");
        close_session(session, "client_buffer_overflow");
        return;
      }
      continue;
    }
    if (n == 0) {
      session.client_eof = true;
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    metrics_.inc_error("client_read");
    close_session(session, "client_read");
    return;
  }
}

void ProxyServer::read_backend(Session &session) {
  char buffer[kIoChunk];
  while (true) {
    const auto n = ::recv(session.backend_fd, buffer, sizeof(buffer), 0);
    if (n > 0) {
      session.last_activity = std::chrono::steady_clock::now();
      session.backend_to_client.append(buffer, static_cast<std::size_t>(n));
      if (session.request_seen && !session.response_seen && has_complete_http_headers(session.backend_to_client)) {
        session.response_seen = true;
        int status_class = 5;
        if (looks_like_http_response(session.backend_to_client) && session.backend_to_client.size() >= 10) {
          const char code = session.backend_to_client[9];
          if (code >= '1' && code <= '5') {
            status_class = code - '0';
          }
        }
        metrics_.inc_request(session.backend_index, status_class);
        metrics_.observe_latency(std::chrono::steady_clock::now() - session.request_started_at);
      }
      if (session.backend_to_client.size() > config_.limits.max_buffer_bytes) {
        metrics_.inc_error("backend_buffer_overflow");
        close_session(session, "backend_buffer_overflow");
        return;
      }
      continue;
    }
    if (n == 0) {
      session.backend_eof = true;
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    metrics_.inc_error("backend_read");
    close_session(session, "backend_read");
    return;
  }
}

void ProxyServer::flush_client(Session &session) {
  while (!session.backend_to_client.empty()) {
    const auto n = ::send(session.client_fd, session.backend_to_client.data(), session.backend_to_client.size(), MSG_NOSIGNAL);
    if (n > 0) {
      session.last_activity = std::chrono::steady_clock::now();
      session.backend_to_client.erase(0, static_cast<std::size_t>(n));
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    metrics_.inc_error("client_write");
    close_session(session, "client_write");
    return;
  }
  if (session.backend_eof || session.client_eof) {
    close_session(session, "finished");
  }
}

void ProxyServer::flush_backend(Session &session) {
  if (!session.backend_connected || session.backend_fd < 0) {
    return;
  }
  while (!session.client_to_backend.empty()) {
    const auto n = ::send(session.backend_fd, session.client_to_backend.data(), session.client_to_backend.size(), MSG_NOSIGNAL);
    if (n > 0) {
      session.last_activity = std::chrono::steady_clock::now();
      session.client_to_backend.erase(0, static_cast<std::size_t>(n));
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    metrics_.inc_error("backend_write");
    close_session(session, "backend_write");
    return;
  }
  if (session.client_eof) {
    (void)::shutdown(session.backend_fd, SHUT_WR);
  }
}

void ProxyServer::close_session(Session &session, const char *reason) {
  (void)reason;
  const int client_fd = session.client_fd;
  if (session.client_fd >= 0) {
    unregister_fd(session.client_fd);
    ::close(session.client_fd);
    session.client_fd = -1;
  }
  if (session.backend_fd >= 0) {
    unregister_fd(session.backend_fd);
    ::close(session.backend_fd);
    session.backend_fd = -1;
  }
  if (session.release_pending) {
    backend_pool_.release_backend(session.backend_index);
    session.release_pending = false;
  }
  if (active_connections_ > 0) {
    --active_connections_;
  }
  metrics_.set_active_connections(active_connections_);
  sessions_.erase(client_fd);
}

void ProxyServer::fail_client(int client_fd, int status, std::string_view reason, std::string_view body) {
  const auto response = make_response(status, reason, body);
  (void)::send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
  ::close(client_fd);
}

void ProxyServer::register_fd(int fd, FdRole role, Session *session, std::uint32_t events) {
  epoll_event event{};
  event.events = events;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) != 0) {
    throw std::runtime_error("epoll_ctl ADD failed");
  }
  fd_info_[fd] = FdInfo{role, session};
}

void ProxyServer::update_fd(int fd, std::uint32_t events) {
  if (fd < 0 || fd_info_.find(fd) == fd_info_.end()) {
    return;
  }
  epoll_event event{};
  event.events = events;
  event.data.fd = fd;
  (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event);
}

void ProxyServer::unregister_fd(int fd) {
  if (fd < 0) {
    return;
  }
  (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  fd_info_.erase(fd);
}

void ProxyServer::rearm(Session &session) {
  std::uint32_t client_events = base_events();
  if (!session.client_eof && session.client_to_backend.size() < config_.limits.max_buffer_bytes) {
    client_events |= EPOLLIN;
  }
  if (!session.backend_to_client.empty()) {
    client_events |= EPOLLOUT;
  }
  update_fd(session.client_fd, client_events);

  if (session.backend_fd >= 0) {
    std::uint32_t backend_events = base_events();
    if (session.backend_connected && !session.backend_eof && session.backend_to_client.size() < config_.limits.max_buffer_bytes) {
      backend_events |= EPOLLIN;
    }
    if (!session.backend_connected || !session.client_to_backend.empty()) {
      backend_events |= EPOLLOUT;
    }
    update_fd(session.backend_fd, backend_events);
  }
}

void ProxyServer::check_timeouts() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<Session *> expired;
  expired.reserve(sessions_.size());
  for (auto &[fd, session] : sessions_) {
    (void)fd;
    if (!session->request_seen && now - session->created_at > config_.limits.client_header_timeout) {
      metrics_.inc_timeout("client_header");
      expired.push_back(session.get());
    } else if (!session->backend_connected && now - session->created_at > config_.limits.upstream_connect_timeout) {
      metrics_.inc_timeout("upstream_connect");
      expired.push_back(session.get());
    } else if (now - session->last_activity > config_.limits.request_timeout) {
      metrics_.inc_timeout("request");
      expired.push_back(session.get());
    }
  }
  for (auto *session : expired) {
    if (sessions_.find(session->client_fd) != sessions_.end()) {
      close_session(*session, "timeout");
    }
  }
}

} // namespace hprp
