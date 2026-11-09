#include "minidb/database.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
using namespace minidb;
std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}
// SQL delimiters inside quoted strings, identifiers and comments are not statement boundaries.
std::vector<std::string> statements(std::string &pending) {
    std::vector<std::string> out;
    std::size_t start = 0;
    char quote = 0;
    bool line = false, block = false;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const char c = pending[i], next = i + 1 < pending.size() ? pending[i + 1] : '\0';
        if (line) {
            if (c == '\n')
                line = false;
            continue;
        }
        if (block) {
            if (c == '*' && next == '/') {
                block = false;
                ++i;
            }
            continue;
        }
        if (quote) {
            if (c == quote) {
                if (next == quote)
                    ++i;
                else
                    quote = 0;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '-' && next == '-') {
            line = true;
            ++i;
            continue;
        }
        if (c == '/' && next == '*') {
            block = true;
            ++i;
            continue;
        }
        if (c == ';') {
            out.push_back(pending.substr(start, i - start + 1));
            start = i + 1;
        }
    }
    pending.erase(0, start);
    return out;
}
bool has_sql(const std::string &sql) {
    auto tokens = lex(sql);
    return tokens.size() > 1;
}
std::string display(const std::string &value) {
    std::string out;
    for (char c : value) {
        if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else
            out += c;
    }
    return out;
}
void print(const ResultSet &result) {
    if (!result.columns.empty()) {
        std::vector<std::size_t> widths;
        for (const auto &column : result.columns)
            widths.push_back(display(column).size());
        std::vector<std::vector<std::string>> rows;
        for (const auto &row : result.rows) {
            std::vector<std::string> values;
            for (std::size_t i = 0; i < row.size(); ++i) {
                values.push_back(display(value_string(row[i])));
                if (i < widths.size())
                    widths[i] = std::max(widths[i], values.back().size());
            }
            rows.push_back(std::move(values));
        }
        auto border = [&] {
            std::cout << '+';
            for (auto width : widths)
                std::cout << std::string(width + 2, '-') << '+';
            std::cout << '\n';
        };
        auto line = [&](const std::vector<std::string> &cells) {
            std::cout << '|';
            for (std::size_t i = 0; i < widths.size(); ++i)
                std::cout << ' ' << std::left << std::setw(static_cast<int>(widths[i]))
                          << (i < cells.size() ? cells[i] : "") << " |";
            std::cout << '\n';
        };
        border();
        std::vector<std::string> headers;
        for (const auto &c : result.columns)
            headers.push_back(display(c));
        line(headers);
        border();
        for (const auto &row : rows)
            line(row);
        border();
        std::cout << result.rows.size() << " row(s)\n";
    } else if (!result.message.empty())
        std::cout << result.message << '\n';
    else
        std::cout << result.affected_rows << " row(s) affected\n";
    if (!result.plan.empty())
        std::cout << "Plan: " << result.plan << '\n';
}
std::size_t number(const std::string &text) {
    std::size_t value = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || value == 0)
        throw std::runtime_error("expected positive buffer page count");
    return value;
}
void usage() {
    std::cout << "MiniDB --database PATH [--buffer-pages N] [--execute SQL | --file SCRIPT]\n"
              << "       MiniDB [PATH] [--batch]\n"
              << "Meta commands: .tables .schema .open PATH .checkpoint .exit .help\n";
}
class Shell {
    Options options_;
    std::unique_ptr<Database> database_;
    std::unique_ptr<Connection> connection_;
    std::filesystem::path path_;

  public:
    explicit Shell(Options options) : options_(options) {}
    void open(const std::string &path) {
        auto normalized = std::filesystem::absolute(path).lexically_normal();
        if (database_ && normalized == path_) {
            connection_.reset();
            connection_ = database_->connect();
            return;
        }
        // Prepare the replacement before releasing the old database, so a bad path
        // leaves the shell usable. Releasing the old connection rolls back its work.
        auto next = std::make_unique<Database>(path, options_);
        auto connection = next->connect();
        connection_.reset();
        database_ = std::move(next);
        connection_ = std::move(connection);
        path_ = std::move(normalized);
    }
    void execute(const std::string &sql) {
        if (has_sql(sql))
            print(connection_->execute(sql));
    }
    void script(std::string source) {
        for (const auto &sql : statements(source))
            execute(sql);
        if (has_sql(source))
            execute(source);
    }
    bool meta(const std::string &text) {
        auto command = trim(text);
        if (command == ".exit" || command == ".quit")
            return false;
        if (command == ".help") {
            usage();
            return true;
        }
        if (command == ".tables") {
            for (const auto &table : connection_->tables())
                std::cout << table << '\n';
            return true;
        }
        if (command == ".schema") {
            std::cout << connection_->schema();
            return true;
        }
        if (command == ".checkpoint") {
            execute("CHECKPOINT");
            return true;
        }
        if (command.starts_with(".open ")) {
            auto path = trim(command.substr(6));
            if (path.size() > 1 && ((path.front() == '"' && path.back() == '"') ||
                                    (path.front() == '\'' && path.back() == '\'')))
                path = path.substr(1, path.size() - 2);
            if (path.empty())
                throw std::runtime_error(".open requires a path");
            open(path);
            std::cout << "Opened " << path << '\n';
            return true;
        }
        throw std::runtime_error("unknown meta command; use .help");
    }
    int input(bool interactive) {
        std::string pending, line;
        bool failed = false;
        while (true) {
            if (interactive)
                std::cout << (trim(pending).empty() ? "MiniDB> " : "    ... ") << std::flush;
            if (!std::getline(std::cin, line))
                break;
            try {
                auto clean = trim(line);
                if (trim(pending).empty() && clean.starts_with('.')) {
                    if (!meta(clean))
                        return failed ? 1 : 0;
                    continue;
                }
                pending += line + '\n';
                for (const auto &sql : statements(pending))
                    execute(sql);
                if (!pending.empty() && !has_sql(pending))
                    pending.clear();
            } catch (const SqlError &e) {
                // An unfinished quote/comment is valid while collecting another line.
                std::string message = e.what();
                if (message.find("unterminated") != std::string::npos)
                    continue;
                std::cerr << "Error: " << message << '\n';
                failed = true;
                pending.clear();
                if (!interactive)
                    return 1;
            } catch (const std::exception &e) {
                std::cerr << "Error: " << e.what() << '\n';
                failed = true;
                pending.clear();
                if (!interactive)
                    return 1;
            }
        }
        try {
            if (has_sql(pending))
                execute(pending);
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << '\n';
            return 1;
        }
        return failed ? 1 : 0;
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        std::string path = "minidb.db", sql, file;
        bool execute_set = false, file_set = false, batch = false, positional = false;
        minidb::Options options;
        for (int i = 1; i < argc; ++i) {
            std::string argument = argv[i];
            auto value = [&]() {
                if (i + 1 >= argc)
                    throw std::runtime_error("missing value for " + argument);
                return std::string(argv[++i]);
            };
            if (argument == "--help" || argument == "-h") {
                usage();
                return 0;
            }
            if (argument == "--database")
                path = value();
            else if (argument == "--execute" || argument == "-e") {
                sql = value();
                execute_set = true;
            } else if (argument == "--file" || argument == "-f") {
                file = value();
                file_set = true;
            } else if (argument == "--buffer-pages")
                options.buffer_pages = number(value());
            else if (argument == "--batch")
                batch = true;
            else if (!argument.starts_with('-') && !positional) {
                path = argument;
                positional = true;
            } else
                throw std::runtime_error("unknown argument " + argument);
        }
        if (execute_set && file_set)
            throw std::runtime_error("choose --execute or --file");
        Shell shell(options);
        shell.open(path);
        if (execute_set) {
            shell.script(sql);
            return 0;
        }
        if (file_set) {
            std::ifstream input(file, std::ios::binary);
            if (!input)
                throw std::runtime_error("cannot open script " + file);
            std::ostringstream source;
            source << input.rdbuf();
            auto text = source.str();
            if (text.starts_with("\xef\xbb\xbf"))
                text.erase(0, 3);
            shell.script(text);
            return 0;
        }
        if (!batch)
            std::cout << "MiniDB C++20 relational database. Type .help for help.\n";
        return shell.input(!batch);
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
