# 命令行与基准程序

## CLI

```powershell
.\minidb_cli.exe --database example.mdb
.\minidb_cli.exe example.mdb --execute "SELECT * FROM users;"
.\minidb_cli.exe --database example.mdb --file examples\demo.sql
.\minidb_cli.exe example.mdb --buffer-pages 8 --batch
```

没有 `--execute` 或 `--file` 时从标准输入读取。交互模式显示 `MiniDB>`；未完成的语句显示续行提示符。分号结束语句，字符串、双引号标识符和注释中的分号不作为分隔符。输入结束时仍可执行最后一条没有分号的完整语句。`--batch` 隐藏提示符并在首个执行错误后停止，退出码为 1；`--file` 和 `--execute` 同样在错误后返回非零退出码。

结果显示列名、表格和行数，字符串中的换行、回车和制表符采用可见转义形式。列宽按字节计算，终端中的宽 Unicode 字符可能不完全对齐。

元命令单独占一行，在没有未完成 SQL 时使用：

| 命令 | 行为 |
|---|---|
| `.tables` | 列出表 |
| `.schema` | 输出 Schema |
| `.open PATH` | 打开另一个数据库，可用引号包含路径空格；关闭原连接会回滚其未提交工作 |
| `.checkpoint` | 执行检查点 |
| `.exit` / `.quit` | 退出，未提交工作通过 RAII 回滚 |
| `.help` | 显示帮助 |

`--file` 和 `--execute` 接受 SQL，不处理点号元命令。脚本支持 UTF-8 BOM。

## 实测性能

```powershell
.\minidb_bench.exe --sizes 1000,10000 --buffers 8,64 --queries 100 --batch-size 200 --csv results.csv
```

参数可调整表规模、缓冲池页数、查询次数和插入批次大小。`--directory` 指定测试数据库目录；每次运行创建唯一子目录，不覆盖已有数据库。`--csv` 指定报告文件，否则 CSV 输出到标准输出。进度与数据库路径输出到标准错误。

实际执行的工作负载：

1. 顺序主键插入；
2. 固定随机种子打乱后的主键插入；
3. 创建非唯一二级索引；
4. 主键点查询；
5. 主键范围查询；
6. 二级索引范围查询；
7. 返回所有记录的全表扫描；
8. 索引条件加额外谓词、排序与 LIMIT。

基准使用真实 Database/Connection API、真实页面文件和实际 WAL 同步；不模拟 I/O、不生成假结果。每个场景校验总行数，查询结果的行数累计到 checksum，防止只统计未执行的查询。

CSV 含规模、Buffer Pool 页数、操作数、总耗时、吞吐量、p50/p95 延迟及实际进程峰值内存。顺序与随机插入采用显式事务，延迟单位为 `committed_batch`（包含该批次 COMMIT）；吞吐量单位为行/秒。查询与创建索引的延迟单位为 `statement`，吞吐量为语句/秒。最后不足一批的插入也计为一次延迟样本。

Windows 通过 `GetProcessMemoryInfo().PeakWorkingSetSize` 采集内存，Linux 使用 `getrusage().ru_maxrss`；CSV 中统一为字节。该值是累计进程峰值，不是每个场景独立峰值。不同缓冲池容量在同一个进程依次运行，操作系统文件缓存也可能影响结果。测试数据库、WAL、查询结果物化和事务页副本的内存不受 Buffer Pool 页数单独限制。

基准仅报告真实运行数据，不保证与其他数据库的性能可比。正式性能报告需记录构建模式、操作系统、CPU、存储设备、数据规模及测试参数。
