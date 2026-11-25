#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <set>

#include "../src/model.h"

using namespace std::literals;

namespace {

model::Map MakeMap(const std::string& id, size_t loot_types) {
    model::Map map{model::Map::Id{id}, "Map "s + id};
    map.AddRoad(model::Road{model::Road::HORIZONTAL, model::Point{0, 0}, 10});
    map.AddRoad(model::Road{model::Road::VERTICAL, model::Point{10, 0}, 20});
    map.SetLootTypeCount(loot_types);
    return map;
}

bool IsOnSomeRoad(const model::Map& map, model::Dog::Position pos) {
    return std::any_of(map.GetRoads().begin(), map.GetRoads().end(), [&](const model::Road& r) {
        const auto s = r.GetStart();
        const auto e = r.GetEnd();
        const double min_x = std::min(s.x, e.x), max_x = std::max(s.x, e.x);
        const double min_y = std::min(s.y, e.y), max_y = std::max(s.y, e.y);
        return pos.x >= min_x && pos.x <= max_x && pos.y >= min_y && pos.y <= max_y;
    });
}

void ConfigureDeterministic(model::Game& game) {
    game.SetLootGeneratorConfig(1s, 1.0, [] { return 1.0; });
}

}  // namespace

TEST_CASE("Map keeps loot type count", "[model]") {
    model::Map map{model::Map::Id{"m"}, "m"};
    CHECK(map.GetLootTypeCount() == 0);
    map.SetLootTypeCount(7);
    CHECK(map.GetLootTypeCount() == 7);
}

TEST_CASE("Loot generation in game", "[model][loot]") {
    model::Game game;
    game.AddMap(MakeMap("map1", 3));
    const model::Map::Id map_id{"map1"};

    SECTION("no loot without generator config") {
        game.JoinPlayer("Rex", map_id);
        game.Tick(10s);
        CHECK(game.GetLostObjectsOnMap(map_id).empty());
    }

    SECTION("no loot without players") {
        ConfigureDeterministic(game);
        game.Tick(10s);
        CHECK(game.GetLostObjectsOnMap(map_id).empty());
    }

    SECTION("loot count follows looter count and never exceeds it") {
        ConfigureDeterministic(game);
        game.JoinPlayer("A", map_id);
        game.JoinPlayer("B", map_id);
        game.JoinPlayer("C", map_id);

        game.Tick(1s);
        CHECK(game.GetLostObjectsOnMap(map_id).size() == 3);

        game.Tick(1s);
        CHECK(game.GetLostObjectsOnMap(map_id).size() == 3);

        game.JoinPlayer("D", map_id);
        game.Tick(1s);
        CHECK(game.GetLostObjectsOnMap(map_id).size() == 4);
    }

    SECTION("generated objects have valid type, position and unique ids") {
        ConfigureDeterministic(game);
        for (int i = 0; i < 50; ++i) {
            game.JoinPlayer("P" + std::to_string(i), map_id);
        }
        game.Tick(1s);

        const auto objects = game.GetLostObjectsOnMap(map_id);
        REQUIRE(objects.size() == 50);

        std::set<model::LostObject::Id> ids;
        for (const auto& obj : objects) {
            CHECK(obj.type < 3);
            CHECK(IsOnSomeRoad(*game.FindMap(map_id), obj.position));
            ids.insert(obj.id);
        }
        CHECK(ids.size() == objects.size());
    }

    SECTION("all loot types can appear") {
        ConfigureDeterministic(game);
        for (int i = 0; i < 200; ++i) {
            game.JoinPlayer("P" + std::to_string(i), map_id);
        }
        game.Tick(1s);

        std::set<size_t> types;
        for (const auto& obj : game.GetLostObjectsOnMap(map_id)) {
            types.insert(obj.type);
        }
        CHECK(types == std::set<size_t>{0, 1, 2});
    }

    SECTION("zero probability generates nothing") {
        game.SetLootGeneratorConfig(1s, 0.0);
        game.JoinPlayer("A", map_id);
        game.Tick(100s);
        CHECK(game.GetLostObjectsOnMap(map_id).empty());
    }

    SECTION("invalid generator config is rejected") {
        CHECK_THROWS_AS(game.SetLootGeneratorConfig(0ms, 0.5), std::invalid_argument);
        CHECK_THROWS_AS(game.SetLootGeneratorConfig(1s, -0.1), std::invalid_argument);
        CHECK_THROWS_AS(game.SetLootGeneratorConfig(1s, 1.1), std::invalid_argument);
    }
}

TEST_CASE("Loot is generated per map", "[model][loot]") {
    model::Game game;
    game.AddMap(MakeMap("a", 2));
    game.AddMap(MakeMap("b", 5));
    ConfigureDeterministic(game);

    game.JoinPlayer("p1", model::Map::Id{"a"});
    game.JoinPlayer("p2", model::Map::Id{"b"});
    game.JoinPlayer("p3", model::Map::Id{"b"});

    game.Tick(1s);

    const auto on_a = game.GetLostObjectsOnMap(model::Map::Id{"a"});
    const auto on_b = game.GetLostObjectsOnMap(model::Map::Id{"b"});
    CHECK(on_a.size() == 1);
    CHECK(on_b.size() == 2);
    for (const auto& obj : on_a) CHECK(obj.type < 2);
    for (const auto& obj : on_b) CHECK(obj.type < 5);

    CHECK(game.GetLostObjectsOnMap(model::Map::Id{"unknown"}).empty());
}

TEST_CASE("Game with loot survives move", "[model][loot]") {
    model::Game source;
    source.AddMap(MakeMap("a", 1));
    ConfigureDeterministic(source);
    source.JoinPlayer("p", model::Map::Id{"a"});
    source.Tick(1s);

    model::Game moved{std::move(source)};
    CHECK(moved.GetLostObjectsOnMap(model::Map::Id{"a"}).size() == 1);
    moved.JoinPlayer("q", model::Map::Id{"a"});
    moved.Tick(1s);
    CHECK(moved.GetLostObjectsOnMap(model::Map::Id{"a"}).size() == 2);
}

namespace {


model::Map MakeLootMap(size_t bag_capacity) {
    model::Map map{model::Map::Id{"loot"}, "Loot"};
    map.AddRoad(model::Road{model::Road::HORIZONTAL, model::Point{0, 0}, 10});
    map.AddOffice(model::Office{model::Office::Id{"o"}, model::Point{5, 0}, model::Offset{0, 0}});
    map.SetBagCapacity(bag_capacity);
    map.SetLootValues({0, 10, 20, 30, 40});  // стоимость = тип * 10
    return map;
}

const model::Map::Id kLootMapId{"loot"};

std::vector<model::BagItem> BagOf(const model::Game& game, model::Player::Id id) {
    for (const auto& state : game.GetPlayerStatesOnMap(kLootMapId)) {
        if (state.id == id) {
            return state.bag;
        }
    }
    return {};
}

int ScoreOf(const model::Game& game, model::Player::Id id) {
    for (const auto& state : game.GetPlayerStatesOnMap(kLootMapId)) {
        if (state.id == id) {
            return state.score;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("Bag capacity defaults to 3", "[model][bag]") {
    model::Map map{model::Map::Id{"m"}, "m"};
    CHECK(map.GetBagCapacity() == 3);
    map.SetBagCapacity(5);
    CHECK(map.GetBagCapacity() == 5);
}

TEST_CASE("Dog picks up items in order, skips when full, returns them at the office",
          "[model][bag]") {
    model::Game game;
    game.AddMap(MakeLootMap(2));
    const auto joined = game.JoinPlayer("Rex", kLootMapId);

    const auto id1 = game.AddLostObject(kLootMapId, 1, {1, 0});
    const auto id2 = game.AddLostObject(kLootMapId, 2, {2, 0});
    const auto id3 = game.AddLostObject(kLootMapId, 3, {3, 0});  
    const auto id4 = game.AddLostObject(kLootMapId, 4, {8, 0});  
    static_cast<void>(id1);
    static_cast<void>(id2);

    REQUIRE(game.SetPlayerSpeed(joined.token, {10, 0}));
    game.Tick(1s);  

    const auto bag = BagOf(game, joined.id);
    REQUIRE(bag.size() == 1);
    CHECK(bag[0].id == id4);
    CHECK(bag[0].type == 4);

    const auto left = game.GetLostObjectsOnMap(kLootMapId);
    REQUIRE(left.size() == 1);
    CHECK(left[0].id == id3);
}

TEST_CASE("Bag keeps pickup order and ids/types", "[model][bag]") {
    model::Game game;
    game.AddMap(MakeLootMap(3));
    const auto joined = game.JoinPlayer("Rex", kLootMapId);

    game.AddLostObject(kLootMapId, 7, {1, 0});
    game.AddLostObject(kLootMapId, 5, {2, 0});
    game.AddLostObject(kLootMapId, 9, {3, 0});

    REQUIRE(game.SetPlayerSpeed(joined.token, {10, 0}));
    game.Tick(400ms);  

    const auto bag = BagOf(game, joined.id);
    REQUIRE(bag.size() == 3);
    CHECK(bag[0].id == 0);
    CHECK(bag[0].type == 7);
    CHECK(bag[1].id == 1);
    CHECK(bag[1].type == 5);
    CHECK(bag[2].id == 2);
    CHECK(bag[2].type == 9);
    CHECK(game.GetLostObjectsOnMap(kLootMapId).empty());
}

TEST_CASE("The dog that reaches the item first takes it", "[model][bag]") {
    model::Game game;
    game.AddMap(MakeLootMap(3));
    const auto slow = game.JoinPlayer("Slow", kLootMapId);
    const auto fast = game.JoinPlayer("Fast", kLootMapId);
    game.AddLostObject(kLootMapId, 0, {2, 0});

    REQUIRE(game.SetPlayerSpeed(slow.token, {5, 0}));
    REQUIRE(game.SetPlayerSpeed(fast.token, {10, 0}));
    game.Tick(430ms);  

    CHECK(BagOf(game, slow.id).empty());
    CHECK(BagOf(game, fast.id).size() == 1);
    CHECK(game.GetLostObjectsOnMap(kLootMapId).empty());
}

TEST_CASE("Item is collected only within half of the dog width", "[model][bag]") {
    model::Game game;
    game.AddMap(MakeLootMap(3));
    const auto joined = game.JoinPlayer("Rex", kLootMapId);
    game.AddLostObject(kLootMapId, 0, {2, 0.29});  // в пределах 0,3
    game.AddLostObject(kLootMapId, 0, {3, 0.31});  // за пределами 0,3

    REQUIRE(game.SetPlayerSpeed(joined.token, {10, 0}));
    game.Tick(400ms);

    CHECK(BagOf(game, joined.id).size() == 1);
    CHECK(game.GetLostObjectsOnMap(kLootMapId).size() == 1);
}

TEST_CASE("Standing dog collects nothing", "[model][bag]") {
    model::Game game;
    game.AddMap(MakeLootMap(3));
    const auto joined = game.JoinPlayer("Rex", kLootMapId);
    game.AddLostObject(kLootMapId, 0, {0, 0});

    game.Tick(1s);

    CHECK(BagOf(game, joined.id).empty());
    CHECK(game.GetLostObjectsOnMap(kLootMapId).size() == 1);
}

TEST_CASE("Dog stops at the road edge", "[model][move]") {
    model::Game game;
    game.AddMap(MakeLootMap(3));
    const auto joined = game.JoinPlayer("Rex", kLootMapId);

    REQUIRE(game.SetPlayerSpeed(joined.token, {100, 0}));
    game.Tick(1s);

    const auto states = game.GetPlayerStatesOnMap(kLootMapId);
    REQUIRE(states.size() == 1);
    CHECK(states[0].position.x == Catch::Approx(10.4));
    CHECK(states[0].position.y == Catch::Approx(0.0));
    CHECK(states[0].speed.x == 0.0);
    CHECK(states[0].speed.y == 0.0);
    CHECK(states[0].dir == "R");
}

TEST_CASE("Dog cannot leave the road across its width", "[model][move]") {
    model::Game game;
    game.AddMap(MakeLootMap(3));
    const auto joined = game.JoinPlayer("Rex", kLootMapId);

    REQUIRE(game.SetPlayerSpeed(joined.token, {0, 5}));
    game.Tick(1s);

    const auto states = game.GetPlayerStatesOnMap(kLootMapId);
    REQUIRE(states.size() == 1);
    CHECK(states[0].position.x == Catch::Approx(0.0));
    CHECK(states[0].position.y == Catch::Approx(0.4));
    CHECK(states[0].speed.y == 0.0);
}

TEST_CASE("Default loot generator is deterministic after a long tick", "[model][loot]") {
    model::Game game;
    game.AddMap(MakeMap("a", 2));
    game.SetLootGeneratorConfig(5s, 0.5);  
    // Игроки стоят на месте тысячи секунд; не даём им уйти на покой посреди теста.
    game.SetDogRetirementTime(std::chrono::hours{24 * 365});
    const model::Map::Id map_id{"a"};

    game.JoinPlayer("p1", map_id);
    game.Tick(5000s);
    CHECK(game.GetLostObjectsOnMap(map_id).size() == 1);

    game.JoinPlayer("p2", map_id);
    game.Tick(5000s);
    CHECK(game.GetLostObjectsOnMap(map_id).size() == 2);
}

TEST_CASE("Score starts at zero and grows by the value of returned items", "[model][score]") {
    model::Game game;
    game.AddMap(MakeLootMap(3));
    const auto joined = game.JoinPlayer("Rex", kLootMapId);
    CHECK(ScoreOf(game, joined.id) == 0);

    game.AddLostObject(kLootMapId, 1, {1, 0});  
    game.AddLostObject(kLootMapId, 3, {2, 0});  
    game.AddLostObject(kLootMapId, 4, {8, 0});  

    REQUIRE(game.SetPlayerSpeed(joined.token, {10, 0}));
    game.Tick(400ms);  
    CHECK(ScoreOf(game, joined.id) == 0);
    CHECK(BagOf(game, joined.id).size() == 2);

    game.Tick(300ms);  
    CHECK(ScoreOf(game, joined.id) == 40);
    CHECK(BagOf(game, joined.id).empty());
}

TEST_CASE("Score is not given for unreturned items", "[model][score]") {
    model::Game game;
    game.AddMap(MakeLootMap(3));
    const auto joined = game.JoinPlayer("Rex", kLootMapId);
    game.AddLostObject(kLootMapId, 2, {1, 0});

    REQUIRE(game.SetPlayerSpeed(joined.token, {10, 0}));
    game.Tick(200ms);

    CHECK(BagOf(game, joined.id).size() == 1);
    CHECK(ScoreOf(game, joined.id) == 0);
}
