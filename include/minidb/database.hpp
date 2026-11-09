#pragma once
#include "minidb/sql.hpp"
#include <chrono>
#include <filesystem>
namespace minidb {
struct Options {
    size_t buffer_pages = 64;
    std::chrono::milliseconds lock_timeout{1000};
};
struct ResultSet {
    std::vector<std::string> columns;
    std::vector<Row> rows;
    size_t affected_rows = 0;
    std::string message;
    std::string plan;
};
class Connection;
class PreparedStatement;
class Database {
  public:
    explicit Database(const std::filesystem::path &path, Options options = {});
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    std::unique_ptr<Connection> connect();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend class Connection;
};
class Connection {
  public:
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ResultSet execute(const std::string &sql);
    ResultSet execute(const Statement &statement);
    PreparedStatement prepare(const std::string &sql);
    std::vector<std::string> tables();
    std::string schema();
    bool in_transaction() const;

  private:
    explicit Connection(std::shared_ptr<Database::Impl> db);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Database;
};
class PreparedStatement {
    Connection *connection_;
    Statement statement_;

  public:
    PreparedStatement(Connection &connection, Statement statement)
        : connection_(&connection), statement_(std::move(statement)) {}
    ResultSet execute() {
        return connection_->execute(statement_);
    }
};
} // namespace minidb
