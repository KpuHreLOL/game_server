#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

#include "../src/game_state.h"
#include "../src/model.h"

using namespace std::literals;

namespace {

model::Map MakeMap(const std::string& id, size_t loot_types) {
    model::Map map{model::Map::Id{id}, "Map "s + id};
    map.AddRoad(model::Road{model::Road::HORIZONTAL, model::Point{0, 0}, 40});
    map.AddRoad(model::Road{model::Road::VERTICAL, model::Point{40, 0}, 30});
    map.SetLootTypeCount(loot_types);
    map.SetLootValues(std::vector<int>(loot_types, 10));
    return map;
}

void Setup(model::Game& game) {
    game.AddMap(MakeMap("map1", 3));
    game.AddMap(MakeMap("map2", 2));
    game.SetLootGeneratorConfig(1s, 1.0, [] { return 1.0; });
}

std::string Serialize(const model::Game& game) {
    std::ostringstream out;
    game_state::WriteSnapshot(out, game.TakeSnapshot());
    return out.str();
}

// Создаёт временную директорию, которая удаляется в конце теста.
struct TempDir {
    TempDir() {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("game_state_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};

}  // namespace

TEST_CASE("Game state survives a save/restore round trip", "[state]") {
    model::Game game;
    Setup(game);

    const auto a = game.JoinPlayer("Rex Ward", model::Map::Id{"map1"});
    const auto b = game.JoinPlayer("Лайка: 1 2 3", model::Map::Id{"map2"});
    REQUIRE(game.SetPlayerSpeed(a.token, {1.5, 0.0}));
    game.AddLostObject(model::Map::Id{"map1"}, 2, {1.0 / 3.0, 0.0});
    game.AddLostObject(model::Map::Id{"map1"}, 0, {20.0, 0.0});
    game.Tick(300ms);
    game.Tick(1s);

    const std::string saved = Serialize(game);

    model::Game restored;
    Setup(restored);
    std::istringstream in{saved};
    restored.RestoreSnapshot(game_state::ReadSnapshot(in));

    CHECK(Serialize(restored) == saved);

    const auto info_a = restored.FindPlayerByToken(a.token);
    REQUIRE(info_a);
    CHECK(info_a->id == a.id);
    CHECK(info_a->name == "Rex Ward");
    const auto info_b = restored.FindPlayerByToken(b.token);
    REQUIRE(info_b);
    CHECK(info_b->name == "Лайка: 1 2 3");
    CHECK(info_b->map_id == model::Map::Id{"map2"});

    CHECK(restored.GetLostObjectsOnMap(model::Map::Id{"map1"}).size() ==
          game.GetLostObjectsOnMap(model::Map::Id{"map1"}).size());

    // Идентификаторы не должны повторяться после восстановления.
    const auto c = restored.JoinPlayer("New", model::Map::Id{"map1"});
    CHECK(c.id > b.id);
}

TEST_CASE("Restoring into the game replaces its dynamic state", "[state]") {
    model::Game source;
    Setup(source);
    source.JoinPlayer("A", model::Map::Id{"map1"});

    model::Game target;
    Setup(target);
    const auto old_player = target.JoinPlayer("Old", model::Map::Id{"map1"});
    target.AddLostObject(model::Map::Id{"map2"}, 1, {1, 0});

    target.RestoreSnapshot(source.TakeSnapshot());
    CHECK_FALSE(target.FindPlayerByToken(old_player.token));
    CHECK(target.GetLostObjectsOnMap(model::Map::Id{"map2"}).empty());
    CHECK(target.GetPlayersOnMap(model::Map::Id{"map1"}).size() == 1);
}

TEST_CASE("Corrupted state is rejected", "[state]") {
    model::Game game;
    Setup(game);
    game.JoinPlayer("A", model::Map::Id{"map1"});
    game.AddLostObject(model::Map::Id{"map1"}, 1, {2, 0});
    const std::string good = Serialize(game);

    auto parse = [](const std::string& text) {
        std::istringstream in{text};
        return game_state::ReadSnapshot(in);
    };

    SECTION("truncated file") {
        for (size_t len : {size_t{0}, size_t{5}, good.size() / 2, good.size() - 5}) {
            CHECK_THROWS_AS(parse(good.substr(0, len)), std::runtime_error);
        }
    }
    SECTION("garbage") {
        CHECK_THROWS_AS(parse("hello world"), std::runtime_error);
        CHECK_THROWS_AS(parse(good + " junk"), std::runtime_error);
    }
    SECTION("unsupported version") {
        std::string bad = good;
        bad.replace(bad.find(" 2\n"), 3, " 99\n");
        CHECK_THROWS_AS(parse(bad), std::runtime_error);
    }
    SECTION("non-numeric and non-finite numbers") {
        std::string bad = good;
        bad.replace(bad.find("next_player_id 1"), 16, "next_player_id x");
        CHECK_THROWS_AS(parse(bad), std::runtime_error);

        std::string bad_pos = good;
        bad_pos.replace(bad_pos.find("objects 1 0 1 ") + 14, 1, "nan");
        CHECK_THROWS_AS(parse(bad_pos), std::runtime_error);
    }
}

TEST_CASE("Inconsistent state is rejected and leaves the game untouched", "[state]") {
    model::Game game;
    Setup(game);
    const auto keep = game.JoinPlayer("Keep", model::Map::Id{"map1"});

    model::Game donor;
    Setup(donor);
    donor.JoinPlayer("A", model::Map::Id{"map1"});
    donor.JoinPlayer("B", model::Map::Id{"map1"});
    donor.AddLostObject(model::Map::Id{"map1"}, 1, {2, 0});
    const model::GameSnapshot good = donor.TakeSnapshot();

    auto check_rejected = [&](model::GameSnapshot bad) {
        CHECK_THROWS_AS(game.RestoreSnapshot(bad), std::invalid_argument);
        CHECK(game.FindPlayerByToken(keep.token));
    };

    SECTION("unknown map") {
        auto bad = good;
        bad.players[0].map_id = model::Map::Id{"nope"};
        check_rejected(bad);
    }
    SECTION("duplicate tokens") {
        auto bad = good;
        bad.players[1].token = bad.players[0].token;
        check_rejected(bad);
    }
    SECTION("duplicate ids") {
        auto bad = good;
        bad.players[1].id = bad.players[0].id;
        check_rejected(bad);
    }
    SECTION("id not below next_player_id") {
        auto bad = good;
        bad.next_player_id = 1;
        check_rejected(bad);
    }
    SECTION("bad direction") {
        auto bad = good;
        bad.players[0].direction = "X";
        check_rejected(bad);
    }
    SECTION("unknown loot type") {
        auto bad = good;
        bad.loot[0].objects[0].type = 100;
        check_rejected(bad);
    }
    SECTION("loot id not below next_id") {
        auto bad = good;
        bad.loot[0].next_id = 0;
        check_rejected(bad);
    }
}

TEST_CASE("State file helpers", "[state]") {
    TempDir dir;
    const auto file = dir.path / "sub" / "state.dat";

    model::Game game;
    Setup(game);

    SECTION("missing file means a clean start") {
        CHECK_FALSE(game_state::RestoreFromFile(game, file));
    }
    SECTION("save then restore, no temp files left behind") {
        const auto p = game.JoinPlayer("A", model::Map::Id{"map1"});
        game_state::SaveToFile(game, file);
        CHECK(std::filesystem::exists(file));
        CHECK_FALSE(std::filesystem::exists(file.string() + ".tmp"));

        model::Game other;
        Setup(other);
        CHECK(game_state::RestoreFromFile(other, file));
        CHECK(other.FindPlayerByToken(p.token));
    }
    SECTION("corrupted file is an error") {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream{file} << "not a state";
        CHECK_THROWS_AS(game_state::RestoreFromFile(game, file), std::runtime_error);
    }
}

TEST_CASE("AutoSaver saves in step with game time", "[state]") {
    TempDir dir;
    const auto file = dir.path / "state.dat";

    model::Game game;
    Setup(game);

    SECTION("period set") {
        game_state::AutoSaver saver{game, file, 1000ms, nullptr};
        saver.Attach();

        game.Tick(400ms);
        game.Tick(500ms);
        CHECK_FALSE(std::filesystem::exists(file));
        game.Tick(100ms);  // накопилось ровно 1000 мс
        CHECK(std::filesystem::exists(file));

        std::filesystem::remove(file);
        game.Tick(999ms);
        CHECK_FALSE(std::filesystem::exists(file));
        game.Tick(1ms);
        CHECK(std::filesystem::exists(file));
    }
    SECTION("no period: ticks never save") {
        game_state::AutoSaver saver{game, file, std::nullopt, nullptr};
        saver.Attach();
        game.Tick(10s);
        CHECK_FALSE(std::filesystem::exists(file));
        saver.Save();
        CHECK(std::filesystem::exists(file));
    }
    SECTION("save errors are reported, not thrown, during ticks") {
        // Путь, родитель которого - обычный файл, создать невозможно.
        std::ofstream{file} << "x";
        std::string error;
        game_state::AutoSaver saver{game, file / "state.dat", 1ms,
                                    [&](std::string_view m) { error = std::string(m); }};
        saver.Attach();
        CHECK_NOTHROW(game.Tick(5ms));
        CHECK_FALSE(error.empty());
    }
}
