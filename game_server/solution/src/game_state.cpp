#include "game_state.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <istream>
#include <limits>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

namespace game_state {

namespace {

using namespace std::literals;

constexpr std::string_view kMagic = "DOG_GAME_STATE"sv;
constexpr int kFormatVersion = 2;
constexpr int kMinSupportedFormatVersion = 1;
constexpr std::size_t kMaxStringLength = 1u << 20;

void WriteString(std::ostream& out, std::string_view str) {
    out << str.size() << ':';
    out.write(str.data(), static_cast<std::streamsize>(str.size()));
}

class Reader {
public:
    explicit Reader(std::istream& in)
        : in_{in} {
    }

    void ExpectWord(std::string_view expected) {
        const std::string word = NextToken();
        if (word != expected) {
            Fail("expected \""s + std::string(expected) + "\", got \"" + Truncated(word) + "\"");
        }
    }

    template <typename T>
    T Number() {
        const std::string token = NextToken();
        T value{};
        const char* begin = token.data();
        const char* end = begin + token.size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc{} || ptr != end) {
            Fail("invalid number \"" + Truncated(token) + "\"");
        }
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(value)) {
                Fail("number is not finite");
            }
        }
        return value;
    }

    std::string String() {
        in_ >> std::ws;
        std::size_t length = 0;
        int digits = 0;
        int ch;
        while ((ch = in_.get()) != std::char_traits<char>::eof() && ch >= '0' && ch <= '9') {
            if (++digits > 9) {
                Fail("string length is too large");
            }
            length = length * 10 + static_cast<std::size_t>(ch - '0');
        }
        if (digits == 0 || ch != ':') {
            Fail("malformed string");
        }
        if (length > kMaxStringLength) {
            Fail("string is too long");
        }
        std::string result(length, '\0');
        in_.read(result.data(), static_cast<std::streamsize>(length));
        if (static_cast<std::size_t>(in_.gcount()) != length) {
            Fail("unexpected end of data inside a string");
        }
        return result;
    }

    void ExpectEnd() {
        in_ >> std::ws;
        if (in_.peek() != std::char_traits<char>::eof()) {
            Fail("unexpected data after the end of the state");
        }
    }

private:
    static std::string Truncated(const std::string& s) {
        return s.size() > 32 ? s.substr(0, 32) + "..." : s;
    }

    std::string NextToken() {
        std::string token;
        if (!(in_ >> token)) {
            Fail("unexpected end of data");
        }
        return token;
    }

    [[noreturn]] static void Fail(const std::string& message) {
        throw std::runtime_error("Corrupted state file: " + message);
    }

    std::istream& in_;
};

std::filesystem::path TempPathFor(const std::filesystem::path& path) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    return tmp;
}

}  // namespace

void WriteSnapshot(std::ostream& out, const model::GameSnapshot& snapshot) {
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);

    out << kMagic << ' ' << kFormatVersion << '\n';
    out << "next_player_id " << snapshot.next_player_id << '\n';

    out << "players " << snapshot.players.size() << '\n';
    for (const model::PlayerSnapshot& p : snapshot.players) {
        out << "player " << p.id << ' ';
        WriteString(out, p.name);
        out << ' ';
        WriteString(out, *p.map_id);
        out << ' ';
        WriteString(out, p.token);
        out << ' ' << p.position.x << ' ' << p.position.y << ' ' << p.speed.x << ' ' << p.speed.y
            << ' ';
        WriteString(out, p.direction);
        out << ' ' << p.score << ' ' << p.play_time.count() << ' ' << p.idle_time.count()
            << " bag " << p.bag.size();
        for (const model::BagItem& item : p.bag) {
            out << ' ' << item.id << ' ' << item.type;
        }
        out << '\n';
    }

    out << "loot " << snapshot.loot.size() << '\n';
    for (const model::MapLootSnapshot& l : snapshot.loot) {
        out << "map ";
        WriteString(out, *l.map_id);
        out << ' ' << l.next_id << ' ' << l.time_without_loot.count() << " objects "
            << l.objects.size();
        for (const model::LostObject& obj : l.objects) {
            out << ' ' << obj.id << ' ' << obj.type << ' ' << obj.position.x << ' '
                << obj.position.y;
        }
        out << '\n';
    }
    out << "end\n";
}

model::GameSnapshot ReadSnapshot(std::istream& in) {
    in.imbue(std::locale::classic());
    Reader reader{in};

    reader.ExpectWord(kMagic);
    const int version = reader.Number<int>();
    if (version < kMinSupportedFormatVersion || version > kFormatVersion) {
        throw std::runtime_error("Unsupported state file version: " + std::to_string(version));
    }

    model::GameSnapshot snapshot;
    reader.ExpectWord("next_player_id");
    snapshot.next_player_id = reader.Number<model::Player::Id>();

    reader.ExpectWord("players");
    const auto player_count = reader.Number<std::uint64_t>();
    for (std::uint64_t i = 0; i < player_count; ++i) {
        model::PlayerSnapshot p;
        reader.ExpectWord("player");
        p.id = reader.Number<model::Player::Id>();
        p.name = reader.String();
        p.map_id = model::Map::Id{reader.String()};
        p.token = reader.String();
        p.position.x = reader.Number<double>();
        p.position.y = reader.Number<double>();
        p.speed.x = reader.Number<double>();
        p.speed.y = reader.Number<double>();
        p.direction = reader.String();
        p.score = reader.Number<int>();
        if (version >= 2) {
            p.play_time = std::chrono::milliseconds{reader.Number<std::int64_t>()};
            p.idle_time = std::chrono::milliseconds{reader.Number<std::int64_t>()};
        }
        reader.ExpectWord("bag");
        const auto bag_size = reader.Number<std::uint64_t>();
        for (std::uint64_t j = 0; j < bag_size; ++j) {
            model::BagItem item;
            item.id = reader.Number<std::uint64_t>();
            item.type = reader.Number<std::size_t>();
            p.bag.push_back(item);
        }
        snapshot.players.push_back(std::move(p));
    }

    reader.ExpectWord("loot");
    const auto loot_count = reader.Number<std::uint64_t>();
    for (std::uint64_t i = 0; i < loot_count; ++i) {
        model::MapLootSnapshot l;
        reader.ExpectWord("map");
        l.map_id = model::Map::Id{reader.String()};
        l.next_id = reader.Number<model::LostObject::Id>();
        l.time_without_loot = std::chrono::milliseconds{reader.Number<std::int64_t>()};
        reader.ExpectWord("objects");
        const auto object_count = reader.Number<std::uint64_t>();
        for (std::uint64_t j = 0; j < object_count; ++j) {
            model::LostObject obj{};
            obj.id = reader.Number<model::LostObject::Id>();
            obj.type = reader.Number<std::size_t>();
            obj.position.x = reader.Number<double>();
            obj.position.y = reader.Number<double>();
            l.objects.push_back(obj);
        }
        snapshot.loot.push_back(std::move(l));
    }

    reader.ExpectWord("end");
    reader.ExpectEnd();
    return snapshot;
}

void SaveToFile(const model::Game& game, const std::filesystem::path& path) {
    namespace fs = std::filesystem;

    const model::GameSnapshot snapshot = game.TakeSnapshot();

    if (path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("Failed to create directory " + path.parent_path().string() + ": " + ec.message());
        }
    }

    const fs::path tmp_path = TempPathFor(path); {
        std::ofstream out{tmp_path, std::ios::binary | std::ios::trunc};
        if (!out) {
            throw std::runtime_error("Failed to open " + tmp_path.string() + " for writing");
        }
        WriteSnapshot(out, snapshot);
        out.flush();
        if (!out) {
            std::error_code ignored;
            fs::remove(tmp_path, ignored);
            throw std::runtime_error("Failed to write state to " + tmp_path.string());
        }
    }

    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp_path, ignored);
        throw std::runtime_error("Failed to move " + tmp_path.string() + " to " + path.string() + ": " + ec.message());
    }
}

bool RestoreFromFile(model::Game& game, const std::filesystem::path& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        throw std::runtime_error("Failed to access state file " + path.string() + ": " + ec.message());
    }
    if (!exists) {
        return false;
    }

    std::ifstream in{path, std::ios::binary};
    if (!in) {
        throw std::runtime_error("Failed to open state file " + path.string());
    }

    try {
        const model::GameSnapshot snapshot = ReadSnapshot(in);
        game.RestoreSnapshot(snapshot);
    } catch (const std::exception& ex) {
        throw std::runtime_error("Failed to restore state from " + path.string() + ": " + ex.what());
    }
    return true;
}

AutoSaver::AutoSaver(model::Game& game, std::filesystem::path path, std::optional<std::chrono::milliseconds> period, ErrorHandler on_error)
    : game_{game}
    , path_{std::move(path)}
    , period_{period}
    , on_error_{std::move(on_error)} {
}

void AutoSaver::Attach() {
    if (!period_) {
        return;
    }
    game_.SetTickListener([this](std::chrono::milliseconds delta) {
        OnTick(delta);
    });
}

void AutoSaver::Save() {
    std::lock_guard lock{mutex_};
    SaveToFile(game_, path_);
    time_since_save_ = std::chrono::milliseconds{0};
}

void AutoSaver::OnTick(std::chrono::milliseconds delta) {
    std::lock_guard lock{mutex_};
    time_since_save_ += delta;
    if (time_since_save_ < *period_) {
        return;
    }
    time_since_save_ = std::chrono::milliseconds{0};
    try {
        SaveToFile(game_, path_);
    } catch (const std::exception& ex) {
        if (on_error_) {
            on_error_(ex.what());
        }
    }
}

}  // namespace game_state
