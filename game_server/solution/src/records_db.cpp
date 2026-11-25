#include "records_db.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace records_db {

using namespace std::literals;

namespace {

constexpr std::size_t kMaxNameChars = 100;  // varchar(100)

// Обрезает строку до max_chars символов UTF-8, не разрезая многобайтовые символы.
std::string TruncateUtf8(const std::string& str, std::size_t max_chars) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < str.size(); ++i) {
        const auto byte = static_cast<unsigned char>(str[i]);
        if ((byte & 0xC0) != 0x80) {  // начало символа
            if (chars == max_chars) {
                return str.substr(0, i);
            }
            ++chars;
        }
    }
    return str;
}

std::string GenerateUuid() {
    thread_local boost::uuids::random_generator generator;
    return boost::uuids::to_string(generator());
}

}  // namespace

ConnectionPool::ConnectionWrapper ConnectionPool::GetConnection() {
    std::unique_lock lock{mutex_};
    cond_var_.wait(lock, [this] {
        return used_connections_ < pool_.size();
    });
    return {std::move(pool_[used_connections_++]), *this};
}

void ConnectionPool::ReturnConnection(ConnectionPtr&& conn) {
    {
        std::lock_guard lock{mutex_};
        pool_[--used_connections_] = std::move(conn);
    }
    cond_var_.notify_one();
}

PostgresRecordsStorage::PostgresRecordsStorage(const std::string& db_url, std::size_t pool_size)
    : pool_{std::max<std::size_t>(1, pool_size), [&db_url] {
                return std::make_shared<pqxx::connection>(db_url);
            }} {
    auto conn = pool_.GetConnection();
    pqxx::work work{*conn};
    work.exec(R"(
CREATE TABLE IF NOT EXISTS retired_players (
    id UUID CONSTRAINT retired_players_id_constraint PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    score INTEGER NOT NULL,
    play_time_ms INTEGER NOT NULL
);
)");
    // Индекс соответствует порядку сортировки таблицы рекордов.
    work.exec(R"(
CREATE INDEX IF NOT EXISTS retired_players_score_play_time_name_idx
    ON retired_players (score DESC, play_time_ms ASC, name ASC);
)");
    work.commit();
}

void PostgresRecordsStorage::Add(const records::Record& record) {
    const auto play_time_ms = std::clamp<std::int64_t>(
        record.play_time.count(), 0, std::numeric_limits<std::int32_t>::max());

    auto conn = pool_.GetConnection();
    pqxx::work work{*conn};
    work.exec_params(
        "INSERT INTO retired_players (id, name, score, play_time_ms) VALUES ($1, $2, $3, $4);",
        GenerateUuid(), TruncateUtf8(record.name, kMaxNameChars), record.score, play_time_ms);
    work.commit();
}

std::vector<records::Record> PostgresRecordsStorage::Get(std::size_t start, std::size_t max_items) {
    auto conn = pool_.GetConnection();
    pqxx::read_transaction tx{*conn};

    const auto rows = tx.exec_params(
        "SELECT name, score, play_time_ms FROM retired_players "
        "ORDER BY score DESC, play_time_ms ASC, name ASC LIMIT $1 OFFSET $2;",
        static_cast<std::int64_t>(max_items), static_cast<std::int64_t>(start));

    std::vector<records::Record> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back(records::Record{row[0].as<std::string>(), row[1].as<int>(),
                                         std::chrono::milliseconds{row[2].as<std::int64_t>()}});
    }
    return result;
}

}  // namespace records_db
