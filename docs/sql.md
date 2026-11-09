# SQL 语法与表达式

MiniDB 使用独立词法分析器、递归下降语法分析器、结构化 AST 和表达式求值器。关键字大小写不敏感；未加双引号的标识符规范化为小写。双引号标识符保留大小写；单引号字符串用两个连续单引号转义。支持 `--` 行注释和 `/* ... */` 块注释。

## 语法

```sql
CREATE DATABASE name;
CREATE TABLE table_name (
  id INTEGER PRIMARY KEY,
  name VARCHAR(100) NOT NULL,
  age INTEGER,
  score DOUBLE
);
CREATE TABLE t (id INTEGER, PRIMARY KEY (id));
DROP TABLE table_name;
CREATE INDEX index_name ON table_name (column_name);

INSERT INTO table_name VALUES (1, 'Alice', 20, 95.5), (2, 'Bob', NULL, 88.0);
INSERT INTO table_name (id, name) VALUES (3, 'Carol');
UPDATE table_name SET age = age + 1, score = 99.0 WHERE id = 1;
DELETE FROM table_name WHERE id = 2;

SELECT u.name AS person, u.age
FROM table_name AS u
WHERE (u.age >= 18 AND u.score > 80) OR u.age IS NULL
ORDER BY u.score DESC, u.id ASC
LIMIT 10 OFFSET 2;

SELECT u.name, COUNT(*), SUM(o.amount), AVG(o.amount), MIN(o.amount), MAX(o.amount)
FROM users u INNER JOIN orders o ON u.id = o.user_id
GROUP BY u.name;

BEGIN; -- BEGIN TRANSACTION / START TRANSACTION 也可
COMMIT;
ROLLBACK;
EXPLAIN SELECT * FROM table_name WHERE id = 1;
CHECKPOINT;
```

解析器每次接受一条语句和一个可选的尾部分号；多语句输入由 CLI 拆分。错误包含输入字节位置。解析器不负责目录绑定、表存在性、列存在性或聚合分组约束；这些由执行层验证。

## 值与表达式

- `INTEGER`/`INT`/`BIGINT`：有符号 64 位整数。
- `DOUBLE`/`REAL`：有限双精度浮点数。
- `VARCHAR(n)`：字符串，长度上限由存储绑定层检查；长度以 UTF-8 字节计算。`TEXT` 表示无声明上限，仍受单行页面容量限制。
- `NULL` 是独立值；表达式还可产生 `TRUE`/`FALSE`。
- 单列主键自动具有非空约束；不支持复合主键。
- 算术 `+ - * / %`、连接 `||`、比较 `= != <> < <= > >=`、`AND OR NOT`、`IS NULL`/`IS NOT NULL`、`LIKE`。
- 优先级从低到高：`OR`，`AND`，`NOT`，比较/`IS NULL`，`+ - ||`，`* / %`，一元 `+ -`，括号/字面量/列引用/函数。
- 字符串比较采用字节序且区分大小写；没有 locale collation。
- 整数与浮点数可比较，比较代码避免把大整数先转为 double 而丢失精度；字符串与数字直接比较报错。
- 整数除法向零截断，混合或浮点除法返回浮点数。余数运算要求整数。整数溢出、浮点溢出、除零均报错。
- 比较和算术遇到 NULL 返回 NULL；WHERE 只保留真值。`FALSE AND NULL = FALSE`，`TRUE OR NULL = TRUE`；其他未确定结果保留 NULL。AND/OR 及 COALESCE 有短路求值。
- ORDER BY 的比较函数默认 NULL 小于非 NULL，因此升序 NULL 在前、降序 NULL 在后。
- `LIKE` 的 `%` 匹配零个或多个字节，`_` 匹配一个字节；没有 `ESCAPE` 子句。
- 标量函数：`ABS(x)`、`LOWER(s)`、`UPPER(s)`、`LENGTH(s)`、`COALESCE(...)`。LOWER/UPPER 仅提供基础字节字符转换，不提供 Unicode 大小写折叠。
- 聚合：`COUNT(*)`、`COUNT(expr)`、`SUM`、`AVG`、`MIN`、`MAX`；实际分组和聚合由查询执行层完成。

## 明确未实现

`ALTER TABLE`、`DROP INDEX`、`TRUNCATE`、外键、UNIQUE（主键除外）、DEFAULT、CHECK、复合索引、子查询、CTE、窗口函数、DISTINCT、HAVING、UNION、OUTER/CROSS JOIN、IN、BETWEEN、日期时间类型、DECIMAL、参数占位符和完整 SQL 标准兼容。SQL 语法有意限制到一致的子集；不支持的语法明确报错。

## 单元验证

`tests/sql_test.cpp` 包含词法转义、注释、完整 DDL/DML/SELECT AST、优先级、JOIN/GROUP/ORDER/LIMIT、非法语法、NULL 三值逻辑、短路、数值边界、溢出、类型不匹配与错误路径的断言。实际执行结果由项目测试报告记录。
