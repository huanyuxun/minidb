# 页面、缓存、WAL 与恢复

核心实现：`src/platform.cpp`、`src/storage.cpp`。数据文件和同路径加 `.wal` 的日志文件均由本项目实现，不依赖其他数据库。

## 数据文件格式（版本 1）

所有整数显式采用 little endian。固定页面 4096 字节，不直接序列化 C++ 对象。页号是 32 位无符号整数，`0xffffffff` 表示不存在。页 0 是 Catalog；文件中的页面偏移为 `page_id * 4096`。

| 字节偏移 | 长度 | 含义 |
|---|---:|---|
| 0 | 4 | magic `MDBP` |
| 4 | 2 | 格式版本 1 |
| 6 | 2 | 类型：Catalog=1、Heap=2、BTreeLeaf=3、BTreeInternal=4、Free=5 |
| 8 | 4 | 页号 |
| 12 | 4 | CRC32，计算时本字段置 0 |
| 16 | 8 | 本页最后修改的 LSN |
| 24 | 8 | 保留，目前为 0 |
| 32 | 4064 | 类型相关载荷 |

CRC32 使用 IEEE 多项式 `0xedb88320`，覆盖完整页面。打开数据库时恢复先运行，再检查每页的 magic、版本、页号、类型和 CRC32。因此已提交 WAL 中的全页镜像能够修复相应数据页的撕裂写入。没有可用日志修复的损坏会返回 `CORRUPTION`，不会用零页静默代替。

释放页写为 Free；打开时从页面类型重建空闲页集合。事务分配优先复用空闲页，否则扩展页号。回滚不扩展物理文件。释放页不会缩短文件，页 0 不允许释放。

## 平台层与缓存

Windows 使用 `CreateFileW`、`LockFileEx`、`FlushFileBuffers`；Linux 使用 `open/pread/pwrite`、`flock`、`fsync`。数据库文件保持独占进程锁，第二个实例打开同文件会失败。文件句柄由 RAII 管理，系统调用均封装在 platform 模块。Linux 新文件创建会同步父目录；Windows 文件路径支持 Unicode。

BufferPool 采用固定帧容量与 LRU。PageGuard 是不可复制、可移动的 RAII 对象，持有帧锁与 pin；析构时释放二者。替换只选 pin 为 0 的页，脏页在替换前写入磁盘。缓存支持并发 fetch/put/flush，统计 hits/misses/evictions。所有帧已固定时最多等待 5 秒，之后返回 `BUFFER_POOL_EXHAUSTED`；调用者应及时释放 Guard，不能在同一线程持有某页 Guard 时再次获取该页。

底层 `BufferPool::put` 的契约是：对应 WAL 必须已持久化。上层 PageTransaction 在提交前将修改保存在独立页集合，不把未提交页放入缓存。提交完成前，整个数据库排他锁仍由 Connection 持有。事务私有页集合不计入 BufferPool 容量；大事务因此可能占用大量内存。

## WAL 格式

每条日志有 40 字节头，类型 PAGE 的日志后跟完整 4096 字节页面，其余日志无载荷。日志由一个串行提交者写入。

| 字节偏移 | 长度 | 含义 |
|---|---:|---|
| 0 | 4 | magic `MWAL` |
| 4 | 2 | 格式版本 1 |
| 6 | 2 | BEGIN=1、PAGE=2、COMMIT=3、CHECKPOINT=4 |
| 8 | 4 | 整条日志长度，40 或 4136 |
| 12 | 4 | 页号，无对应页时 `0xffffffff` |
| 16 | 8 | 严格递增 LSN |
| 24 | 8 | 事务号，CHECKPOINT 为 0 |
| 32 | 4 | CRC32，计算时本字段置 0 |
| 36 | 4 | 保留，目前为 0 |

PAGE 载荷页 LSN 与其日志 LSN 相同。事务号在单次打开期间唯一；重启及 checkpoint 后允许复用事务号。LSN 根据保留日志和数据页最大 LSN 继续分配。

## 提交和回滚

1. 修改发生在 PageTransaction 的私有全页副本中（no-steal）。
2. 提交先写 BEGIN、全部 PAGE 日志并同步 WAL。
3. 写 COMMIT，再次同步 WAL。这是持久提交点。
4. 已提交页面进入 BufferPool，全部刷入数据文件，并同步数据文件（force）。
5. 更新内存页数、空闲页集合和事务状态，然后上层释放数据库锁。

因此任何数据写入前，对应 PAGE 和 COMMIT 都已持久化。回滚仅丢弃私有副本，既不需要物理 undo，也不会留下未提交数据页。事务析构执行回滚。底层同时存在的过期事务会因 generation 不匹配返回 `WRITE_CONFLICT`；SQL 层的严格数据库锁避免正常事务遇到此冲突。

提交或 checkpoint 的 I/O 异常会把 Pager 标记为 poisoned，后续操作拒绝执行，必须重新打开恢复。提交持久点之后的 I/O 异常属于结果不确定：调用者应重开并检查，不能认定提交失败就一定回滚。

## 重启与 checkpoint

恢复顺序扫描日志并验证记录 CRC、LSN、事务边界与页镜像，仅对出现有效 COMMIT 的事务按顺序覆盖数据页。全页覆盖可以重复执行，恢复中再次崩溃不会重复插入行。没有 COMMIT 的事务全部丢弃。日志末尾不足一个头或不足声明记录长度的片段被视为不完整尾部；完整记录 CRC 不匹配或非法头会拒绝恢复，避免把介质损坏误当成未提交尾部。

恢复完成后先同步数据文件，再清空并同步 WAL。显式 CHECKPOINT 先同步缓存与数据，再写入并同步 CHECKPOINT 记录，最后截断并同步 WAL。截断前后崩溃均安全：旧日志能重复重放，空日志对应已经持久化的数据。

这是一种 no-steal/force 全页 redo 协议，不是 ARIES，也没有磁盘 before-image/undo、模糊 checkpoint、增量备份或日志归档。成功提交依赖操作系统及存储设备遵守 flush/fsync 的持久性契约；校验和用于检测，不能恢复没有日志覆盖的随机介质损坏。WAL 没有自动大小阈值，长时间运行应定期执行 CHECKPOINT。当前恢复扫描与启动页面校验为线性开销。

## 测试与故障注入

`storage_test` 覆盖 CRC32 标准向量、页错误检测、文件锁、缓存 pin/移动/替换/刷盘、8 线程缓存读取、事务分配回滚、析构回滚、过期事务拒绝、空闲页复用、重启、checkpoint 和全页日志修复。

环境变量 `MINIDB_CRASH_POINT` 仅用于测试，匹配故障点后以退出码 86 调用 `_Exit`，不运行析构：

| 故障点 | 触发时机 |
|---|---|
| wal_partial | 写入半条 PAGE 日志并同步后 |
| wal_flushed | PAGE 日志持久化后、COMMIT 之前 |
| before_commit | 同上，独立的提交前注入点 |
| commit_flushed | COMMIT 持久化后、数据页写入前 |
| data_write | 写入并同步半个数据页后 |
| checkpoint | 数据及 CHECKPOINT 日志同步后、WAL 截断前 |

请勿在日常运行中设置该变量。具体进程级恢复测试见 `recovery-testing.md`。
