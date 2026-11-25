#include "http_server.h"

#include "logger.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>

#include <utility>

namespace http_server {
namespace {

thread_local std::string current_client_ip;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(net::ip::tcp::socket socket, RequestHandler handler, net::strand<net::io_context::executor_type> api_strand)
        : stream_(std::move(socket))
        , handler_(std::move(handler))
        , api_strand_(std::move(api_strand)) {
    }

    void Run() {
        net::dispatch(stream_.get_executor(), [self = shared_from_this()] {
            self->ReadRequest();
        });
    }

private:
    void ReadRequest() {
        req_ = {};
        http::async_read(stream_, buffer_, req_,[self = shared_from_this()](beast::error_code ec, std::size_t) {
            self->OnRead(ec);
        });
    }

    void OnRead(beast::error_code ec) {
        if (ec == http::error::end_of_stream) {
            return Close();
        }
        if (ec) {
            if (ec != net::error::operation_aborted) {
                logger::LogNetworkError(ec.value(), ec.message(), "read");
            }
            return;
        }

        const std::string client_ip = stream_.socket().remote_endpoint().address().to_string();
        auto self = shared_from_this();
        auto send = [self](http::response<http::string_body>&& response) {
            self->WriteResponse(std::move(response));
        };

        net::post(api_strand_,
                  [self, client_ip, req = std::move(req_),
                   send = std::move(send)]() mutable {
                       ScopedClientIp client_ip_scope{client_ip};
                        try {
                            self->handler_(std::move(req), std::move(send));
                        } catch (const std::exception& ex) {
                            http::response<http::string_body> response{http::status::internal_server_error, req.version()};
                            response.set(http::field::content_type, "application/json");
                            response.set(http::field::cache_control, "no-cache");
                            response.body() = std::string("{\"code\":\"internalError\",\"message\":\"") + ex.what() + "\"}";
                            response.prepare_payload();
                            self->WriteResponse(std::move(response));
                        }
                    }
        );
    }

    void WriteResponse(http::response<http::string_body>&& response) {
        auto self = shared_from_this();
        net::dispatch(stream_.get_executor(),[self, response = std::move(response)]() mutable {
            self->DoWrite(std::move(response));
        });
    }

    void DoWrite(http::response<http::string_body>&& response) {
        if (writing_) {
            return;
        }
        writing_ = true;
        const bool keep_alive = response.keep_alive();
        response_ptr_ = std::make_shared<http::response<http::string_body>>(std::move(response));

        http::async_write(stream_, *response_ptr_,
                          [self = shared_from_this(), keep_alive](beast::error_code ec, std::size_t) {
                              self->writing_ = false;
                              self->response_ptr_.reset();
                              if (ec) {
                                  if (ec != net::error::operation_aborted) {
                                      logger::LogNetworkError(ec.value(), ec.message(), "write");
                                  }
                                  return;
                              }
                              if (!keep_alive) {
                                  self->Close();
                                  return;
                              }
                              self->ReadRequest();
                          });
    }

    void Close() {
        beast::error_code ec;
        stream_.socket().shutdown(net::ip::tcp::socket::shutdown_send, ec);
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    RequestHandler handler_;
    net::strand<net::io_context::executor_type> api_strand_;
    bool writing_ = false;
    std::shared_ptr<http::response<http::string_body>> response_ptr_;
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(net::io_context& ioc, net::ip::tcp::endpoint endpoint, RequestHandler handler)
        : ioc_(ioc)
        , acceptor_(ioc)
        , handler_(std::move(handler))
        , api_strand_(net::make_strand(ioc)) {
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw beast::system_error(ec);
        }    
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            throw beast::system_error(ec);
        }    
        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw beast::system_error(ec);
        }    
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            throw beast::system_error(ec);
        }    
    }

    void Run() {
        DoAccept();
    }

private:
    void DoAccept() {
        acceptor_.async_accept([self = shared_from_this()](beast::error_code ec, net::ip::tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), self->handler_, self->api_strand_)->Run();
                } else if (ec != net::error::operation_aborted) {
                    logger::LogNetworkError(ec.value(), ec.message(), "accept");
                }
                if (self->acceptor_.is_open()) {
                    self->DoAccept();
                }
            });
    }

    net::io_context& ioc_;
    net::ip::tcp::acceptor acceptor_;
    RequestHandler handler_;
    net::strand<net::io_context::executor_type> api_strand_;
};

}  // namespace

ScopedClientIp::ScopedClientIp(std::string ip)
    : previous_(std::exchange(current_client_ip, std::move(ip))) {
}

ScopedClientIp::~ScopedClientIp() {
    current_client_ip = std::move(previous_);
}

const std::string& ScopedClientIp::Get() {
    return current_client_ip;
}

void ServeHttp(net::io_context& ioc, const net::ip::tcp::endpoint& endpoint, RequestHandler handler) {
    std::make_shared<Listener>(ioc, endpoint, std::move(handler))->Run();
}

}  // namespace http_server
