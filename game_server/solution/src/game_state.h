#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string_view>

#include "model.h"

namespace game_state {

void WriteSnapshot(std::ostream& out, const model::GameSnapshot& snapshot);

model::GameSnapshot ReadSnapshot(std::istream& in);

void SaveToFile(const model::Game& game, const std::filesystem::path& path);

bool RestoreFromFile(model::Game& game, const std::filesystem::path& path);

class AutoSaver {
public:
    using ErrorHandler = std::function<void(std::string_view message)>;

    AutoSaver(model::Game& game, std::filesystem::path path, std::optional<std::chrono::milliseconds> period, ErrorHandler on_error);

    AutoSaver(const AutoSaver&) = delete;
    AutoSaver& operator=(const AutoSaver&) = delete;

    void Attach();

    void Save();

private:
    void OnTick(std::chrono::milliseconds delta);

    model::Game& game_;
    std::filesystem::path path_;
    std::optional<std::chrono::milliseconds> period_;
    ErrorHandler on_error_;

    std::mutex mutex_;  
    std::chrono::milliseconds time_since_save_{0};
};

}  // namespace game_state
