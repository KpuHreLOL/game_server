#pragma once

#include <boost/json.hpp>

#include <unordered_map>
#include <utility>

#include "model.h"

namespace extra_data {

/*
 * Данные, нужные только фронтенду и не влияющие на игровую логику.
 * Хранятся отдельно от модели, чтобы модель не зависела от JSON.
 * Сейчас здесь описание типов трофеев (lootTypes) для каждой карты.
 */
class MapExtraData {
public:
    void SetLootTypes(const model::Map::Id& map_id, boost::json::array loot_types) {
        loot_types_.insert_or_assign(map_id, std::move(loot_types));
    }

    // nullptr, если для карты ничего не сохранено.
    const boost::json::array* GetLootTypes(const model::Map::Id& map_id) const noexcept {
        if (auto it = loot_types_.find(map_id); it != loot_types_.end()) {
            return &it->second;
        }
        return nullptr;
    }

private:
    std::unordered_map<model::Map::Id, boost::json::array, util::TaggedHasher<model::Map::Id>> loot_types_;
};

}  // namespace extra_data
