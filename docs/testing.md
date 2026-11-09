# 测试入口与覆盖范围

推荐通过 CTest 统一执行，测试不依赖第三方测试框架。所有断言失败都会返回非零退出码。

| 套件 | 覆盖 |
|---|---|
| storage | 页面 CRC、文件锁、LRU/pin/dirty/刷盘、并发缓存读取、事务页分配/释放/回滚、WAL 修复 |
| structures | SlottedPage、Heap 变长更新和复用、持久化 B+ Tree 多层分裂、重复键、范围、删除合并/重分配、随机模型对照 |
| sql | Lexer、Parser、AST、表达式、类型边界、三值逻辑、迭代执行器 |
| query | 持久化页面到查询结果、索引范围、分组聚合、排序分页、自连接及查询绑定 |
| integration | SQL → AST → 执行 → 存储 → ResultSet；多表、约束、索引维护、事务、DDL、重启 |
| concurrency | 共享/排他锁、超时、锁升级死锁受害者、多线程读取和写入、事务原子性、索引一致性 |
| recovery | 七个实际子进程退出边界、二次恢复、WAL/页面损坏检测、跨进程文件锁 |
| regression | 大整数/DOUBLE 索引与全扫一致性、空表绑定、TEXT、JOIN 同名列、多页目录、UPDATE 原始行求值 |
| cli | SQL 脚本、多行/注释/引号内分号、元命令、错误退出、未提交退出回滚 |

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'recovery|concurrency' --output-on-failure
```

Windows 多配置构建加 `-C Release`。测试使用本机临时目录，不操作用户数据库；存储、结构、集成及并发套件使用固定测试文件名，因此不要同时运行两份同一套件。不同套件可并行。

`MINIDB_SANITIZE=ON` 在 GCC/Clang 下启用 ASan/UBSan。构建选项不代表已经执行，实际检查结果以 [验证记录](results.md) 为准。

性能程序 `minidb_bench` 是独立可重复运行的测量程序，不属于“阈值通过”测试。CSV 包含吞吐量、延迟百分位和进程峰值内存，单位、批次大小、缓存及数据规模详见 [CLI 与基准说明](cli-benchmark.md)。
