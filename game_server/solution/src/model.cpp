#include "model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

namespace model {
using namespace std::literals;

namespace {

constexpr double kRoadHalfWidth{0.4};
constexpr double kPositionEps{1e-9};

constexpr double kItemHalfWidth{0.0};
constexpr double kDogHalfWidth{0.6 / 2};
constexpr double kOfficeHalfWidth{0.5 / 2};

struct RoadBounds {
    double min_x, max_x, min_y, max_y;

    bool Contains(Dog::Position pos) const noexcept {
        return pos.x >= min_x - kPositionEps && pos.x <= max_x + kPositionEps &&
               pos.y >= min_y - kPositionEps && pos.y <= max_y + kPositionEps;
    }

    Dog::Position Clamp(Dog::Position pos) const noexcept {
        return {std::clamp(pos.x, min_x, max_x), std::clamp(pos.y, min_y, max_y)};
    }
};

RoadBounds GetRoadBounds(const Road& road) noexcept {
    const Point a = road.GetStart();
    const Point b = road.GetEnd();
    return {std::min(a.x, b.x) - kRoadHalfWidth, std::max(a.x, b.x) + kRoadHalfWidth,
            std::min(a.y, b.y) - kRoadHalfWidth, std::max(a.y, b.y) + kRoadHalfWidth};
}

struct MoveResult {
    Dog::Position position;
    bool stopped;
};


MoveResult MoveAlongRoads(const Map& map, Dog::Position from, Dog::Speed speed, double seconds) {
    const Dog::Position target{from.x + speed.x * seconds, from.y + speed.y * seconds};

    bool on_road = false;
    Dog::Position best = from;
    double best_dist = -1.0;
    for (const Road& road : map.GetRoads()) {
        const RoadBounds bounds = GetRoadBounds(road);
        if (!bounds.Contains(from)) {
            continue;
        }
        on_road = true;
        const Dog::Position candidate = bounds.Clamp(target);
        const double dist = std::abs(candidate.x - from.x) + std::abs(candidate.y - from.y);
        if (dist > best_dist) {
            best_dist = dist;
            best = candidate;
        }
    }

    if (!on_road) {
        return {from, true};
    }
    return {best, best.x != target.x || best.y != target.y};
}

class MapGatherProvider final : public collision_detector::ItemGathererProvider {
public:
    MapGatherProvider(const std::vector<LostObject>& objects, const Map::Offices& offices,
                      std::vector<collision_detector::Gatherer> gatherers)
        : objects_(objects)
        , offices_(offices)
        , gatherers_(std::move(gatherers)) {
    }

    size_t ItemsCount() const override {
        return objects_.size() + offices_.size();
    }

    collision_detector::Item GetItem(size_t idx) const override {
        if (idx < objects_.size()) {
            const auto pos = objects_[idx].position;
            return {{pos.x, pos.y}, kItemHalfWidth};
        }
        const Point pos = offices_[idx - objects_.size()].GetPosition();
        return {{static_cast<double>(pos.x), static_cast<double>(pos.y)}, kOfficeHalfWidth};
    }

    size_t GatherersCount() const override {
        return gatherers_.size();
    }

    collision_detector::Gatherer GetGatherer(size_t idx) const override {
        return gatherers_[idx];
    }

    bool IsOffice(size_t item_idx) const noexcept {
        return item_idx >= objects_.size();
    }

private:
    const std::vector<LostObject>& objects_;
    const Map::Offices& offices_;
    std::vector<collision_detector::Gatherer> gatherers_;
};

}  // namespace

void Map::AddOffice(Office office) {
    if (warehouse_id_to_index_.contains(office.GetId())) {
        throw std::invalid_argument("Duplicate warehouse");
    }

    const size_t index = offices_.size();
    Office& o = offices_.emplace_back(std::move(office));
    try {
        warehouse_id_to_index_.emplace(o.GetId(), index);
    } catch (...) {
        offices_.pop_back();
        throw;
    }
}

Game::Game(Game&& other) noexcept {
    std::lock_guard lock(other.players_mutex_);
    maps_ = std::move(other.maps_);
    map_id_to_index_ = std::move(other.map_id_to_index_);
    players_ = std::move(other.players_);
    token_to_player_index_ = std::move(other.token_to_player_index_);
    next_player_id_ = other.next_player_id_;
    loot_config_ = std::move(other.loot_config_);
    map_loot_ = std::move(other.map_loot_);
    position_rng_ = other.position_rng_;
    dog_retirement_time_ = other.dog_retirement_time_;
    tick_listener_ = std::move(other.tick_listener_);
    retirement_listener_ = std::move(other.retirement_listener_);
}

Game& Game::operator=(Game&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    std::scoped_lock lock(players_mutex_, other.players_mutex_);
    maps_ = std::move(other.maps_);
    map_id_to_index_ = std::move(other.map_id_to_index_);
    players_ = std::move(other.players_);
    token_to_player_index_ = std::move(other.token_to_player_index_);
    next_player_id_ = other.next_player_id_;
    loot_config_ = std::move(other.loot_config_);
    map_loot_ = std::move(other.map_loot_);
    position_rng_ = other.position_rng_;
    dog_retirement_time_ = other.dog_retirement_time_;
    tick_listener_ = std::move(other.tick_listener_);
    retirement_listener_ = std::move(other.retirement_listener_);
    return *this;
}

void Game::AddMap(Map map) {
    const size_t index = maps_.size();
    if (auto [it, inserted] = map_id_to_index_.emplace(map.GetId(), index); !inserted) {
        throw std::invalid_argument("Map with id "s + *map.GetId() + " already exists"s);
    } else {
        try {
            maps_.emplace_back(std::move(map));
        } catch (...) {
            map_id_to_index_.erase(it);
            throw;
        }
    }
}

std::string Game::GenerateToken() {
    static constexpr char kHex[] = "0123456789abcdef";

    std::random_device rd;
    std::mt19937_64 generator{(static_cast<std::uint64_t>(rd()) << 32) ^ rd()};

    std::string token;
    token.reserve(32);

    for (int i = 0; i < 2; ++i) {
        const std::uint64_t value = generator();
        for (int shift = 60; shift >= 0; shift -= 4) {
            token.push_back(kHex[(value >> shift) & 0x0f]);
        }
    }

    return token;
}

JoinedPlayer Game::JoinPlayer(std::string name, Map::Id map_id) {
    std::lock_guard lock(players_mutex_);

    const Player::Id player_id = next_player_id_++;
    std::string token;
    do {
        token = GenerateToken();
    } while (token_to_player_index_.contains(token));

    const size_t index = players_.size();
    players_.emplace_back(player_id, std::move(name), std::move(map_id), token);
    if (const auto map_it = map_id_to_index_.find(players_.back().GetMapId()); map_it != map_id_to_index_.end()) {
        const auto& roads = maps_[map_it->second].GetRoads();
        if (!roads.empty()) {
            const Point start = roads.front().GetStart();
            players_.back().GetDog().SetPosition({static_cast<double>(start.x),static_cast<double>(start.y)});
        }
    }
    try {
        token_to_player_index_.emplace(players_.back().GetToken(), index);
    } catch (...) {
        players_.pop_back();
        throw;
    }

    return {player_id, std::move(token)};
}

std::optional<PlayerInfo> Game::FindPlayerByToken(const std::string& token) const {
    std::lock_guard lock(players_mutex_);

    const auto it = token_to_player_index_.find(token);
    if (it == token_to_player_index_.end()) {
        return std::nullopt;
    }

    const Player& player = players_.at(it->second);
    return PlayerInfo{player.GetId(), player.GetName(), player.GetMapId()};
}

std::vector<PlayerInfo> Game::GetPlayersOnMap(const Map::Id& map_id) const {
    std::lock_guard lock(players_mutex_);

    std::vector<PlayerInfo> result;
    for (const Player& player : players_) {
        if (player.GetMapId() == map_id) {
            result.push_back({player.GetId(), player.GetName(), player.GetMapId()});
        }
    }
    return result;
}

std::vector<PlayerState> Game::GetPlayerStatesOnMap(const Map::Id& map_id) const {
    std::lock_guard lock(players_mutex_);

    std::vector<PlayerState> result;
    for (const Player& player : players_) {
        if (player.GetMapId() == map_id) {
            result.push_back({
                player.GetId(),
                player.GetDog().GetPosition(),
                player.GetDog().GetSpeed(),
                player.GetDog().GetDirection(),
                player.GetDog().GetBag(),
                player.GetDog().GetScore()
            });
        }
    }
    return result;
}

bool Game::SetPlayerSpeed(const std::string& token, Dog::Speed speed) {
    std::lock_guard lock(players_mutex_);

    const auto it = token_to_player_index_.find(token);
    if (it == token_to_player_index_.end()) {
        return false;
    }

    players_.at(it->second).GetDog().SetSpeed(speed);
    return true;
}

void Game::SetLootGeneratorConfig(loot_gen::LootGenerator::TimeInterval period, double probability,
                                  loot_gen::LootGenerator::RandomGenerator random_gen) {
    if (period.count() <= 0) {
        throw std::invalid_argument("Loot generator period must be positive");
    }
    if (probability < 0.0 || probability > 1.0) {
        throw std::invalid_argument("Loot generator probability must be in [0, 1]");
    }

    if (!random_gen) {

        random_gen = [] { return 1.0; };
    }

    std::lock_guard lock(players_mutex_);
    loot_config_ = LootConfig{period, probability, std::move(random_gen)};
    for (auto& [index, loot] : map_loot_) {
        loot.generator.reset();
    }
}

Dog::Position Game::GenerateRandomPositionOnRoad(const Map& map) {
    const auto& roads = map.GetRoads();
    std::uniform_int_distribution<size_t> road_dist(0, roads.size() - 1);
    const Road& road = roads[road_dist(position_rng_)];

    const Point start = road.GetStart();
    const Point end = road.GetEnd();
    const double t = std::uniform_real_distribution<double>(0.0, 1.0)(position_rng_);

    return {start.x + (end.x - start.x) * t, start.y + (end.y - start.y) * t};
}

void Game::SetDogRetirementTime(std::chrono::milliseconds time) {
    if (time.count() < 0) {
        throw std::invalid_argument("Dog retirement time must not be negative");
    }
    std::lock_guard lock(players_mutex_);
    dog_retirement_time_ = time;
}

std::chrono::milliseconds Game::GetDogRetirementTime() const {
    std::lock_guard lock(players_mutex_);
    return dog_retirement_time_;
}

void Game::Tick(loot_gen::LootGenerator::TimeInterval delta) {
    const std::vector<RetiredPlayer> retired = AdvanceState(delta);
    // Слушатели вызываются вне блокировки, чтобы они могли безопасно обращаться к модели
    // (например, сделать снимок состояния или записать результаты в базу данных).
    if (retirement_listener_) {
        for (const RetiredPlayer& player : retired) {
            retirement_listener_(player);
        }
    }
    if (tick_listener_) {
        tick_listener_(delta);
    }
}

std::vector<RetiredPlayer> Game::AdvanceState(loot_gen::LootGenerator::TimeInterval delta) {
    std::lock_guard lock(players_mutex_);

    std::vector<RetiredPlayer> retired;
    for (size_t index = 0; index < maps_.size(); ++index) {
        const Map& map = maps_[index];

        MoveDogsAndCollect(index, delta);
        // Уходящие на покой игроки не должны учитываться при генерации трофеев.
        RetireIdlePlayers(map, retired);

        if (!loot_config_ || map.GetLootTypeCount() == 0 || map.GetRoads().empty()) {
            continue;
        }

        MapLoot& loot = map_loot_[index];
        if (!loot.generator) {
            loot.generator.emplace(loot_config_->period, loot_config_->probability,
                                   loot_config_->random_gen);
            loot.generator->SetTimeWithoutLoot(loot.pending_time_without_loot);
            loot.pending_time_without_loot = {};
        }

        const auto looter_count = static_cast<unsigned>(std::count_if(
            players_.begin(), players_.end(),
            [&map](const Player& player) { return player.GetMapId() == map.GetId(); }));

        const unsigned to_generate = loot.generator->Generate(
            delta, static_cast<unsigned>(loot.objects.size()), looter_count);

        std::uniform_int_distribution<size_t> type_dist(0, map.GetLootTypeCount() - 1);
        for (unsigned i = 0; i < to_generate; ++i) {
            loot.objects.push_back(LostObject{
                loot.next_id++,
                type_dist(position_rng_),
                GenerateRandomPositionOnRoad(map)
            });
        }
    }
    return retired;
}

void Game::RetireIdlePlayers(const Map& map, std::vector<RetiredPlayer>& retired) {
    const auto is_retiring = [&](const Player& player) {
        const Dog& dog = player.GetDog();
        return player.GetMapId() == map.GetId() && dog.IsStationary() &&
               dog.GetIdleTime() >= dog_retirement_time_;
    };
    if (std::none_of(players_.begin(), players_.end(), is_retiring)) {
        return;
    }

    std::vector<Player> remaining;
    remaining.reserve(players_.size());
    for (Player& player : players_) {
        if (!is_retiring(player)) {
            remaining.push_back(std::move(player));
            continue;
        }
        const Dog& dog = player.GetDog();
        // Тик мог закончиться уже после ухода на покой: лишнее время в игровое не входит.
        const auto overshoot = dog.GetIdleTime() - dog_retirement_time_;
        retired.push_back(RetiredPlayer{player.GetName(), dog.GetScore(),
                                        dog.GetPlayTime() - overshoot});
    }

    players_ = std::move(remaining);
    token_to_player_index_.clear();
    for (size_t i = 0; i < players_.size(); ++i) {
        token_to_player_index_.emplace(players_[i].GetToken(), i);
    }
}

void Game::MoveDogsAndCollect(size_t map_index, std::chrono::milliseconds delta) {
    if (delta.count() <= 0) {
        return;
    }
    const double seconds = std::chrono::duration<double>(delta).count();
    const Map& map = maps_[map_index];

    std::vector<Dog*> dogs;
    std::vector<collision_detector::Gatherer> gatherers;
    for (Player& player : players_) {
        if (player.GetMapId() != map.GetId()) {
            continue;
        }
        Dog& dog = player.GetDog();
        dog.AddPlayTime(delta);

        const Dog::Position start = dog.GetPosition();
        const Dog::Speed speed = dog.GetSpeed();
        if (dog.IsStationary()) {
            // Собака стояла весь тик.
            dog.AddIdleTime(delta);
            continue;
        }

        const MoveResult moved = MoveAlongRoads(map, start, speed, seconds);
        dog.SetPosition(moved.position);
        if (moved.stopped) {
            // Собака двигалась только часть тика; остаток времени она уже стоит.
            const double distance = std::abs(moved.position.x - start.x) +
                                    std::abs(moved.position.y - start.y);
            const double speed_value = std::abs(speed.x) + std::abs(speed.y);
            auto moving_time = std::chrono::milliseconds{
                std::llround(distance / speed_value * 1000.0)};
            moving_time = std::clamp(moving_time, std::chrono::milliseconds{0}, delta);

            dog.SetSpeed({0.0, 0.0});  // обнуляет счётчик бездействия
            dog.AddIdleTime(delta - moving_time);
        }

        dogs.push_back(&dog);
        gatherers.push_back({{start.x, start.y},
                             {moved.position.x, moved.position.y},
                             kDogHalfWidth});
    }
    if (dogs.empty()) {
        return;
    }

    const auto loot_it = map_loot_.find(map_index);
    static const std::vector<LostObject> kNoObjects;
    const std::vector<LostObject>& objects = loot_it != map_loot_.end() ? loot_it->second.objects : kNoObjects;

    const MapGatherProvider provider{objects, map.GetOffices(), std::move(gatherers)};
    auto events = collision_detector::FindGatherEvents(provider);
    std::stable_sort(events.begin(), events.end(),
                     [](const collision_detector::GatheringEvent& lhs,
                        const collision_detector::GatheringEvent& rhs) {
                            return lhs.time < rhs.time;
                        }
    );

    std::vector<bool> collected(objects.size(), false);
    bool any_collected = false;
    for (const auto& event : events) {
        Dog& dog = *dogs[event.gatherer_id];
        if (provider.IsOffice(event.item_id)) {
            for (const BagItem& item : dog.GetBag()) {
                dog.AddScore(map.GetLootValue(item.type));
            }
            dog.ClearBag();
            continue;
        }
        if (collected[event.item_id] || dog.GetBag().size() >= map.GetBagCapacity()) {
            continue;  
        }
        const LostObject& object = objects[event.item_id];
        dog.AddToBag({object.id, object.type});
        collected[event.item_id] = true;
        any_collected = true;
    }


    if (any_collected) {
        auto& mutable_objects = loot_it->second.objects;
        size_t kept{0};
        for (size_t i = 0; i < mutable_objects.size(); ++i) {
            if (!collected[i]) {
                if (kept != i) {
                    mutable_objects[kept] = mutable_objects[i];
                }
                ++kept;
            }
        }
        mutable_objects.resize(kept);
    }
}

std::vector<LostObject> Game::GetLostObjectsOnMap(const Map::Id& map_id) const {
    std::lock_guard lock(players_mutex_);

    const auto it = map_id_to_index_.find(map_id);
    if (it == map_id_to_index_.end()) {
        return {};
    }
    const auto loot_it = map_loot_.find(it->second);
    if (loot_it == map_loot_.end()) {
        return {};
    }
    return loot_it->second.objects;
}

GameSnapshot Game::TakeSnapshot() const {
    std::lock_guard lock(players_mutex_);

    GameSnapshot snapshot;
    snapshot.next_player_id = next_player_id_;

    snapshot.players.reserve(players_.size());
    for (const Player& player : players_) {
        const Dog& dog = player.GetDog();
        snapshot.players.push_back(PlayerSnapshot{
            player.GetId(), player.GetName(), player.GetMapId(), player.GetToken(),
            dog.GetPosition(), dog.GetSpeed(), dog.GetDirection(), dog.GetBag(), dog.GetScore(),
            dog.GetPlayTime(), dog.GetIdleTime()});
    }

    // Обходим карты в порядке их загрузки, чтобы содержимое снимка было детерминированным.
    for (size_t index = 0; index < maps_.size(); ++index) {
        const auto it = map_loot_.find(index);
        if (it == map_loot_.end()) {
            continue;
        }
        const MapLoot& loot = it->second;
        const auto time_without_loot =
            loot.generator ? loot.generator->GetTimeWithoutLoot() : loot.pending_time_without_loot;
        snapshot.loot.push_back(MapLootSnapshot{maps_[index].GetId(), loot.next_id, loot.objects,
                                                time_without_loot});
    }
    return snapshot;
}

void Game::RestoreSnapshot(const GameSnapshot& snapshot) {
    const auto is_valid_position = [](Dog::Position pos) {
        return std::isfinite(pos.x) && std::isfinite(pos.y);
    };
    const auto fail = [](const std::string& message) {
        throw std::invalid_argument("Invalid game state: " + message);
    };

    // maps_ и map_id_to_index_ после загрузки конфигурации не меняются,
    // поэтому проверять снимок можно до захвата блокировки.
    std::lock_guard lock(players_mutex_);

    // 1. Полная проверка снимка; состояние игры меняется только если всё корректно.
    std::unordered_map<std::string, size_t> new_tokens;
    std::unordered_map<Player::Id, size_t> new_ids;
    for (size_t i = 0; i < snapshot.players.size(); ++i) {
        const PlayerSnapshot& p = snapshot.players[i];

        const auto map_it = map_id_to_index_.find(p.map_id);
        if (map_it == map_id_to_index_.end()) {
            fail("player " + std::to_string(p.id) + " refers to unknown map \"" + *p.map_id + "\"");
        }
        const Map& map = maps_[map_it->second];

        if (p.id >= snapshot.next_player_id) {
            fail("player id " + std::to_string(p.id) + " is not less than next player id");
        }
        if (!new_ids.emplace(p.id, i).second) {
            fail("duplicate player id " + std::to_string(p.id));
        }
        if (p.token.empty()) {
            fail("player " + std::to_string(p.id) + " has empty token");
        }
        if (!new_tokens.emplace(p.token, i).second) {
            fail("duplicate player token");
        }
        if (!is_valid_position(p.position)) {
            fail("player " + std::to_string(p.id) + " has invalid position");
        }
        if (!std::isfinite(p.speed.x) || !std::isfinite(p.speed.y)) {
            fail("player " + std::to_string(p.id) + " has invalid speed");
        }
        if (p.direction != "L" && p.direction != "R" && p.direction != "U" && p.direction != "D") {
            fail("player " + std::to_string(p.id) + " has invalid direction");
        }
        if (p.score < 0) {
            fail("player " + std::to_string(p.id) + " has negative score");
        }
        if (p.play_time.count() < 0 || p.idle_time.count() < 0) {
            fail("player " + std::to_string(p.id) + " has negative time");
        }
        for (const BagItem& item : p.bag) {
            if (item.type >= map.GetLootTypeCount()) {
                fail("player " + std::to_string(p.id) + " has item of unknown type");
            }
        }
    }

    std::unordered_map<size_t, const MapLootSnapshot*> new_loot;
    for (const MapLootSnapshot& l : snapshot.loot) {
        const auto map_it = map_id_to_index_.find(l.map_id);
        if (map_it == map_id_to_index_.end()) {
            fail("loot refers to unknown map \"" + *l.map_id + "\"");
        }
        if (!new_loot.emplace(map_it->second, &l).second) {
            fail("duplicate loot section for map \"" + *l.map_id + "\"");
        }
        if (l.time_without_loot.count() < 0) {
            fail("negative time without loot");
        }
        const Map& map = maps_[map_it->second];
        std::unordered_map<LostObject::Id, bool> seen_ids;
        for (const LostObject& obj : l.objects) {
            if (obj.id >= l.next_id) {
                fail("lost object id " + std::to_string(obj.id) + " is not less than next object id");
            }
            if (!seen_ids.emplace(obj.id, true).second) {
                fail("duplicate lost object id " + std::to_string(obj.id));
            }
            if (obj.type >= map.GetLootTypeCount()) {
                fail("lost object " + std::to_string(obj.id) + " has unknown type");
            }
            if (!is_valid_position(obj.position)) {
                fail("lost object " + std::to_string(obj.id) + " has invalid position");
            }
        }
    }

    // 2. Применение: дальше исключений быть не должно.
    std::vector<Player> players;
    players.reserve(snapshot.players.size());
    for (const PlayerSnapshot& p : snapshot.players) {
        Player& player = players.emplace_back(p.id, p.name, p.map_id, p.token);
        player.GetDog().Restore(p.position, p.speed, p.direction, p.bag, p.score, p.play_time,
                                p.idle_time);
    }

    std::unordered_map<size_t, MapLoot> loot;
    for (const auto& [index, l] : new_loot) {
        MapLoot& map_loot = loot[index];
        map_loot.objects = l->objects;
        map_loot.next_id = l->next_id;
        map_loot.pending_time_without_loot = l->time_without_loot;
    }

    players_ = std::move(players);
    token_to_player_index_.clear();
    for (size_t i = 0; i < players_.size(); ++i) {
        token_to_player_index_.emplace(players_[i].GetToken(), i);
    }
    next_player_id_ = snapshot.next_player_id;
    map_loot_ = std::move(loot);
}

LostObject::Id Game::AddLostObject(const Map::Id& map_id, size_t type, Dog::Position position) {
    std::lock_guard lock(players_mutex_);

    const auto it = map_id_to_index_.find(map_id);
    if (it == map_id_to_index_.end()) {
        throw std::invalid_argument("Unknown map");
    }
    MapLoot& loot = map_loot_[it->second];
    const LostObject::Id id = loot.next_id++;
    loot.objects.push_back(LostObject{id, type, position});
    return id;
}

}  // namespace model
