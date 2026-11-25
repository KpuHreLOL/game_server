#include "sdk.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <iostream>
#include <thread>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "game_state.h"
#include "json_loader.h"
#include "request_handler.h"
#include "http_server.h"
#include "logger.h"
#include "records_db.h"
#include "ticker.h"

using namespace std::literals;
namespace net = boost::asio;

namespace {

constexpr unsigned kPort = 8080;
constexpr std::string_view kAddress = "0.0.0.0";
constexpr std::chrono::milliseconds kDefaultTickPeriod{100};
constexpr const char* kDbUrlEnvName = "GAME_DB_URL";

template <typename Fn>
void RunWorkers(unsigned n, const Fn& fn) {
    n = std::max(1u, n);
    std::vector<std::jthread> workers;
    workers.reserve(n - 1);
    while (--n) {
        workers.emplace_back(fn);
    }
    fn();
}

}  // namespace

int main(int argc, const char* argv[]) {
    constexpr auto kUsage =
        "Usage: game_server <game-config-json> <static-root> [--tick-period <milliseconds>] "
        "[--state-file <path>] [--save-state-period <milliseconds>]"sv;

    std::chrono::milliseconds tick_period = kDefaultTickPeriod;

    bool manual_tick = true;

    std::string state_file;
    std::optional<std::chrono::milliseconds> save_state_period;

    std::vector<std::string> positional;
    std::string config_path;
    std::string static_root;

    auto parse_tick = [&](const std::string& value) {
        try {
            tick_period = std::chrono::milliseconds{std::stoll(value)};
        } catch (const std::exception&) {
            return false;
        }
        if (tick_period.count() <= 0) {
            return false;
        }
        manual_tick = false;
        return true;
    };

    auto parse_save_period = [&](const std::string& value) {
        std::int64_t ms = 0;
        const char* end = value.data() + value.size();
        const auto [ptr, ec] = std::from_chars(value.data(), end, ms);
        if (value.empty() || ec != std::errc{} || ptr != end || ms < 0) {
            return false;
        }
        save_state_period = std::chrono::milliseconds{ms};
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_value = [&](std::string& out) {
            if (i + 1 < argc) {
                out = argv[++i];
                return true;
            }
            return false;
        };
        auto value_of = [&](std::string_view name, std::string_view short_name,std::string& out) {
            if (arg == name || (!short_name.empty() && arg == short_name)) {
                return next_value(out);
            }
            const std::string prefix = std::string(name) + "=";
            if (arg.rfind(prefix, 0) == 0) {
                out = arg.substr(prefix.size());
                return true;
            }
            return false;
        };

        std::string value;
        if (value_of("--tick-period", "-t", value)) {
            if (!parse_tick(value)) {
                std::cerr << kUsage << std::endl;
                return EXIT_FAILURE;
            }
        } else if (value_of("--state-file", "", value)) {
            if (value.empty()) {
                std::cerr << kUsage << std::endl;
                return EXIT_FAILURE;
            }
            state_file = value;
        } else if (value_of("--save-state-period", "", value)) {
            if (!parse_save_period(value)) {
                std::cerr << kUsage << std::endl;
                return EXIT_FAILURE;
            }
        } else if (value_of("--config-file", "-c", value)) {
            config_path = value;
        } else if (value_of("--www-root", "-w", value)) {
            static_root = value;
        } else if (arg == "--tick-period" || arg == "-t" || arg == "--config-file" ||
                   arg == "-c" || arg == "--www-root" || arg == "-w" || arg == "--state-file" ||
                   arg == "--save-state-period") {
            std::cerr << kUsage << std::endl;
            return EXIT_FAILURE;
        } else if (!arg.empty() && arg[0] == '-') {

            continue;
        } else {
            positional.push_back(arg);
        }
    }

    if (config_path.empty() && !positional.empty()) {
        config_path = positional[0];
    }
    if (static_root.empty() && positional.size() > 1) {
        static_root = positional[1];
    }
    if (config_path.empty() || static_root.empty()) {
        std::cerr << kUsage << std::endl;
        return EXIT_FAILURE;
    }

    logger::InitLogging();

    try {
        auto loaded = json_loader::LoadGame(config_path);
        model::Game& game = loaded.game;
        const extra_data::MapExtraData& extra_data = loaded.extra_data;

        const unsigned num_threads = std::max(1u, std::thread::hardware_concurrency());
        net::io_context ioc(static_cast<int>(num_threads));

        // Таблица рекордов хранится в PostgreSQL; адрес базы данных задаётся переменной окружения.
        const char* db_url = std::getenv(kDbUrlEnvName);
        if (db_url == nullptr || *db_url == '\0') {
            throw std::runtime_error(std::string(kDbUrlEnvName) + " environment variable is not set");
        }
        // Соединений столько же, сколько рабочих потоков: больше одновременных запросов не будет.
        records_db::PostgresRecordsStorage records_storage{db_url, num_threads};

        // Ушедших на покой игроков сразу записываем в таблицу рекордов. Ошибка базы данных
        // не должна останавливать игру, поэтому она только логируется.
        game.SetRetirementListener([&records_storage](const model::RetiredPlayer& player) {
            try {
                records_storage.Add({player.name, player.score, player.play_time});
            } catch (const std::exception& ex) {
                logger::LogError(ex.what(), "save retired player");
            }
        });

        // Сохранение состояния включается только при заданном --state-file;
        // --save-state-period без него игнорируется.
        std::optional<game_state::AutoSaver> state_saver;
        if (!state_file.empty()) {
            try {
                game_state::RestoreFromFile(game, state_file);
            } catch (const std::exception& ex) {
                logger::LogError(ex.what(), "restore state");
                logger::LogServerExited(EXIT_FAILURE, ex.what());
                return EXIT_FAILURE;
            }
            state_saver.emplace(game, state_file, save_state_period,
                                [](std::string_view message) {
                                    logger::LogError(message, "save state");
                                });
            // Автосохранение по ходу игровых часов: и для /api/v1/game/tick, и для --tick-period.
            state_saver->Attach();
        }

        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&ioc](const boost::system::error_code&, int) {
            ioc.stop();
        });

        std::optional<ticker::Ticker> ticker;
        if (!manual_tick) {
            ticker.emplace(ioc, tick_period, [&game](std::chrono::milliseconds delta) {
                game.Tick(delta);
            });
            ticker->Start();
        }

        http_handler::RequestHandler handler{game, extra_data, records_storage, static_root, manual_tick};
        http_handler::LoggingRequestHandler logging_handler{handler};

        const auto address = net::ip::make_address(kAddress);
        http_server::ServeHttp(ioc, {address, kPort},[&logging_handler](auto&& req, auto&& send) {
                                    logging_handler(
                                        std::forward<decltype(req)>(req),
                                        std::forward<decltype(send)>(send));
        });

        logger::LogServerStarted(kPort, kAddress);

        RunWorkers(num_threads, [&ioc] {
            ioc.run();
        });

        // Рабочие потоки завершены, поэтому состояние больше никто не меняет.
        if (state_saver) {
            try {
                state_saver->Save();
            } catch (const std::exception& ex) {
                logger::LogError(ex.what(), "save state");
                logger::LogServerExited(EXIT_FAILURE, ex.what());
                return EXIT_FAILURE;
            }
        }

        logger::LogServerExited(EXIT_SUCCESS);
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        logger::LogServerExited(EXIT_FAILURE, ex.what());
        return EXIT_FAILURE;
    }
}
