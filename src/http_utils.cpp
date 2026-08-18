#include "hprp/http_utils.hpp"

#include <sstream>

namespace hprp {

bool has_complete_http_headers(std::string_view data) {
  return data.find("\r\n\r\n") != std::string_view::npos;
}

bool looks_like_http_request(std::string_view data) {
  auto line_end = data.find("\r\n");
  if (line_end == std::string_view::npos) {
    return true;
  }
  auto line = data.substr(0, line_end);
  return line.starts_with("GET ") || line.starts_with("POST ") || line.starts_with("PUT ") ||
         line.starts_with("PATCH ") || line.starts_with("DELETE ") || line.starts_with("HEAD ") ||
         line.starts_with("OPTIONS ");
}

bool looks_like_http_response(std::string_view data) {
  return data.starts_with("HTTP/1.0 ") || data.starts_with("HTTP/1.1 ");
}

std::string make_response(int status, std::string_view reason, std::string_view body) {
  std::ostringstream out;
  out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: text/plain\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  return out.str();
}

std::string endpoint_to_string(const Endpoint &endpoint) {
  return endpoint.host + ":" + std::to_string(endpoint.port);
}

} // namespace hprp
