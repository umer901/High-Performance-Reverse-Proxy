#pragma once

#include "hprp/config.hpp"

#include <string>
#include <string_view>

namespace hprp {

bool has_complete_http_headers(std::string_view data);
bool looks_like_http_request(std::string_view data);
bool looks_like_http_response(std::string_view data);
std::string make_response(int status, std::string_view reason, std::string_view body);
std::string endpoint_to_string(const Endpoint &endpoint);

} // namespace hprp
