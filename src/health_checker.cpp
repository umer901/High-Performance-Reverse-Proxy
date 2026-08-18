#include "hprp/health_checker.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace hprp {
namespace {

bool set_socket_timeout(int fd, std::chrono::milliseconds timeout) {
  timeval tv{};
  tv.tv_sec = static_cast<long>(timeout.count() / 1000);
  tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
         ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

} // namespace

HealthChecker::HealthChecker(BackendPool &pool) : pool_(pool) {}

HealthChecker::~HealthChecker() {
  stop();
}

void HealthChecker::start() {
  running_.store(true);
  thread_ = std::thread(&HealthChecker::run, this);
}

void HealthChecker::stop() {
  running_.store(false);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void HealthChecker::run() {
  while (running_.load()) {
    auto min_sleep = std::chrono::milliseconds(1000);
    for (std::size_t i = 0; i < pool_.size(); ++i) {
      auto &backend = pool_.at(i);
      pool_.mark_health(i, probe(backend));
      min_sleep = std::min(min_sleep, backend.config.health_check.interval);
    }
    std::this_thread::sleep_for(min_sleep);
  }
}

bool HealthChecker::probe(const BackendState &backend) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo *results = nullptr;
  const auto port = std::to_string(backend.config.endpoint.port);
  if (::getaddrinfo(backend.config.endpoint.host.c_str(), port.c_str(), &hints, &results) != 0) {
    return false;
  }

  bool ok = false;
  for (auto *rp = results; rp != nullptr && !ok; rp = rp->ai_next) {
    const int fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      continue;
    }
    (void)set_socket_timeout(fd, backend.config.health_check.timeout);
    if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      const auto request = "GET " + backend.config.health_check.path +
                           " HTTP/1.1\r\nHost: " + backend.config.endpoint.host +
                           "\r\nConnection: close\r\n\r\n";
      if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) > 0) {
        char buffer[128];
        const auto n = ::recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
          buffer[n] = '\0';
          ok = std::strstr(buffer, "HTTP/1.1 2") != nullptr || std::strstr(buffer, "HTTP/1.0 2") != nullptr;
        }
      }
    }
    ::close(fd);
  }
  ::freeaddrinfo(results);
  return ok;
}

} // namespace hprp
