#pragma once

#include <boost/json.hpp>
#include <boost/log/keywords/format.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/expressions/keyword.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/manipulators/add_value.hpp>

#include <string_view>

namespace logger {

namespace json = boost::json;
namespace logging = boost::log;

BOOST_LOG_ATTRIBUTE_KEYWORD(additional_data, "AdditionalData", json::value)

void InitLogging();

void JsonFormatter(const logging::record_view& record,logging::formatting_ostream& stream);

void LogServerStarted(unsigned port, std::string_view address);
void LogServerExited(int code, std::string_view exception = {});
void LogNetworkError(int code, std::string_view text, std::string_view where);
void LogError(std::string_view text, std::string_view where);

}  // namespace logger
