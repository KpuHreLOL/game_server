#include "logger.h"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/keywords/auto_flush.hpp>

#include <iostream>
#include <string>

namespace logger {
namespace {

namespace pt = boost::posix_time;
namespace keywords = boost::log::keywords;
namespace trivial = boost::log::trivial;
namespace expressions = boost::log::expressions;

}  // namespace

void JsonFormatter(const logging::record_view& record,
                   logging::formatting_ostream& stream) {
    json::object result;

    if (auto timestamp = logging::extract<pt::ptime>("TimeStamp", record)) {
        result["timestamp"] = pt::to_iso_extended_string(timestamp.get());
    } else {
        result["timestamp"] = nullptr;
    }

    if (auto data = record[additional_data]) {
        result["data"] = data.get();
    } else {
        result["data"] = json::object{};
    }

    if (auto message = record[expressions::smessage]) {
        result["message"] = message.get();
    } else {
        result["message"] = "";
    }

    stream << result;
}

void InitLogging() {
    static bool initialized = false;
    if (initialized) {
        return;
    }

    auto sink = logging::add_console_log(std::cout,keywords::format = &JsonFormatter,keywords::auto_flush = true);
    (void)sink;
    logging::add_common_attributes();
    initialized = true;
}

void LogServerStarted(unsigned port, std::string_view address) {
    json::value data{{"port", port}, {"address", std::string(address)}};
    BOOST_LOG_TRIVIAL(info)
        << logging::add_value(additional_data, std::move(data))
        << "server started";
}

void LogServerExited(int code, std::string_view exception) {
    json::object data;
    data["code"] = code;
    if (!exception.empty()) {
        data["exception"] = std::string(exception);
    }

    BOOST_LOG_TRIVIAL(info)
        << logging::add_value(additional_data, json::value(std::move(data)))
        << "server exited";
}

void LogNetworkError(int code, std::string_view text, std::string_view where) {
    json::value data{
        {"code", code},
        {"text", std::string(text)},
        {"where", std::string(where)}};

    BOOST_LOG_TRIVIAL(error)
        << logging::add_value(additional_data, std::move(data))
        << "error";
}

void LogError(std::string_view text, std::string_view where) {
    json::value data{
        {"text", std::string(text)},
        {"where", std::string(where)}};

    BOOST_LOG_TRIVIAL(error)
        << logging::add_value(additional_data, std::move(data))
        << "error";
}

}  // namespace logger
