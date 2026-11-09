# 实际验证与性能报告

日期：2026-11-10（Asia/Shanghai）。以下为本次实际运行结果，未使用其他数据库引擎代跑。

## 自动化验证

| 构建 | 结果 | 总时间 | 原始日志 |
|---|---|---|---|
| Windows x64 / MSVC 19.51.36260 / Release | 9/9 通过 | 7.21 秒 | [Windows CTest](ctest-windows.log) |
| VMware Ubuntu x86_64 / GCC 13.3.0 / Release，最终源码 | 9/9 通过 | 1.85 秒 | [Linux CTest](ctest-linux-final.log) |
| 同一 Ubuntu / Debug + AddressSanitizer + UndefinedBehaviorSanitizer，detect_leaks=1 | 9/9 通过 | 44.63 秒 | [Sanitizer 日志](ctest-sanitizers.log) |

套件为 storage、structures、sql、query、integration、concurrency、recovery、regression、cli。覆盖范围见 [测试文档](testing.md)。其中 recovery 启动真实子进程，在未提交、部分日志、日志刷盘、提交前、提交持久化后、半页写入及 checkpoint 边界强制退出，验证表与索引原子性、二次恢复、损坏检测和进程间文件锁。

SQL 与迭代器有 106 项断言，存储有 27 项断言，独立持久化查询有 14 项断言；完整 CTest 还包含集成、并发及回归测试，不能把套件数量视为断言总数。测试通过不是无缺陷证明，也不代表真实断电和任意硬件故障已验证。

CLI 示例与 C++ API 示例已在 Windows 实际运行；SQL 输出见 [演示记录](demo-output.txt)。Windows 与 Linux 的最终编译日志未发现编译器警告。初期模块预检曾有缩进告警，已通过统一格式化消除。

## 性能测量方法

- Windows 主机 CPU：Intel Core i7-14700HX；MSVC 19.51、CMake 4.3.1-msvc1。
- 用户已有 VMware Ubuntu：2 vCPU、约 3.8 GiB RAM、ext4、GCC 13.3、CMake 3.28.3、Linux 6.8.0-146-generic。
- 单表规模 1,000 和 10,000 行，BufferPool 8 和 64 页；每页 4096 字节。
- 顺序和随机插入分别执行真实 SQL，每 200 行一个提交事务；随机种子固定。
- 查询每项执行 100 条语句，全表扫描每配置执行 10 次。
- 插入吞吐单位为行/秒，插入延迟单位为整批提交耗时；查询吞吐单位为语句/秒，延迟为微秒。
- 内存列为 OS 返回的进程累计峰值工作集/RSS，包含所有缓存及结果集，不能当作当前单一算子的内存。
- 测量期间宿主机还有开发活动，部分 Linux 测量与 sanitizer 编译/测试重叠；结果反映这次实际运行，不是隔离负载性能上限，也不能直接进行 Windows/Linux 优劣排名。

完整原始 CSV：[Windows](benchmark-windows.csv)、[Linux](benchmark-linux.csv)。

```sh
minidb_bench --sizes 1000,10000 --buffers 8,64 --queries 100 --batch-size 200 --csv results.csv
```

## 实测摘要

| 平台 | 行数 | 缓存页 | 顺序插入 行/秒 | 主键点查 语句/秒 | 点查 p95 微秒 | 进程峰值 MiB |
|---|---:|---:|---:|---:|---:|---:|
| Windows | 1,000 | 8 | 1,256.10 | 10,787.25 | 127.60 | 6.80 |
| Windows | 1,000 | 64 | 1,283.70 | 27,425.06 | 50.30 | 7.18 |
| Windows | 10,000 | 8 | 95.52 | 8,743.25 | 152.70 | 12.00 |
| Windows | 10,000 | 64 | 110.38 | 11,221.45 | 117.10 | 12.34 |
| Linux | 1,000 | 8 | 1,753.61 | 8,341.75 | 122.45 | 5.13 |
| Linux | 1,000 | 64 | 1,988.65 | 26,506.96 | 41.68 | 5.13 |
| Linux | 10,000 | 8 | 130.12 | 11,414.35 | 138.61 | 8.43 |
| Linux | 10,000 | 64 | 164.11 | 17,437.35 | 84.56 | 8.59 |

## 已观察到的性能边界

小 BufferPool、大表逐条 INSERT 会反复扫描 Heap 寻找可用页，造成明显退化。当前没有持久化 free-space map；每个 INSERT 构造新的 HeapFile，跨语句不保留插入位置提示。该瓶颈与全页 WAL、同步提交叠加，不能据此承诺生产吞吐量。主键点查使用持久化 B+ Tree；二级范围查询还要通过 RID 回表，小缓存配置会产生更多页面读取。

排序、聚合、JOIN 和 ResultSet 会物化；事务私有页面也占用额外内存，因此 BufferPool 配置不是进程总内存上限。完整功能边界见 [已知限制](limitations.md)。
