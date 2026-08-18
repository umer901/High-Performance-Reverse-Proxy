#include "hprp/metrics_server.hpp"

#include "hprp/http_utils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hprp {
namespace {

int make_listener(const Endpoint &endpoint) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
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
    throw std::runtime_error("metrics host must be an IPv4 address: " + endpoint.host);
  }
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, 128) != 0) {
    const auto error = std::strerror(errno);
    ::close(fd);
    throw std::runtime_error(std::string("failed to bind metrics listener: ") + error);
  }
  return fd;
}

void write_all(int fd, const std::string &data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      return;
    }
    sent += static_cast<std::size_t>(n);
  }
}

} // namespace

MetricsServer::MetricsServer(Endpoint listen, Metrics &metrics) : listen_(std::move(listen)), metrics_(metrics) {}

MetricsServer::~MetricsServer() {
  stop();
}

void MetricsServer::start() {
  running_.store(true);
  thread_ = std::thread(&MetricsServer::run, this);
}

void MetricsServer::stop() {
  running_.store(false);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

void MetricsServer::run() {
  try {
    listen_fd_ = make_listener(listen_);
    std::cerr << "metrics listening on " << endpoint_to_string(listen_) << "\n";
    while (running_.load()) {
      sockaddr_in addr{};
      socklen_t len = sizeof(addr);
      const int client = ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len);
      if (client < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      char buffer[1024];
      (void)::recv(client, buffer, sizeof(buffer), 0);
      const auto body = metrics_.render_prometheus();
      const auto response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      write_all(client, response);
      ::close(client);
    }
  } catch (const std::exception &ex) {
    std::cerr << "metrics server failed: " << ex.what() << "\n";
  }
}

} // namespace hprp
