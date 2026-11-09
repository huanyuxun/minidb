#include "minidb/database.hpp"
#include <filesystem>
#include <iostream>

// Each thread should use its own Connection. The Database may be shared.
int main(int argc, char **argv) {
    try {
        const std::filesystem::path path = argc > 1 ? argv[1] : "api-example.db";
        minidb::Database database(path);
        auto connection = database.connect();
        if (connection->tables().empty()) {
            connection->execute(
                "CREATE TABLE users(id INTEGER PRIMARY KEY,name VARCHAR(100) NOT NULL)");
            connection->execute("BEGIN");
            connection->execute("INSERT INTO users VALUES(1,'Alice'),(2,'Bob')");
            connection->execute("COMMIT");
        }
        auto statement = connection->prepare("SELECT id,name FROM users ORDER BY id");
        auto result = statement.execute();
        for (const auto &row : result.rows) {
            std::cout << minidb::value_string(row[0]) << ": " << minidb::value_string(row[1])
                      << '\n';
        }
        connection->execute("BEGIN");
        connection->execute("UPDATE users SET name='Discarded' WHERE id=1");
        connection->execute("ROLLBACK");
        connection->execute("CHECKPOINT");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
