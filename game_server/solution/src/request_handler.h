#pragma once

#include "extra_data.h"
#include "http_server.h"
#include "model.h"
#include "logger.h"
#include "records.h"

#include <boost/json.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace http_handler {

namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace logging = boost::log;
namespace fs = std::filesystem;

class Endpoints {
public:
    static constexpr std::string_view maps = "/api/v1/maps";
    static constexpr std::string_view maps_prefix = "/api/v1/maps/";
    static constexpr std::string_view game_join = "/api/v1/game/join";
    static constexpr std::string_view game_players = "/api/v1/game/players";
    static constexpr std::string_view game_state = "/api/v1/game/state";
    static constexpr std::string_view game_player_action = "/api/v1/game/player/action";
    static constexpr std::string_view game_tick = "/api/v1/game/tick";
    static constexpr std::string_view game_records = "/api/v1/game/records";
};

class RequestHandler {
public:
    RequestHandler(model::Game& game, const extra_data::MapExtraData& extra_data,
                   records::RecordsStorage& records_storage, fs::path static_root,
                   bool manual_tick = false)
        : game_{game}
        , extra_data_{extra_data}
        , records_{records_storage}
        , static_root_{fs::weakly_canonical(std::move(static_root))}
        , manual_tick_{manual_tick} {
        if (!fs::is_directory(static_root_)) {
            throw std::invalid_argument(
                "Static root is not a directory: " + static_root_.string());
        }
    }

    RequestHandler(const RequestHandler&) = delete;
    RequestHandler& operator=(const RequestHandler&) = delete;

    template <typename Body, typename Allocator, typename Send>
    void operator()(
        http::request<Body, http::basic_fields<Allocator>>&& req,
        Send&& send) {

        const std::string target = std::string(req.target());

        // Запрос рекордов содержит параметры в строке запроса, поэтому сравниваем только путь.
        if (target.substr(0, target.find('?')) == Endpoints::game_records) {
            HandleRecords(req, target, std::forward<Send>(send));
            return;
        }

        if (target == Endpoints::game_join) {
            HandleJoin(req, std::forward<Send>(send));
            return;
        }

        if (target == Endpoints::game_players) {
            HandlePlayers(req, std::forward<Send>(send));
            return;
        }

        if (target == Endpoints::game_state) {
            HandleState(req, std::forward<Send>(send));
            return;
        }

        if (target == Endpoints::game_player_action) {
            HandlePlayerAction(req, std::forward<Send>(send));
            return;
        }

        if (target == Endpoints::game_tick) {
            HandleTick(req, std::forward<Send>(send));
            return;
        }

        if (target.rfind("/api/", 0) != 0) {
            if (req.method() != http::verb::get &&
                req.method() != http::verb::head) {
                send(MakeApiResponse(
                    http::status::bad_request,
                    R"({"code":"badRequest","message":"Bad request"})",
                    req.version(),
                    req.keep_alive()));
                return;
            }

            send(ServeStatic(target, req.method() == http::verb::head,req.version(), req.keep_alive()));
            return;
        }

        if (target == Endpoints::maps || target.rfind(Endpoints::maps_prefix, 0) == 0) {
            HandleMaps(req, target, std::forward<Send>(send));
            return;
        }

        send(MakeApiResponse(
            http::status::bad_request,
            R"({"code":"badRequest","message":"Bad request"})",
            req.version(),
            req.keep_alive())
        );
    }

private:
    template <typename Body, typename Allocator, typename Send>
    void HandleMaps(
        const http::request<Body, http::basic_fields<Allocator>>& req,
        const std::string& target,
        Send&& send) {

        if (req.method() != http::verb::get &&
            req.method() != http::verb::head) {
            send(MakeApiResponse(
                http::status::method_not_allowed,
                R"({"code":"invalidMethod","message":"Invalid method"})",
                req.version(),
                req.keep_alive(),
                "GET, HEAD")
            );
            return;
        }

        const bool head = req.method() == http::verb::head;

        if (target == Endpoints::maps) {
            send(MakeHeadAwareResponse(http::status::ok,json::serialize(SerializeMaps()),head, req.version(), req.keep_alive()));
            return;
        }

        const std::string id = target.substr(Endpoints::maps_prefix.size());

        if (id.empty() ||
            id.find('/') != std::string::npos ||
            id.find('?') != std::string::npos ||
            id.find('#') != std::string::npos) {
            send(MakeHeadAwareResponse(
                http::status::bad_request,
                R"({"code":"badRequest","message":"Bad request"})",
                head, req.version(), req.keep_alive())
            );
            return;
        }

        const model::Map* map = game_.FindMap(model::Map::Id{id});
        if (!map) {
            send(MakeHeadAwareResponse(
                http::status::not_found,
                R"({"code":"mapNotFound","message":"Map not found"})",
                head, req.version(), req.keep_alive())
            );
            return;
        }

        send(MakeHeadAwareResponse(
            http::status::ok,
            json::serialize(SerializeMap(*map)),
            head, req.version(), req.keep_alive())
        );
    }

    static bool ParseQueryNumber(std::string_view query, std::string_view name,
                                 std::optional<std::size_t>& result) {
        while (!query.empty()) {
            const std::size_t amp = query.find('&');
            const std::string_view param = query.substr(0, amp);
            query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);

            const std::size_t eq = param.find('=');
            if (param.substr(0, eq) != name) {
                continue;
            }
            const std::string_view value =
                eq == std::string_view::npos ? std::string_view{} : param.substr(eq + 1);

            std::size_t number = 0;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size()) {
                return false;
            }
            result = number;
        }
        return true;
    }

    template <typename Body, typename Allocator, typename Send>
    void HandleRecords(
        const http::request<Body, http::basic_fields<Allocator>>& req,
        const std::string& target,
        Send&& send) {

        if (req.method() != http::verb::get &&
            req.method() != http::verb::head) {
            send(MakeApiResponse(
                http::status::method_not_allowed,
                R"({"code":"invalidMethod","message":"Invalid method"})",
                req.version(),
                req.keep_alive(),
                "GET, HEAD"));
            return;
        }
        const bool head = req.method() == http::verb::head;

        const auto bad_request = [&](std::string_view message) {
            json::object error{{"code", "badRequest"}, {"message", std::string(message)}};
            send(MakeHeadAwareResponse(http::status::bad_request, json::serialize(error), head,
                                       req.version(), req.keep_alive()));
        };

        const std::size_t query_pos = target.find('?');
        const std::string_view query =
            query_pos == std::string::npos ? std::string_view{}
                                           : std::string_view{target}.substr(query_pos + 1);

        std::optional<std::size_t> start;
        std::optional<std::size_t> max_items;
        if (!ParseQueryNumber(query, "start", start)) {
            bad_request("Invalid start parameter");
            return;
        }
        if (!ParseQueryNumber(query, "maxItems", max_items)) {
            bad_request("Invalid maxItems parameter");
            return;
        }
        if (max_items && *max_items > records::kMaxRecordsPerRequest) {
            bad_request("maxItems must not exceed 100");
            return;
        }

        try {
            const auto items = records_.Get(start.value_or(0),
                                            max_items.value_or(records::kMaxRecordsPerRequest));

            json::array response;
            response.reserve(items.size());
            for (const auto& item : items) {
                response.push_back(json::object{
                    {"name", item.name},
                    {"score", item.score},
                    {"playTime", std::chrono::duration<double>(item.play_time).count()}});
            }
            send(MakeHeadAwareResponse(http::status::ok, json::serialize(response), head,
                                       req.version(), req.keep_alive()));
        } catch (const std::exception& ex) {
            logger::LogError(ex.what(), "get records");
            send(MakeHeadAwareResponse(
                http::status::internal_server_error,
                R"({"code":"internalError","message":"Failed to get records"})",
                head, req.version(), req.keep_alive()));
        }
    }

    template <typename Body, typename Allocator, typename Send>
    void HandleTick(
        const http::request<Body, http::basic_fields<Allocator>>& req,
        Send&& send) {

        if (!manual_tick_) {
            send(MakeApiResponse(
                http::status::bad_request,
                R"({"code":"badRequest","message":"Invalid endpoint"})",
                req.version(),
                req.keep_alive()));
            return;
        }

        if (req.method() != http::verb::post) {
            send(MakeApiResponse(
                http::status::method_not_allowed,
                R"({"code":"invalidMethod","message":"Only POST method is expected"})",
                req.version(),
                req.keep_alive(),
                "POST"));
            return;
        }

        if (req.find(http::field::content_type) == req.end() ||
            !IsJsonContentType(req[http::field::content_type])) {
            send(MakeApiResponse(
                http::status::bad_request,
                R"({"code":"invalidArgument","message":"Invalid content type"})",
                req.version(),
                req.keep_alive()));
            return;
        }

        try {
            const json::value value = json::parse(req.body());
            const auto& time_delta = value.as_object().at("timeDelta");
            if (!time_delta.is_int64() || time_delta.as_int64() < 0) {
                throw std::invalid_argument("Invalid timeDelta");
            }

            game_.Tick(std::chrono::milliseconds{time_delta.as_int64()});

            send(MakeApiResponse(
                http::status::ok,
                "{}",
                req.version(),
                req.keep_alive()));
        } catch (const std::exception&) {
            send(MakeApiResponse(
                http::status::bad_request,
                R"({"code":"invalidArgument","message":"Failed to parse tick request JSON"})",
                req.version(),
                req.keep_alive()));
        }
    }

    template <typename Body, typename Allocator, typename Send>
    void HandleJoin(
        const http::request<Body, http::basic_fields<Allocator>>& req,
        Send&& send) {

        if (req.method() != http::verb::post) {
            send(MakeApiResponse(
                http::status::method_not_allowed,
                R"({"code":"invalidMethod","message":"Only POST method is expected"})",
                req.version(),
                req.keep_alive(),
                "POST"));
            return;
        }

        if (req.find(http::field::content_type) == req.end() ||
            !IsJsonContentType(req[http::field::content_type])) {
            send(MakeApiResponse(
                http::status::bad_request,
                R"({"code":"invalidArgument","message":"Join game request parse error"})",
                req.version(),
                req.keep_alive())
            );
            return;
        }

        try {
            const json::value value = json::parse(req.body());
            const auto& object = value.as_object();

            const auto& user_name_value = object.at("userName");
            const auto& map_id_value = object.at("mapId");

            const std::string user_name =
                std::string(user_name_value.as_string().c_str());
            const std::string map_id =
                std::string(map_id_value.as_string().c_str());

            if (user_name.empty()) {
                send(MakeApiResponse(
                    http::status::bad_request,
                    R"({"code":"invalidArgument","message":"Invalid name"})",
                    req.version(),
                    req.keep_alive())
                );
                return;
            }

            if (!game_.FindMap(model::Map::Id{map_id})) {
                send(MakeApiResponse(
                    http::status::not_found,
                    R"({"code":"mapNotFound","message":"Map not found"})",
                    req.version(),
                    req.keep_alive())
                );
                return;
            }

            const model::JoinedPlayer player =
                game_.JoinPlayer(user_name, model::Map::Id{map_id});

            json::object response;
            response["authToken"] = player.token;
            response["playerId"] = player.id;

            send(MakeApiResponse(
                http::status::ok,
                json::serialize(response),
                req.version(),
                req.keep_alive())
            );
        } catch (const std::exception&) {
            send(MakeApiResponse(
                http::status::bad_request,
                R"({"code":"invalidArgument","message":"Join game request parse error"})",
                req.version(),
                req.keep_alive()));
        }
    }

    template <typename Body, typename Allocator, typename Send>
    void HandleState(
        const http::request<Body, http::basic_fields<Allocator>>& req,
        Send&& send) {

        if (req.method() != http::verb::get &&
            req.method() != http::verb::head) {
            send(MakeApiResponse(
                http::status::method_not_allowed,
                R"({"code":"invalidMethod","message":"Invalid method"})",
                req.version(),
                req.keep_alive(),
                "GET, HEAD")
            );
            return;
        }

        const auto token = GetValidatedToken(req, send);
        if (!token) {
            return;
        }

        const auto player = game_.FindPlayerByToken(*token);
        if (!player) {
            send(MakeApiResponse(
                http::status::unauthorized,
                R"({"code":"unknownToken","message":"Player token has not been found"})",
                req.version(),
                req.keep_alive())
            );
            return;
        }

        const auto states = game_.GetPlayerStatesOnMap(player->map_id);
        json::object players;
        for (const auto& state : states) {
            json::array bag;
            for (const auto& item : state.bag) {
                bag.push_back(json::object{{"id", item.id}, {"type", item.type}});
            }
            players[std::to_string(state.id)] = json::object{
                {"pos", json::array{state.position.x, state.position.y}},
                {"speed", json::array{state.speed.x, state.speed.y}},
                {"dir", state.dir},
                {"bag", std::move(bag)},
                {"score", state.score}
            };
        }

        json::object lost_objects;
        for (const auto& obj : game_.GetLostObjectsOnMap(player->map_id)) {
            lost_objects[std::to_string(obj.id)] = json::object{
                {"type", obj.type},
                {"pos", json::array{obj.position.x, obj.position.y}}
            };
        }

        std::string body = json::serialize(json::object{
            {"players", std::move(players)},
            {"lostObjects", std::move(lost_objects)}
        });

        if (req.method() == http::verb::head) {
            auto head_response = MakeApiResponse(
                http::status::ok, std::string{}, req.version(), req.keep_alive());
            head_response.content_length(body.size());
            send(std::move(head_response));
        } else {
            send(MakeApiResponse(http::status::ok, std::move(body),req.version(), req.keep_alive()));
        }
    }

    template <typename Body, typename Allocator, typename Send>
    void HandlePlayerAction(
        const http::request<Body, http::basic_fields<Allocator>>& req,
        Send&& send) {

        if (req.method() != http::verb::post) {
            send(MakeApiResponse(
                http::status::method_not_allowed,
                R"({"code":"invalidMethod","message":"Invalid method"})",
                req.version(),
                req.keep_alive(),
                "POST"));
            return;
        }

        if (req.find(http::field::content_type) == req.end() ||
            !IsJsonContentType(req[http::field::content_type])) {
            send(MakeApiResponse(
                http::status::bad_request,
                R"({"code":"invalidArgument","message":"Invalid content type"})",
                req.version(),
                req.keep_alive()));
            return;
        }

        const auto authorization = req.find(http::field::authorization);
        if (authorization == req.end()) {
            send(MakeApiResponse(
                http::status::unauthorized,
                R"({"code":"invalidToken","message":"Authorization header is required"})",
                req.version(),
                req.keep_alive())
            );
            return;
        }

        const std::string header_value = std::string(authorization->value());
        constexpr std::string_view prefix = "Bearer ";
        if (header_value.size() != prefix.size() + 32 ||
            header_value.compare(0, prefix.size(), prefix) != 0 ||
            !IsHexToken(std::string_view(header_value).substr(prefix.size()))) {
            send(MakeApiResponse(
                http::status::unauthorized,
                R"({"code":"invalidToken","message":"Invalid authorization token"})",
                req.version(),
                req.keep_alive())
            );
            return;
        }

        const std::string token = header_value.substr(prefix.size());
        if (!game_.FindPlayerByToken(token)) {
            send(MakeApiResponse(
                http::status::unauthorized,
                R"({"code":"unknownToken","message":"Player token has not been found"})",
                req.version(),
                req.keep_alive()));
            return;
        }

        try {
            const json::value value = json::parse(req.body());
            const auto& object = value.as_object();
            const auto& move_value = object.at("move");
            const std::string move =
                std::string(move_value.as_string().c_str());

            model::Dog::Speed speed;
            if (move == "L") {
                speed.x = -GetPlayerMapSpeed(token);
            } else if (move == "R") {
                speed.x = GetPlayerMapSpeed(token);
            } else if (move == "U") {
                speed.y = -GetPlayerMapSpeed(token);
            } else if (move == "D") {
                speed.y = GetPlayerMapSpeed(token);
            } else if (!move.empty()) {
                throw std::invalid_argument("Invalid move");
            }

            if (!game_.SetPlayerSpeed(token, speed)) {
                send(MakeApiResponse(
                    http::status::unauthorized,
                    R"({"code":"unknownToken","message":"Player token has not been found"})",
                    req.version(),
                    req.keep_alive())
                );
                return;
            }

            send(MakeApiResponse(http::status::ok,"{}",req.version(),req.keep_alive()));
        } catch (const std::exception&) {
            send(MakeApiResponse(
                http::status::bad_request,
                R"({"code":"invalidArgument","message":"Failed to parse action"})",
                req.version(),
                req.keep_alive())
            );
        }
    }

    template <typename Body, typename Allocator, typename Send>
    void HandlePlayers(
        const http::request<Body, http::basic_fields<Allocator>>& req,
        Send&& send) {

        if (req.method() != http::verb::get &&
            req.method() != http::verb::head) {
            send(MakeApiResponse(
                http::status::method_not_allowed,
                R"({"code":"invalidMethod","message":"Invalid method"})",
                req.version(),
                req.keep_alive(),
                "GET, HEAD")
            );
            return;
        }

        const auto authorization = req.find(http::field::authorization);
        if (authorization == req.end()) {
            send(MakeApiResponse(
                http::status::unauthorized,
                R"({"code":"invalidToken","message":"Authorization header is missing"})",
                req.version(),
                req.keep_alive())
            );
            return;
        }

        const std::string header_value = std::string(authorization->value());
        constexpr std::string_view prefix = "Bearer ";
        if (header_value.size() != prefix.size() + 32 ||
            header_value.compare(0, prefix.size(), prefix) != 0 ||
            !IsHexToken(std::string_view(header_value).substr(prefix.size()))) {
            send(MakeApiResponse(
                http::status::unauthorized,
                R"({"code":"invalidToken","message":"Invalid authorization token"})",
                req.version(),
                req.keep_alive())
            );
            return;
        }

        const std::string token = header_value.substr(prefix.size());
        const auto player = game_.FindPlayerByToken(token);
        if (!player) {
            send(MakeApiResponse(
                http::status::unauthorized,
                R"({"code":"unknownToken","message":"Player token has not been found"})",
                req.version(),
                req.keep_alive())
            );
            return;
        }

        const auto map_id = player->map_id;
        const auto players = game_.GetPlayersOnMap(map_id);

        json::object response;
        for (const auto& item : players) {
            response[std::to_string(item.id)] =
                json::object{{"name", item.name}};
        }

        std::string body = json::serialize(response);
        if (req.method() == http::verb::head) {
            auto head_response = MakeApiResponse(
                http::status::ok, std::string{}, req.version(), req.keep_alive());
            head_response.content_length(body.size());
            send(std::move(head_response));
        } else {
            send(MakeApiResponse(http::status::ok, std::move(body),req.version(), req.keep_alive()));
        }
    }

    template <typename Body, typename Allocator, typename Send>
    std::optional<std::string> GetValidatedToken(
        const http::request<Body, http::basic_fields<Allocator>>& req,
        Send&& send) {

        const auto authorization = req.find(http::field::authorization);
        if (authorization == req.end()) {
            send(MakeApiResponse(
                http::status::unauthorized,
                R"({"code":"invalidToken","message":"Authorization header is required"})",
                req.version(),
                req.keep_alive())
            );
            return std::nullopt;
        }

        const std::string header_value = std::string(authorization->value());
        constexpr std::string_view prefix = "Bearer ";
        if (header_value.size() != prefix.size() + 32 ||
            header_value.compare(0, prefix.size(), prefix) != 0 ||
            !IsHexToken(std::string_view(header_value).substr(prefix.size()))) {
            send(MakeApiResponse(
                http::status::unauthorized,
                R"({"code":"invalidToken","message":"Invalid authorization token"})",
                req.version(),
                req.keep_alive())
            );
            return std::nullopt;
        }

        return header_value.substr(prefix.size());
    }

    double GetPlayerMapSpeed(const std::string& token) const {
        const auto player = game_.FindPlayerByToken(token);
        if (!player) {
            return 0.0;
        }

        const model::Map* map = game_.FindMap(player->map_id);
        if (!map) {
            return 0.0;
        }
        return map->GetDogSpeed();
    }

    static bool IsJsonContentType(beast::string_view value) {
        const std::string value_string(value.data(), value.size());
        const size_t semicolon = value_string.find(';');
        std::string_view media_type = std::string_view(value_string).substr(0, semicolon);

        while (!media_type.empty() && std::isspace(static_cast<unsigned char>(media_type.front()))) {
            media_type.remove_prefix(1);
        }
        while (!media_type.empty() && std::isspace(static_cast<unsigned char>(media_type.back()))) {
            media_type.remove_suffix(1);
        }

        if (media_type.size() != std::string_view{"application/json"}.size()) {
            return false;
        }

        return std::equal(
            media_type.begin(), media_type.end(),
            std::string_view{"application/json"}.begin(),
            [](char lhs, char rhs) {
                return std::tolower(static_cast<unsigned char>(lhs)) ==
                       std::tolower(static_cast<unsigned char>(rhs));
            });
    }

    static bool IsHexToken(std::string_view token) {
        if (token.size() != 32) {
            return false;
        }

        return std::all_of(token.begin(), token.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
    }
    struct DecodedTarget {
        std::string path;
        bool valid = false;
    };

    static int HexDigit(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    static DecodedTarget UrlDecodePath(std::string_view target) {
        const size_t query_pos = target.find('?');
        const size_t fragment_pos = target.find('#');

        size_t end = target.size();
        if (query_pos != std::string_view::npos) {
            end = query_pos;
        }
        if (fragment_pos != std::string_view::npos) {
            end = std::min(end, fragment_pos);
        }

        std::string decoded;
        decoded.reserve(end);

        for (size_t i = 0; i < end; ++i) {
            const char c = target[i];
            if (c != '%') {
                if (c == '\0') {
                    return {};
                }
                decoded.push_back(c);
                continue;
            }

            if (i + 2 >= end) {
                return {};
            }

            const int hi = HexDigit(target[i + 1]);
            const int lo = HexDigit(target[i + 2]);
            if (hi < 0 || lo < 0) {
                return {};
            }

            const char decoded_char = static_cast<char>((hi << 4) | lo);
            if (decoded_char == '\0') {
                return {};
            }

            decoded.push_back(decoded_char);
            i += 2;
        }

        return {std::move(decoded), true};
    }

    static bool IsPathInsideRoot(const fs::path& path, const fs::path& root) {
        const fs::path relative = path.lexically_relative(root);
        if (relative.empty() || relative.is_absolute()) {
            return false;
        }

        for (const auto& component : relative) {
            if (component == "..") {
                return false;
            }
        }
        return true;
    }

    static std::string GetMimeType(const fs::path& path) {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            }
        );

        if (extension == ".htm" || extension == ".html") {
            return "text/html";
        }    
        if (extension == ".css") {
            return "text/css";
        }    
        if (extension == ".txt") {
            return "text/plain";
        }    
        if (extension == ".js") {
            return "text/javascript";
        }
        if (extension == ".json") {
            return "application/json";
        }
        if (extension == ".xml") {
            return "application/xml";
        }
        if (extension == ".png") {
            return "image/png";
        }
        if (extension == ".jpg" || extension == ".jpe" || extension == ".jpeg") {
            return "image/jpeg";
        }
        if (extension == ".gif") {
            return "image/gif";
        }
        if (extension == ".bmp") {
            return "image/bmp";
        }
        if (extension == ".ico") {
            return "image/vnd.microsoft.icon";
        }
        if (extension == ".tiff" || extension == ".tif") {
            return "image/tiff";
        }
        if (extension == ".svg" || extension == ".svgz") {
            return "image/svg+xml";
        }
        if (extension == ".mp3") {
            return "audio/mpeg";
        }

        return "application/octet-stream";
    }

    http::response<http::string_body> ServeStatic(std::string_view target,bool head,unsigned version,bool keep_alive) const {

        const DecodedTarget decoded = UrlDecodePath(target);
        if (!decoded.valid || decoded.path.empty() || decoded.path.front() != '/') {
            return MakeTextResponse(
                http::status::bad_request,
                "Bad request",
                version,
                keep_alive,
                head
            );
        }

        fs::path relative_path = fs::path(decoded.path.substr(1));
        if (relative_path.has_root_path()) {
            return MakeTextResponse(
                http::status::bad_request,
                "Bad request",
                version,
                keep_alive,
                head
            );
        }

        std::error_code ec;
        fs::path requested_path = fs::weakly_canonical(static_root_ / relative_path, ec);
        if (ec || !IsPathInsideRoot(requested_path, static_root_)) {
            return MakeTextResponse(http::status::bad_request,"Bad request",version,keep_alive,head);
        }

        fs::path file_path = requested_path;
        if (fs::is_directory(file_path, ec)) {
            if (ec) {
                return MakeTextResponse(http::status::not_found,"File not found",version,keep_alive,head);
            }
            file_path /= "index.html";
        }

        file_path = fs::weakly_canonical(file_path, ec);
        if (ec || !IsPathInsideRoot(file_path, static_root_)) {
            return MakeTextResponse(http::status::bad_request,"Bad request",version,keep_alive,head);
        }

        if (!fs::is_regular_file(file_path, ec) || ec) {
            return MakeTextResponse(http::status::not_found,"File not found",version,keep_alive,head);
        }

        const auto file_size = fs::file_size(file_path, ec);
        if (ec) {
            return MakeTextResponse(http::status::not_found,"File not found",version,keep_alive,head);
        }

        http::response<http::string_body> response{http::status::ok, version};
        response.set(http::field::content_type, GetMimeType(file_path));
        response.keep_alive(keep_alive);

        if (head) {
            response.content_length(file_size);
            return response;
        }

        std::ifstream input(file_path, std::ios::binary);
        if (!input) {
            return MakeTextResponse(http::status::not_found,"File not found",version,keep_alive,false);
        }

        std::string body(static_cast<std::streamsize>(file_size), '\0');
        input.read(body.data(), static_cast<std::streamsize>(body.size()));
        if (!input && !input.eof()) {
            return MakeTextResponse(http::status::not_found,"File not found",version,keep_alive,false);
        }

        response.body() = std::move(body);
        response.prepare_payload();
        return response;
    }

    static http::response<http::string_body> MakeResponse(
        http::status status,
        std::string body,
        std::string content_type,
        unsigned version,
        bool keep_alive) {

        http::response<http::string_body> response{status, version};
        response.set(http::field::content_type, std::move(content_type));
        response.set(http::field::cache_control, "no-cache");
        response.keep_alive(keep_alive);
        response.body() = std::move(body);
        response.prepare_payload();
        return response;
    }

    static http::response<http::string_body> MakeTextResponse(
        http::status status,
        std::string body,
        unsigned version,
        bool keep_alive,
        bool head) {

        if (head) {
            http::response<http::string_body> response{status, version};
            response.set(http::field::content_type, "text/plain");
            response.keep_alive(keep_alive);
            response.content_length(body.size());
            return response;
        }

        return MakeResponse(
            status,
            std::move(body),
            "text/plain",
            version,
            keep_alive);
    }

    static http::response<http::string_body> MakeHeadAwareResponse(
        http::status status,
        std::string body,
        bool head,
        unsigned version,
        bool keep_alive) {

        if (!head) {
            return MakeApiResponse(status, std::move(body), version, keep_alive);
        }
        const auto size = body.size();
        auto response = MakeApiResponse(status, std::string{}, version, keep_alive);
        response.content_length(size);
        return response;
    }

    static http::response<http::string_body> MakeApiResponse(
        http::status status,
        std::string body,
        unsigned version,
        bool keep_alive,
        std::string_view allow = {}) {

        auto response = MakeResponse(status,std::move(body),"application/json",version,keep_alive);
        if (!allow.empty()) {
            response.set(http::field::allow,beast::string_view{allow.data(), allow.size()});
        }
        return response;
    }

    json::array SerializeMaps() const {
        json::array maps;

        for (const auto& map : game_.GetMaps()) {
            maps.emplace_back(SerializeMapInfo(map));
        }

        return maps;
    }

    static json::object SerializeMapInfo(const model::Map& map) {
        json::object item;
        item["id"] = *map.GetId();
        item["name"] = map.GetName();
        return item;
    }

    json::object SerializeMap(const model::Map& map) const {
        json::object item = SerializeMapInfo(map);

        item["roads"] = SerializeRoads(map);
        item["buildings"] = SerializeBuildings(map);
        item["offices"] = SerializeOffices(map);

        if (const auto* loot_types = extra_data_.GetLootTypes(map.GetId())) {
            item["lootTypes"] = *loot_types;
        }

        return item;
    }

    static json::array SerializeRoads(const model::Map& map) {
        json::array roads;

        for (const auto& road : map.GetRoads()) {
            roads.emplace_back(SerializeRoad(road));
        }

        return roads;
    }

    static json::object SerializeRoad(const model::Road& road) {
        json::object item;

        const auto start = road.GetStart();
        const auto end = road.GetEnd();

        item["x0"] = start.x;
        item["y0"] = start.y;

        if (road.IsHorizontal()) {
            item["x1"] = end.x;
        } else {
            item["y1"] = end.y;
        }

        return item;
    }

    static json::array SerializeBuildings(const model::Map& map) {
        json::array buildings;

        for (const auto& building : map.GetBuildings()) {
            buildings.emplace_back(SerializeBuilding(building));
        }

        return buildings;
    }

    static json::object SerializeBuilding(const model::Building& building) {

        const auto& bounds = building.GetBounds();

        json::object item;
        item["x"] = bounds.position.x;
        item["y"] = bounds.position.y;
        item["w"] = bounds.size.width;
        item["h"] = bounds.size.height;

        return item;
    }

    static json::array SerializeOffices(const model::Map& map) {
        json::array offices;

        for (const auto& office : map.GetOffices()) {
            offices.emplace_back(SerializeOffice(office));
        }

        return offices;
    }

    static json::object SerializeOffice(const model::Office& office) {
        const auto position = office.GetPosition();
        const auto offset = office.GetOffset();

        json::object item;
        item["id"] = *office.GetId();
        item["x"] = position.x;
        item["y"] = position.y;
        item["offsetX"] = offset.dx;
        item["offsetY"] = offset.dy;

        return item;
    }


    model::Game& game_;
    const extra_data::MapExtraData& extra_data_;
    records::RecordsStorage& records_;
    fs::path static_root_;
    bool manual_tick_ = false;
};

template <typename SomeRequestHandler>
class LoggingRequestHandler {
public:
    explicit LoggingRequestHandler(SomeRequestHandler& decorated)
        : decorated_{decorated} {
    }

    LoggingRequestHandler(const LoggingRequestHandler&) = delete;
    LoggingRequestHandler& operator=(const LoggingRequestHandler&) = delete;

    template <typename Body, typename Allocator, typename Send>
    void operator()(http::request<Body, http::basic_fields<Allocator>>&& req, Send&& send) {

        LogRequest(req);
        const auto started = std::chrono::steady_clock::now();
        const auto client_ip = http_server::ScopedClientIp::Get();

        auto logging_send =
            [this, started, client_ip, send = std::forward<Send>(send)]
            (auto&& response) mutable {
                LogResponse(response, started, client_ip);
                std::forward<Send>(send)(std::forward<decltype(response)>(response));
            };

        decorated_(std::move(req), std::move(logging_send));
    }

private:
    template <typename Body, typename Allocator>
    static void LogRequest(
        const http::request<Body, http::basic_fields<Allocator>>& req) {

        json::value data{
            {"ip", http_server::ScopedClientIp::Get()},
            {"URI", std::string(req.target())},
            {"method", std::string(http::to_string(req.method()))}};

        BOOST_LOG_TRIVIAL(info)
            << logging::add_value(logger::additional_data, std::move(data))
            << "request received";
    }

    template <typename Response>
    static void LogResponse(
        const Response& response,
        std::chrono::steady_clock::time_point started,
        std::string_view client_ip) {

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        json::object data;
        data["ip"] = std::string(client_ip);
        data["response_time"] = elapsed;
        data["code"] = response.result_int();

        if (auto content_type = response.find(http::field::content_type);
            content_type != response.end()) {
            data["content_type"] = std::string(content_type->value());
        } else {
            data["content_type"] = nullptr;
        }

        BOOST_LOG_TRIVIAL(info)
            << logging::add_value(logger::additional_data, json::value(std::move(data)))
            << "response sent";
    }

    SomeRequestHandler& decorated_;
};

}  // namespace http_handler
