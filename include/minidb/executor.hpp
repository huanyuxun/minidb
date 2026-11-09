#pragma once
#include "minidb/sql.hpp"
#include <functional>
namespace minidb {
class Executor {
  public:
    virtual ~Executor() = default;
    virtual void Init() = 0;
    virtual bool Next(Row &row) = 0;
    virtual void Close() noexcept = 0;
};
using ExecutorPtr = std::unique_ptr<Executor>;
ExecutorPtr rows_executor(std::vector<Row> rows);
ExecutorPtr filter_executor(ExecutorPtr input, std::function<bool(const Row &)> predicate);
ExecutorPtr projection_executor(ExecutorPtr input, std::function<Row(const Row &)> project);
ExecutorPtr limit_executor(ExecutorPtr input, size_t offset, std::optional<size_t> limit);
ExecutorPtr sort_executor(ExecutorPtr input, std::function<bool(const Row &, const Row &)> less);
ExecutorPtr join_executor(ExecutorPtr left, std::vector<Row> right,
                          std::function<bool(const Row &)> predicate);
std::vector<Row> collect(ExecutorPtr executor);
} // namespace minidb
