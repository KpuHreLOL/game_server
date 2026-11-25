#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <functional>
#include <utility>

namespace ticker {

class Ticker {
public:
    using Clock = std::chrono::steady_clock;
    using Handler = std::function<void(std::chrono::milliseconds delta)>;

    Ticker(boost::asio::io_context& ioc, std::chrono::milliseconds period, Handler handler)
        : timer_{ioc}
        , period_{period}
        , handler_{std::move(handler)} {
    }

    Ticker(const Ticker&) = delete;
    Ticker& operator=(const Ticker&) = delete;

    void Start() {
        last_tick_ = Clock::now();
        ScheduleTick();
    }

private:
    void ScheduleTick() {
        timer_.expires_after(period_);
        timer_.async_wait([this](const boost::system::error_code& ec) {
            if (ec) { 
                return;
            }
            const auto now = Clock::now();
            const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_);
            last_tick_ = now;
            handler_(delta);
            ScheduleTick();
        });
    }

    boost::asio::steady_timer timer_;
    std::chrono::milliseconds period_;
    Handler handler_;
    Clock::time_point last_tick_;
};

}  // namespace ticker
