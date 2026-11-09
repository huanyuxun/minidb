# MiniDB

MiniDB 是从页面、记录和索引开始实现的单机 C++20 关系数据库。核心代码不依赖 SQLite、MySQL 或其他数据库引擎。它提供持久化 SQL 数据库、交互命令行、C++ 库接口和自动化测试。

当前实现面向教学、数据库内核实验和本地小规模数据。并发采用数据库级严格两阶段锁；写事务串行执行。完整边界见 [已知限制](docs/limitations.md)，实测结果见 [验证报告](docs/results.md)。

## 构建

本次交付的 Windows x64 Release 程序位于 `bin/`。在项目目录中可直接运行：

```powershell
.\bin\minidb_cli.exe --database demo.db
```

这些程序已在当前 Windows 主机运行验证；迁移到其他机器可能需要安装 Microsoft Visual C++ x64 Runtime。源码包可按下面步骤重新构建。

需要 C++20 编译器和 CMake 3.20 或更高版本，无第三方 C++ 库下载步骤。

Windows x64：在 **x64 Native Tools Command Prompt for VS** 中执行：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
build\minidb_cli.exe --database demo.db
```

也可使用 Visual Studio CMake 生成器；多配置生成器需要 `cmake --build build --config Release` 和 `ctest --test-dir build -C Release`，程序位于 `build\Release`。

Linux：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/minidb_cli --database demo.db
```

GCC/Clang 的可选检查构建：`cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMINIDB_SANITIZE=ON`，然后构建并运行 CTest。此选项启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。

## 使用

```sql
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    age INTEGER,
    score DOUBLE
);
INSERT INTO users VALUES (1, 'Alice', 20, 95.5), (2, 'Bob', 22, 88.0);
CREATE INDEX idx_users_age ON users(age);
SELECT name, age FROM users WHERE age >= 18 ORDER BY score DESC LIMIT 10;
BEGIN;
UPDATE users SET age = age + 1 WHERE id = 1;
ROLLBACK;
SELECT COUNT(*), AVG(score) FROM users;
EXPLAIN SELECT * FROM users WHERE id = 1;
CHECKPOINT;
```

退出后用相同路径重新启动即可读取已提交数据。数据库使用页面文件和同目录 WAL 文件，备份或移动时应先正常关闭所有连接，再同时处理这两个文件。不要在数据库打开期间手动复制、删除或修改文件。

CLI 支持多行 SQL、表格结果、`.tables`、`.schema`、`.open PATH`、`.checkpoint`、`.exit`。脚本与单语句：

```sh
./build/minidb_cli --database demo.db --file examples/demo.sql
./build/minidb_cli --database demo.db --execute "SELECT * FROM users;"
```

`--buffer-pages N` 设置固定容量的已提交页面缓存。事务私有页、排序、连接及结果集另占内存。CLI 细节和基准参数见 [命令行与基准程序](docs/cli-benchmark.md)。

## C++ API

```cpp
#include <minidb/database.hpp>
#include <iostream>

int main() {
    minidb::Database db("example.db", minidb::Options{64});
    auto connection = db.connect();
    auto result = connection->execute("SELECT 1 + 2 AS answer");
    std::cout << minidb::value_string(result.rows.at(0).at(0)) << '\n';
}
```

使用 CMake 的 `add_subdirectory(...)` 后链接 `minidb` target。每个线程使用独立 `Connection`；共享一个 `Database`。连接析构会回滚尚未提交的事务。`prepare(sql)` 返回已解析的语句对象；执行时原连接必须仍然存活。当前没有参数绑定接口。运行错误使用 C++ 异常报告。

## 工程结构

```text
include/minidb/    公开类型、SQL、存储、索引、执行器、锁和数据库接口
src/              各模块实现，platform.cpp 集中封装 Windows/POSIX I/O
tests/            SQL、页面、结构、集成、并发和子进程崩溃恢复测试
bench/            真实 SQL/API 性能测试程序
examples/         可运行 SQL 脚本
docs/             架构、文件格式、语法、限制和验证记录
```

CTest 包含九个测试套件：`storage`、`structures`、`sql`、`query`、`integration`、`concurrency`、`recovery`、`regression`、`cli`。测试使用临时数据库，崩溃测试启动子进程并通过故障注入强制退出。

基准示例：

```sh
./build/minidb_bench --sizes 1000,10000 --buffers 8,64 --queries 100 --batch-size 200 --csv results.csv
```

报告含吞吐量、p50/p95 延迟与实际进程峰值内存。插入延迟包含每批事务提交，其他工作负载按语句计时。不同缓冲池配置和数据规模均真实执行；不以其他数据库代跑。

## 设计资料

- [架构与事务保证](docs/architecture.md)
- [页面、WAL 与恢复格式](docs/storage.md)
- [Catalog、Tuple 与索引编码](docs/catalog-format.md)
- [Heap 与 B+ Tree 布局](docs/structures.md)
- [SQL 子集和表达式语义](docs/sql.md)
- [已知限制](docs/limitations.md)
- [实际验证与性能数据](docs/results.md)
- [测试套件说明](docs/testing.md)
