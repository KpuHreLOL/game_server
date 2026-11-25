#include "json_loader.h"

#include <boost/json.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace json_loader {
namespace {
namespace json = boost::json;

constexpr json::string_view k_maps = "maps";
constexpr json::string_view k_default_dog_speed = "defaultDogSpeed";
constexpr json::string_view k_dog_speed = "dogSpeed";
constexpr json::string_view k_default_bag_capacity = "defaultBagCapacity";
constexpr json::string_view k_bag_capacity = "bagCapacity";
constexpr json::string_view k_value = "value";
constexpr json::string_view k_id = "id";
constexpr json::string_view k_name = "name";
constexpr json::string_view k_roads = "roads";
constexpr json::string_view k_buildings = "buildings";
constexpr json::string_view k_offices = "offices";
constexpr json::string_view k_loot_types = "lootTypes";
constexpr json::string_view k_loot_generator_config = "lootGeneratorConfig";
constexpr json::string_view k_period = "period";
constexpr json::string_view k_probability = "probability";
constexpr json::string_view k_dog_retirement_time = "dogRetirementTime";

constexpr json::string_view k_x0 = "x0";
constexpr json::string_view k_y0 = "y0";
constexpr json::string_view k_x1 = "x1";
constexpr json::string_view k_y1 = "y1";

constexpr json::string_view k_x = "x";
constexpr json::string_view k_y = "y";
constexpr json::string_view k_w = "w";
constexpr json::string_view k_h = "h";

constexpr json::string_view k_offset_x = "offsetX";
constexpr json::string_view k_offset_y = "offsetY";

constexpr json::string_view k_invalid_road_coordinate ="Invalid road coordinate";
constexpr json::string_view k_invalid_road_description ="Invalid road description";
constexpr json::string_view k_integer_value_out_of_range ="Integer value is out of range";
constexpr json::string_view k_failed_to_open_config_file = "Failed to open config file: ";

int AsInt(const json::value& value) {
    const auto integer_value = value.as_int64();

    if (integer_value < std::numeric_limits<int>::min() || integer_value > std::numeric_limits<int>::max()) {
        throw std::invalid_argument(std::string(k_integer_value_out_of_range));
    }

    return static_cast<int>(integer_value);
}

double AsDouble(const json::value& value) {
    if (value.is_double()) {
        return value.as_double();
    }
    if (value.is_int64()) {
        return static_cast<double>(value.as_int64());
    }
    throw std::invalid_argument("Expected number");
}

model::Road ParseRoad(const json::object& obj) {
    const model::Point start{AsInt(obj.at(k_x0)), AsInt(obj.at(k_y0))};

    if (auto it = obj.find(k_x1); it != obj.end()) {
        return model::Road(model::Road::HORIZONTAL,start,AsInt(it->value()));
    }

    if (auto it = obj.find(k_y1); it != obj.end()) {
        return model::Road(model::Road::VERTICAL,start,AsInt(it->value()));
    }

    throw std::invalid_argument(
        std::string(k_invalid_road_description));
}

model::Building ParseBuilding(const json::object& obj) {
    return model::Building{
        model::Rectangle{
            model::Point{AsInt(obj.at(k_x)),AsInt(obj.at(k_y))},
            model::Size{AsInt(obj.at(k_w)),AsInt(obj.at(k_h))}
        }
    };
}

model::Office ParseOffice(const json::object& obj) {
    return model::Office{
        model::Office::Id{std::string(obj.at(k_id).as_string().c_str())},
        model::Point{AsInt(obj.at(k_x)), AsInt(obj.at(k_y))},
        model::Offset{AsInt(obj.at(k_offset_x)),AsInt(obj.at(k_offset_y))}
    };
}

void LoadRoads(model::Map& map, const json::array& roads) {
    for (const auto& road_value : roads) {
        map.AddRoad(ParseRoad(road_value.as_object()));
    }
}

void LoadBuildings(model::Map& map, const json::array& buildings) {
    for (const auto& building_value : buildings) {
        map.AddBuilding(ParseBuilding(building_value.as_object()));
    }
}

void LoadOffices(model::Map& map, const json::array& offices) {
    for (const auto& office_value : offices) {
        map.AddOffice(ParseOffice(office_value.as_object()));
    }
}

// Возвращает описание типов трофеев для фронтенда (как есть из конфига).
json::array ParseLootTypes(const json::object& map_obj) {
    const auto it = map_obj.find(k_loot_types);
    if (it == map_obj.end() || !it->value().is_array()) {
        throw std::invalid_argument("Map must have lootTypes array");
    }
    json::array loot_types = it->value().as_array();
    if (loot_types.empty()) {
        throw std::invalid_argument("Map must have at least one loot type");
    }
    return loot_types;
}

void LoadLootGeneratorConfig(model::Game& game, const json::object& root_obj) {
    const auto it = root_obj.find(k_loot_generator_config);
    if (it == root_obj.end() || !it->value().is_object()) {
        throw std::invalid_argument("Config must have lootGeneratorConfig object");
    }
    const auto& config = it->value().as_object();

    const double period_sec = AsDouble(config.at(k_period));
    const double probability = AsDouble(config.at(k_probability));

    if (!std::isfinite(period_sec) || period_sec <= 0.0) {
        throw std::invalid_argument("lootGeneratorConfig.period must be positive");
    }
    if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0) {
        throw std::invalid_argument("lootGeneratorConfig.probability must be in [0, 1]");
    }

    const auto period = std::chrono::milliseconds{std::llround(period_sec * 1000.0)};
    if (period.count() <= 0) {
        throw std::invalid_argument("lootGeneratorConfig.period is too small");
    }

    game.SetLootGeneratorConfig(period, probability);
}

void LoadDogRetirementTime(model::Game& game, const json::object& root_obj) {
    const auto it = root_obj.find(k_dog_retirement_time);
    if (it == root_obj.end()) {
        return;  // остаётся значение по умолчанию (одна минута)
    }

    const double seconds = AsDouble(it->value());
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 1e9) {
        throw std::invalid_argument("dogRetirementTime must be a non-negative number of seconds");
    }
    game.SetDogRetirementTime(std::chrono::milliseconds{std::llround(seconds * 1000.0)});
}

size_t ParseBagCapacity(const json::value& value) {
    const auto capacity = value.as_int64();
    if (capacity < 0 || capacity > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("Bag capacity must be a non-negative integer");
    }
    return static_cast<size_t>(capacity);
}

std::vector<int> ParseLootValues(const json::array& loot_types) {
    std::vector<int> values;
    values.reserve(loot_types.size());
    for (const auto& type_value : loot_types) {
        const int value = AsInt(type_value.as_object().at(k_value));
        if (value < 0) {
            throw std::invalid_argument("Loot value must be non-negative");
        }
        values.push_back(value);
    }
    return values;
}

model::Map ParseMap(const json::object& obj, double default_dog_speed, size_t default_bag_capacity) {
    double dog_speed = default_dog_speed;
    if (auto it = obj.find(k_dog_speed); it != obj.end()) {
        dog_speed = AsDouble(it->value());
    }

    model::Map map{
        model::Map::Id{obj.at(k_id).as_string().c_str()},
        std::string(obj.at(k_name).as_string().c_str()),
        dog_speed
    };

    size_t bag_capacity = default_bag_capacity;
    if (auto it = obj.find(k_bag_capacity); it != obj.end()) {
        bag_capacity = ParseBagCapacity(it->value());
    }
    map.SetBagCapacity(bag_capacity);

    LoadRoads(map, obj.at(k_roads).as_array());
    LoadBuildings(map, obj.at(k_buildings).as_array());
    LoadOffices(map, obj.at(k_offices).as_array());

    return map;
}

}  // namespace

LoadedGame LoadGame(const std::filesystem::path& json_path) {
    std::ifstream input(json_path);

    if (!input) {
        throw std::runtime_error(
            std::string(k_failed_to_open_config_file) +
            json_path.string());
    }

    std::stringstream buffer;
    buffer << input.rdbuf();

    json::value root;
        try {
            root = json::parse(buffer.str());
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to parse config file " + json_path.string() + ": " + e.what());
        }
    const auto& root_obj = root.as_object();
    const auto& maps = root_obj.at(k_maps).as_array();

    double default_dog_speed = 1.0;
    if (auto it = root_obj.find(k_default_dog_speed); it != root_obj.end()) {
        default_dog_speed = AsDouble(it->value());
    }

    size_t default_bag_capacity = model::kDefaultBagCapacity;
    if (auto it = root_obj.find(k_default_bag_capacity); it != root_obj.end()) {
        default_bag_capacity = ParseBagCapacity(it->value());
    }

    model::Game game;
    extra_data::MapExtraData extra;

    LoadLootGeneratorConfig(game, root_obj);
    LoadDogRetirementTime(game, root_obj);

    for (const auto& map_value : maps) {
        const auto& map_obj = map_value.as_object();

        model::Map map = ParseMap(map_obj, default_dog_speed, default_bag_capacity);
        json::array loot_types = ParseLootTypes(map_obj);
        map.SetLootTypeCount(loot_types.size());
        map.SetLootValues(ParseLootValues(loot_types));

        const model::Map::Id map_id = map.GetId();
        game.AddMap(std::move(map));
        extra.SetLootTypes(map_id, std::move(loot_types));
    }

    return LoadedGame{std::move(game), std::move(extra)};
}

}  // namespace json_loader
