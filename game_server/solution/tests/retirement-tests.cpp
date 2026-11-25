#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "../src/game_state.h"
#include "../src/model.h"

using namespace std::literals;

namespace {

const model::Map::Id kMapId{"map1"};

model::Map MakeMap() {
    model::Map map{kMapId, "Map 1"};
    // Дорога от 0 до 10 по X; собака, идущая вправо, остановится в точке x = 10.4.
    map.AddRoad(model::Road{model::Road::HORIZONTAL, model::Point{0, 0}, 10});
    return map;
}

struct Fixture {
    Fixture() {
        game.AddMap(MakeMap());
        game.SetDogRetirementTime(5s);
        game.SetRetirementListener([this](const model::RetiredPlayer& player) {
            retired.push_back(player);
        });
    }

    model::Game game;
    std::vector<model::RetiredPlayer> retired;
};

}  // namespace

TEST_CASE("Retirement time defaults to one minute and rejects negative values", "[retirement]") {
    model::Game game;
    CHECK(game.GetDogRetirementTime() == 60s);
    game.SetDogRetirementTime(10s);
    CHECK(game.GetDogRetirementTime() == 10s);
    CHECK_THROWS_AS(game.SetDogRetirementTime(-1s), std::invalid_argument);
}

TEST_CASE("A dog that never moves retires after the retirement time", "[retirement]") {
    Fixture f;
    const auto player = f.game.JoinPlayer("Rex", kMapId);

    f.game.Tick(4999ms);
    CHECK(f.retired.empty());
    CHECK(f.game.FindPlayerByToken(player.token));

    f.game.Tick(1ms);
    REQUIRE(f.retired.size() == 1);
    CHECK(f.retired[0].name == "Rex");
    CHECK(f.retired[0].score == 0);
    CHECK(f.retired[0].play_time == 5000ms);

    // Токен аннулирован, игрок исчез с карты.
    CHECK_FALSE(f.game.FindPlayerByToken(player.token));
    CHECK(f.game.GetPlayersOnMap(kMapId).empty());
    CHECK_FALSE(f.game.SetPlayerSpeed(player.token, {1.0, 0.0}));

    // Повторно в таблицу рекордов он не попадает.
    f.game.Tick(10s);
    CHECK(f.retired.size() == 1);
}

TEST_CASE("Play time stops at the moment of retirement", "[retirement]") {
    Fixture f;
    f.game.JoinPlayer("Rex", kMapId);

    f.game.Tick(1h);
    REQUIRE(f.retired.size() == 1);
    CHECK(f.retired[0].play_time == 5s);
}

TEST_CASE("Idle time starts when the dog stops at the road edge", "[retirement]") {
    Fixture f;
    const auto player = f.game.JoinPlayer("Rex", kMapId);
    REQUIRE(f.game.SetPlayerSpeed(player.token, {1.0, 0.0}));

    // Собака идёт 10,4 с до края дороги и стоит оставшиеся 4,6 с.
    f.game.Tick(15s);
    CHECK(f.retired.empty());

    // До ухода на покой осталось 0,4 с.
    f.game.Tick(399ms);
    CHECK(f.retired.empty());
    f.game.Tick(1ms);
    REQUIRE(f.retired.size() == 1);
    // Вошла в игру в 0, остановилась в 10,4 с, вышла на покой в 15,4 с.
    CHECK(f.retired[0].play_time == 15400ms);
}

TEST_CASE("Retirement can happen within the same tick as the stop", "[retirement]") {
    Fixture f;
    const auto player = f.game.JoinPlayer("Rex", kMapId);
    REQUIRE(f.game.SetPlayerSpeed(player.token, {1.0, 0.0}));

    f.game.Tick(20s);
    REQUIRE(f.retired.size() == 1);
    CHECK(f.retired[0].play_time == 15400ms);
}

TEST_CASE("Moving and stopping commands restart the idle time", "[retirement]") {
    Fixture f;
    const auto player = f.game.JoinPlayer("Rex", kMapId);

    f.game.Tick(4s);  // стоит с момента входа
    REQUIRE(f.game.SetPlayerSpeed(player.token, {1.0, 0.0}));
    f.game.Tick(3s);  // движется
    REQUIRE(f.game.SetPlayerSpeed(player.token, {0.0, 0.0}));
    f.game.Tick(4999ms);  // стоит почти 5 с
    CHECK(f.retired.empty());
    REQUIRE(f.game.SetPlayerSpeed(player.token, {-1.0, 0.0}));
    f.game.Tick(1s);  // снова движется, отсчёт сброшен
    REQUIRE(f.game.SetPlayerSpeed(player.token, {0.0, 0.0}));
    f.game.Tick(4999ms);
    CHECK(f.retired.empty());
    f.game.Tick(1ms);

    REQUIRE(f.retired.size() == 1);
    CHECK(f.retired[0].play_time == (4s + 3s + 4999ms + 1s + 5s));
}

TEST_CASE("Repeated stop command does not restart the idle time", "[retirement]") {
    Fixture f;
    const auto player = f.game.JoinPlayer("Rex", kMapId);

    f.game.Tick(3s);
    REQUIRE(f.game.SetPlayerSpeed(player.token, {0.0, 0.0}));
    f.game.Tick(2s);

    REQUIRE(f.retired.size() == 1);
    CHECK(f.retired[0].play_time == 5s);
}

TEST_CASE("A moving dog never retires", "[retirement]") {
    Fixture f;
    const auto player = f.game.JoinPlayer("Rex", kMapId);

    for (int i = 0; i < 10; ++i) {
        // Собака ходит туда-обратно, ни разу не простояв дольше 5 с.
        REQUIRE(f.game.SetPlayerSpeed(player.token, {i % 2 == 0 ? 1.0 : -1.0, 0.0}));
        f.game.Tick(4s);
    }
    CHECK(f.retired.empty());
    CHECK(f.game.FindPlayerByToken(player.token));
}

TEST_CASE("Retired dog keeps the score it collected", "[retirement]") {
    model::Game game;
    auto map = MakeMap();
    map.SetLootTypeCount(1);
    map.SetLootValues({25});
    map.AddOffice(model::Office{model::Office::Id{"o"}, model::Point{10, 0}, model::Offset{0, 0}});
    game.AddMap(std::move(map));
    game.SetDogRetirementTime(5s);
    std::vector<model::RetiredPlayer> retired;
    game.SetRetirementListener([&](const model::RetiredPlayer& p) { retired.push_back(p); });

    const auto player = game.JoinPlayer("Rex", kMapId);
    game.AddLostObject(kMapId, 0, {5.0, 0.0});
    REQUIRE(game.SetPlayerSpeed(player.token, {1.0, 0.0}));
    game.Tick(11s);  // подобрал трофей, сдал его в бюро находок и остановился на краю дороги

    REQUIRE(game.GetPlayerStatesOnMap(kMapId).size() == 1);
    CHECK(game.GetPlayerStatesOnMap(kMapId)[0].score == 25);

    game.Tick(10s);
    REQUIRE(retired.size() == 1);
    CHECK(retired[0].score == 25);
}

TEST_CASE("Other players are unaffected when someone retires", "[retirement]") {
    Fixture f;
    const auto idle = f.game.JoinPlayer("Idle", kMapId);
    const auto walker = f.game.JoinPlayer("Walker", kMapId);
    const auto last = f.game.JoinPlayer("Last", kMapId);
    REQUIRE(f.game.SetPlayerSpeed(walker.token, {0.0, 0.0}));

    f.game.Tick(3s);
    REQUIRE(f.game.SetPlayerSpeed(walker.token, {0.1, 0.0}));
    REQUIRE(f.game.SetPlayerSpeed(last.token, {0.1, 0.0}));
    f.game.Tick(2s);

    REQUIRE(f.retired.size() == 1);
    CHECK(f.retired[0].name == "Idle");
    CHECK_FALSE(f.game.FindPlayerByToken(idle.token));

    const auto walker_info = f.game.FindPlayerByToken(walker.token);
    const auto last_info = f.game.FindPlayerByToken(last.token);
    REQUIRE(walker_info);
    REQUIRE(last_info);
    CHECK(walker_info->name == "Walker");
    CHECK(last_info->name == "Last");
    CHECK(f.game.SetPlayerSpeed(last.token, {0.0, 0.0}));
}

TEST_CASE("Retirement listener may use the game", "[retirement]") {
    model::Game game;
    game.AddMap(MakeMap());
    game.SetDogRetirementTime(1s);
    std::size_t players_left = 100;
    game.SetRetirementListener([&](const model::RetiredPlayer&) {
        players_left = game.GetPlayersOnMap(kMapId).size();  // не должно блокироваться
    });
    game.JoinPlayer("Rex", kMapId);
    game.Tick(1s);
    CHECK(players_left == 0);
}

TEST_CASE("Play and idle times survive a save/restore round trip", "[retirement][state]") {
    Fixture f;
    const auto player = f.game.JoinPlayer("Rex", kMapId);
    f.game.Tick(3s);

    std::ostringstream out;
    game_state::WriteSnapshot(out, f.game.TakeSnapshot());

    model::Game restored;
    restored.AddMap(MakeMap());
    restored.SetDogRetirementTime(5s);
    std::vector<model::RetiredPlayer> retired;
    restored.SetRetirementListener([&](const model::RetiredPlayer& p) { retired.push_back(p); });
    std::istringstream in{out.str()};
    restored.RestoreSnapshot(game_state::ReadSnapshot(in));

    REQUIRE(restored.FindPlayerByToken(player.token));
    restored.Tick(1999ms);
    CHECK(retired.empty());
    restored.Tick(1ms);
    REQUIRE(retired.size() == 1);
    CHECK(retired[0].play_time == 5s);
}

TEST_CASE("State files without times (format version 1) are still readable", "[retirement][state]") {
    const std::string v1 =
        "DOG_GAME_STATE 1\n"
        "next_player_id 1\n"
        "players 1\n"
        "player 0 3:Rex 4:map1 3:abc 0 0 0 0 1:U 7 bag 0\n"
        "loot 0\n"
        "end\n";
    std::istringstream in{v1};
    const auto snapshot = game_state::ReadSnapshot(in);
    REQUIRE(snapshot.players.size() == 1);
    CHECK(snapshot.players[0].score == 7);
    CHECK(snapshot.players[0].play_time == 0ms);
    CHECK(snapshot.players[0].idle_time == 0ms);
}
