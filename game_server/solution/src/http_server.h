#pragma once
#include "sdk.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace http_server {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using RequestHandler = std::function<void(
    http::request<http::string_body>&&,
    std::function<void(http::response<http::string_body>&&)>)>;

class ScopedClientIp {
public:
    explicit ScopedClientIp(std::string ip);
    ~ScopedClientIp();

    ScopedClientIp(const ScopedClientIp&) = delete;
    ScopedClientIp& operator=(const ScopedClientIp&) = delete;

    static const std::string& Get();

private:
    std::string previous_;
};

void ServeHttp(net::io_context& ioc, const net::ip::tcp::endpoint& endpoint, RequestHandler handler);

template <typename Handler>
void ServeHttp(net::io_context& ioc, const net::ip::tcp::endpoint& endpoint, Handler&& handler) {
    ServeHttp(ioc, endpoint, RequestHandler{std::forward<Handler>(handler)});
}

}  // namespace http_server
