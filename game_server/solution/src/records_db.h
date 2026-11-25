#pragma once

#include <pqxx/pqxx>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "records.h"

namespace records_db {

// Пул соединений с PostgreSQL. Соединения нельзя использовать из нескольких потоков
// одновременно, поэтому каждый поток берёт собственное соединение на время работы.
class ConnectionPool {
    using PoolType = ConnectionPool;
    using ConnectionPtr = std::shared_ptr<pqxx::connection>;

public:
    class ConnectionWrapper {
    public:
        ConnectionWrapper(ConnectionPtr&& conn, PoolType& pool) noexcept
            : conn_{std::move(conn)}
            , pool_{&pool} {
        }

        ConnectionWrapper(const ConnectionWrapper&) = delete;
        ConnectionWrapper& operator=(const ConnectionWrapper&) = delete;
        ConnectionWrapper(ConnectionWrapper&&) = default;
        ConnectionWrapper& operator=(ConnectionWrapper&&) = default;

        pqxx::connection& operator*() const& noexcept {
            return *conn_;
        }
        pqxx::connection* operator->() const& noexcept {
            return conn_.get();
        }

        ~ConnectionWrapper() {
            if (conn_) {
                pool_->ReturnConnection(std::move(conn_));
            }
        }

    private:
        ConnectionPtr conn_;
        PoolType* pool_;
    };

    template <typename ConnectionFactory>
    ConnectionPool(std::size_t capacity, ConnectionFactory&& connection_factory) {
        pool_.reserve(capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            pool_.emplace_back(connection_factory());
        }
    }

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    ConnectionWrapper GetConnection();

private:
    void ReturnConnection(ConnectionPtr&& conn);

    std::mutex mutex_;
    std::condition_variable cond_var_;
    std::vector<ConnectionPtr> pool_;
    std::size_t used_connections_ = 0;
};

// Таблица рекордов в PostgreSQL (таблица retired_players).
class PostgresRecordsStorage final : public records::RecordsStorage {
public:
    // Создаёт пул соединений и, если нужно, таблицу с индексами.
    // Бросает исключение, если к базе данных подключиться не удалось.
    PostgresRecordsStorage(const std::string& db_url, std::size_t pool_size);

    void Add(const records::Record& record) override;
    std::vector<records::Record> Get(std::size_t start, std::size_t max_items) override;

private:
    ConnectionPool pool_;
};

}  // namespace records_db
