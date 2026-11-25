#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "collision_detector.h"
#include "loot_generator.h"
#include "tagged.h"

namespace model {

using Dimension = int;
using Coord = Dimension;

struct Point {
    Coord x, y;
};

struct Size {
    Dimension width, height;
};

struct Rectangle {
    Point position;
    Size size;
};

struct Offset {
    Dimension dx, dy;
};

class Road {
    struct HorizontalTag {
        explicit HorizontalTag() = default;
    };

    struct VerticalTag {
        explicit VerticalTag() = default;
    };

public:
    constexpr static HorizontalTag HORIZONTAL{};
    constexpr static VerticalTag VERTICAL{};

    Road(HorizontalTag, Point start, Coord end_x) noexcept
        : start_{start}
        , end_{end_x, start.y} {
    }

    Road(VerticalTag, Point start, Coord end_y) noexcept
        : start_{start}
        , end_{start.x, end_y} {
    }

    bool IsHorizontal() const noexcept {
        return start_.y == end_.y;
    }

    bool IsVertical() const noexcept {
        return start_.x == end_.x;
    }

    Point GetStart() const noexcept {
        return start_;
    }

    Point GetEnd() const noexcept {
        return end_;
    }

private:
    Point start_;
    Point end_;
};

class Building {
public:
    explicit Building(Rectangle bounds) noexcept
        : bounds_{bounds} {
    }

    const Rectangle& GetBounds() const noexcept {
        return bounds_;
    }

private:
    Rectangle bounds_;
};

class Office {
public:
    using Id = util::Tagged<std::string, Office>;

    Office(Id id, Point position, Offset offset) noexcept
        : id_{std::move(id)}
        , position_{position}
        , offset_{offset} {
    }

    const Id& GetId() const noexcept {
        return id_;
    }

    Point GetPosition() const noexcept {
        return position_;
    }

    Offset GetOffset() const noexcept {
        return offset_;
    }

private:
    Id id_;
    Point position_;
    Offset offset_;
};

inline constexpr size_t kDefaultBagCapacity = 3;

class Map {
public:
    using Id = util::Tagged<std::string, Map>;
    using Roads = std::vector<Road>;
    using Buildings = std::vector<Building>;
    using Offices = std::vector<Office>;

    Map(Id id, std::string name, double dog_speed = 1.0) noexcept
        : id_(std::move(id))
        , name_(std::move(name))
        , dog_speed_(dog_speed) {
    }

    const Id& GetId() const noexcept {
        return id_;
    }

    const std::string& GetName() const noexcept {
        return name_;
    }

    double GetDogSpeed() const noexcept {
        return dog_speed_;
    }

    size_t GetLootTypeCount() const noexcept {
        return loot_type_count_;
    }

    void SetLootTypeCount(size_t count) noexcept {
        loot_type_count_ = count;
    }


    size_t GetBagCapacity() const noexcept {
        return bag_capacity_;
    }

    void SetBagCapacity(size_t capacity) noexcept {
        bag_capacity_ = capacity;
    }

    void SetLootValues(std::vector<int> values) {
        loot_values_ = std::move(values);
    }

    int GetLootValue(size_t type) const noexcept {
        return type < loot_values_.size() ? loot_values_[type] : 0;
    }

    const Buildings& GetBuildings() const noexcept {
        return buildings_;
    }

    const Roads& GetRoads() const noexcept {
        return roads_;
    }

    const Offices& GetOffices() const noexcept {
        return offices_;
    }

    void AddRoad(const Road& road) {
        roads_.emplace_back(road);
    }

    void AddBuilding(const Building& building) {
        buildings_.emplace_back(building);
    }

    void AddOffice(Office office);

private:
    using OfficeIdToIndex = std::unordered_map<Office::Id, size_t, util::TaggedHasher<Office::Id>>;

    Id id_;
    std::string name_;
    double dog_speed_ = 1.0;
    size_t loot_type_count_ = 0;
    size_t bag_capacity_ = kDefaultBagCapacity;
    std::vector<int> loot_values_;
    Roads roads_;
    Buildings buildings_;

    OfficeIdToIndex warehouse_id_to_index_;
    Offices offices_;
};

struct BagItem {
    std::uint64_t id{0};
    size_t type{0};
};

class Dog {
public:
    struct Position {
        double x{0.0};
        double y{0.0};
    };

    struct Speed {
        double x{0.0};
        double y{0.0};
    };

    explicit Dog(std::string name)
        : name_(std::move(name)) {
    }

    const std::string& GetName() const noexcept {
        return name_;
    }

    Position GetPosition() const noexcept {
        return position_;
    }

    Speed GetSpeed() const noexcept {
        return speed_;
    }

    const std::string& GetDirection() const noexcept {
        return direction_;
    }

    void SetPosition(Position position) noexcept {
        position_ = position;
    }

    const std::vector<BagItem>& GetBag() const noexcept {
        return bag_;
    }

    void AddToBag(BagItem item) {
        bag_.push_back(item);
    }

    void ClearBag() noexcept {
        bag_.clear();
    }

    int GetScore() const noexcept {
        return score_;
    }

    void AddScore(int points) noexcept {
        score_ += points;
    }

    // Полностью восстанавливает состояние собаки (используется при загрузке сохранённой игры).
    void Restore(Position position, Speed speed, std::string direction, std::vector<BagItem> bag,
                 int score, std::chrono::milliseconds play_time = std::chrono::milliseconds{0},
                 std::chrono::milliseconds idle_time = std::chrono::milliseconds{0}) {
        position_ = position;
        speed_ = speed;
        direction_ = std::move(direction);
        bag_ = std::move(bag);
        score_ = score;
        play_time_ = play_time;
        idle_time_ = idle_time;
    }

    // Время, прошедшее с момента входа в игру.
    std::chrono::milliseconds GetPlayTime() const noexcept {
        return play_time_;
    }

    // Непрерывное время, в течение которого скорость собаки равна нулю.
    std::chrono::milliseconds GetIdleTime() const noexcept {
        return idle_time_;
    }

    void AddPlayTime(std::chrono::milliseconds delta) noexcept {
        play_time_ += delta;
    }

    void AddIdleTime(std::chrono::milliseconds delta) noexcept {
        idle_time_ += delta;
    }

    bool IsStationary() const noexcept {
        return speed_.x == 0.0 && speed_.y == 0.0;
    }

    void SetSpeed(Speed speed) noexcept {
        // Бездействие отсчитывается от момента остановки: любое изменение движения
        // (старт, остановка) обнуляет счётчик. Повторная команда остановки уже стоящей
        // собаке счётчик не сбрасывает, так как стоянка при этом не прерывается.
        const bool was_moving = !IsStationary();
        speed_ = speed;
        if (was_moving || !IsStationary()) {
            idle_time_ = std::chrono::milliseconds{0};
        }

        if (speed.x < 0) {
            direction_ = "L";
        } else if (speed.x > 0) {
            direction_ = "R";
        } else if (speed.y < 0) {
            direction_ = "U";
        } else if (speed.y > 0) {
            direction_ = "D";
        }
    }

private:
    std::string name_;
    Position position_;
    Speed speed_;
    std::string direction_ = "U";
    std::vector<BagItem> bag_;
    int score_{0};
    std::chrono::milliseconds play_time_{0};
    std::chrono::milliseconds idle_time_{0};
};

// Игрок, отправленный на покой из-за бездействия.
struct RetiredPlayer {
    std::string name;
    int score{0};
    std::chrono::milliseconds play_time{0};
};

struct LostObject {
    using Id = std::uint64_t;

    Id id;
    size_t type;
    Dog::Position position;
};

class Player {
public:
    using Id = std::uint64_t;

    Player(Id id, std::string name, Map::Id map_id, std::string token)
        : id_(id)
        , name_(std::move(name))
        , map_id_(std::move(map_id))
        , token_(std::move(token))
        , dog_(name_) {
    }

    Id GetId() const noexcept {
        return id_;
    }

    const std::string& GetName() const noexcept {
        return name_;
    }

    const Map::Id& GetMapId() const noexcept {
        return map_id_;
    }

    const std::string& GetToken() const noexcept {
        return token_;
    }

    const Dog& GetDog() const noexcept {
        return dog_;
    }

    Dog& GetDog() noexcept {
        return dog_;
    }

private:
    Id id_;
    std::string name_;
    Map::Id map_id_;
    std::string token_;
    Dog dog_;
};

struct JoinedPlayer {
    Player::Id id;
    std::string token;
};

struct PlayerInfo {
    Player::Id id;
    std::string name;
    Map::Id map_id;
};

struct PlayerState {
    Player::Id id;
    Dog::Position position;
    Dog::Speed speed;
    std::string dir;
    std::vector<BagItem> bag;
    int score{0};
};

// Снимок состояния одного игрока вместе с его собакой.
struct PlayerSnapshot {
    Player::Id id{0};
    std::string name;
    Map::Id map_id{std::string{}};
    std::string token;
    Dog::Position position;
    Dog::Speed speed;
    std::string direction;
    std::vector<BagItem> bag;
    int score{0};
    std::chrono::milliseconds play_time{0};
    std::chrono::milliseconds idle_time{0};
};

// Снимок потерянных предметов на одной карте.
struct MapLootSnapshot {
    Map::Id map_id{std::string{}};
    LostObject::Id next_id{0};
    std::vector<LostObject> objects;
    // Время, прошедшее с последней генерации трофея на этой карте.
    std::chrono::milliseconds time_without_loot{0};
};

// Динамическое состояние игры: всё, что меняется в процессе работы сервера.
struct GameSnapshot {
    Player::Id next_player_id{0};
    std::vector<PlayerSnapshot> players;
    std::vector<MapLootSnapshot> loot;
};

class Game {
public:
    using Maps = std::vector<Map>;
    using TickListener = std::function<void(std::chrono::milliseconds delta)>;
    // Вызывается для каждого игрока, ушедшего на покой (без захваченной блокировки модели).
    using RetirementListener = std::function<void(const RetiredPlayer& player)>;

    static constexpr std::chrono::milliseconds kDefaultDogRetirementTime{60'000};

    Game() = default;
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    Game(Game&& other) noexcept;
    Game& operator=(Game&& other) noexcept;

    void AddMap(Map map);

    const Maps& GetMaps() const noexcept {
        return maps_;
    }

    const Map* FindMap(const Map::Id& id) const noexcept {
        if (auto it = map_id_to_index_.find(id); it != map_id_to_index_.end()) {
            return &maps_.at(it->second);
        }
        return nullptr;
    }

    JoinedPlayer JoinPlayer(std::string name, Map::Id map_id);
    std::optional<PlayerInfo> FindPlayerByToken(const std::string& token) const;
    std::vector<PlayerInfo> GetPlayersOnMap(const Map::Id& map_id) const;
    std::vector<PlayerState> GetPlayerStatesOnMap(const Map::Id& map_id) const;
    bool SetPlayerSpeed(const std::string& token, Dog::Speed speed);

    /*
     * Настраивает генератор трофеев (по отдельному экземпляру на каждую карту).
     * period - базовый интервал, probability - вероятность появления трофея за него.
     * random_gen - источник чисел из [0, 1]; если не задан, используется mt19937_64.
     * Пока конфигурация не задана, трофеи не генерируются.
     */
    void SetLootGeneratorConfig(loot_gen::LootGenerator::TimeInterval period, double probability,
                                loot_gen::LootGenerator::RandomGenerator random_gen = {});

    // Слушатель вызывается после каждого игрового тика (уже без захваченной блокировки модели).
    // Устанавливать нужно до запуска обработки запросов и таймера, до начала многопоточной работы.
    void SetTickListener(TickListener listener) {
        tick_listener_ = std::move(listener);
    }

    // Время бездействия, после которого собака уходит на покой (по умолчанию одна минута).
    // Устанавливать нужно до начала многопоточной работы.
    void SetDogRetirementTime(std::chrono::milliseconds time);
    std::chrono::milliseconds GetDogRetirementTime() const;

    // Подписчик на уход игроков на покой. Устанавливать нужно до запуска обработки запросов и таймера.
    void SetRetirementListener(RetirementListener listener) {
        retirement_listener_ = std::move(listener);
    }

    // Возвращает согласованный снимок динамического состояния (игроки, собаки, трофеи).
    GameSnapshot TakeSnapshot() const;

    // Заменяет динамическое состояние игры состоянием из снимка.
    // Карты и конфигурация генератора трофеев должны быть уже загружены.
    // Бросает std::invalid_argument, если снимок противоречит загруженным картам;
    // в этом случае состояние игры остаётся прежним.
    void RestoreSnapshot(const GameSnapshot& snapshot);

    // Игровой тик: двигает собак по дорогам, обрабатывает подбор предметов и сдачу
    // их в бюро находок (в хронологическом порядке), затем генерирует новые трофеи.
    void Tick(loot_gen::LootGenerator::TimeInterval delta);

    std::vector<LostObject> GetLostObjectsOnMap(const Map::Id& map_id) const;

    // Кладёт на карту потерянный предмет в заданной точке (используется и в тестах).
    // Бросает std::invalid_argument, если карты нет.
    LostObject::Id AddLostObject(const Map::Id& map_id, size_t type, Dog::Position position);

private:
    struct LootConfig {
        loot_gen::LootGenerator::TimeInterval period;
        double probability;
        loot_gen::LootGenerator::RandomGenerator random_gen;
    };

    struct MapLoot {
        std::vector<LostObject> objects;
        LostObject::Id next_id = 0;
        std::optional<loot_gen::LootGenerator> generator;
        // Восстановленное из сохранения время без генерации трофеев; применяется при
        // создании генератора.
        loot_gen::LootGenerator::TimeInterval pending_time_without_loot{0};
    };

    // Возвращает игроков, ушедших на покой за этот тик (они уже удалены из игры).
    std::vector<RetiredPlayer> AdvanceState(loot_gen::LootGenerator::TimeInterval delta);

    void RetireIdlePlayers(const Map& map, std::vector<RetiredPlayer>& retired);

    Dog::Position GenerateRandomPositionOnRoad(const Map& map);

    void MoveDogsAndCollect(size_t map_index, std::chrono::milliseconds delta);

    using MapIdHasher = util::TaggedHasher<Map::Id>;
    using MapIdToIndex = std::unordered_map<Map::Id, size_t, MapIdHasher>;

    static std::string GenerateToken();

    Maps maps_;
    MapIdToIndex map_id_to_index_;

    mutable std::mutex players_mutex_;
    std::vector<Player> players_;
    Player::Id next_player_id_ = 0;
    std::unordered_map<std::string, size_t> token_to_player_index_;

    // Всё ниже защищено тем же players_mutex_.
    std::optional<LootConfig> loot_config_;
    std::unordered_map<size_t, MapLoot> map_loot_;  // ключ - индекс карты в maps_
    std::mt19937_64 position_rng_{std::random_device{}()};
    std::chrono::milliseconds dog_retirement_time_{kDefaultDogRetirementTime};
    TickListener tick_listener_;
    RetirementListener retirement_listener_;
};

}  // namespace model
