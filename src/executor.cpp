#include "minidb/executor.hpp"
#include <algorithm>
namespace minidb {
namespace {
class Rows final : public Executor {
    std::vector<Row> rows_;
    size_t pos_ = 0;

  public:
    explicit Rows(std::vector<Row> rows) : rows_(std::move(rows)) {}
    void Init() override {
        pos_ = 0;
    }
    bool Next(Row &row) override {
        if (pos_ == rows_.size())
            return false;
        row = rows_[pos_++];
        return true;
    }
    void Close() noexcept override {
        pos_ = rows_.size();
    }
};
class Unary : public Executor {
  protected:
    ExecutorPtr input_;

  public:
    explicit Unary(ExecutorPtr p) : input_(std::move(p)) {}
    void Init() override {
        input_->Init();
    }
    void Close() noexcept override {
        input_->Close();
    }
};
class Filter final : public Unary {
    std::function<bool(const Row &)> fn_;

  public:
    Filter(ExecutorPtr p, decltype(fn_) fn) : Unary(std::move(p)), fn_(std::move(fn)) {}
    bool Next(Row &row) override {
        while (input_->Next(row))
            if (fn_(row))
                return true;
        return false;
    }
};
class Projection final : public Unary {
    std::function<Row(const Row &)> fn_;

  public:
    Projection(ExecutorPtr p, decltype(fn_) fn) : Unary(std::move(p)), fn_(std::move(fn)) {}
    bool Next(Row &row) override {
        Row raw;
        if (!input_->Next(raw))
            return false;
        row = fn_(raw);
        return true;
    }
};
class Limit final : public Unary {
    size_t offset_, seen_ = 0, count_ = 0;
    std::optional<size_t> limit_;

  public:
    Limit(ExecutorPtr p, size_t off, std::optional<size_t> n)
        : Unary(std::move(p)), offset_(off), limit_(n) {}
    void Init() override {
        Unary::Init();
        seen_ = count_ = 0;
    }
    bool Next(Row &row) override {
        while (seen_ < offset_) {
            if (!input_->Next(row))
                return false;
            ++seen_;
        }
        if (limit_ && count_ >= *limit_)
            return false;
        if (!input_->Next(row))
            return false;
        ++count_;
        return true;
    }
};
class Sort final : public Unary {
    std::function<bool(const Row &, const Row &)> less_;
    std::vector<Row> rows_;
    size_t pos_ = 0;

  public:
    Sort(ExecutorPtr p, decltype(less_) fn) : Unary(std::move(p)), less_(std::move(fn)) {}
    void Init() override {
        Unary::Init();
        rows_.clear();
        pos_ = 0;
        Row r;
        while (input_->Next(r))
            rows_.push_back(r);
        std::stable_sort(rows_.begin(), rows_.end(), less_);
    }
    bool Next(Row &r) override {
        if (pos_ == rows_.size())
            return false;
        r = rows_[pos_++];
        return true;
    }
};
class JoinExecutor final : public Unary {
    std::vector<Row> right_;
    std::function<bool(const Row &)> fn_;
    Row left_;
    size_t pos_ = 0;
    bool has_ = false;

  public:
    JoinExecutor(ExecutorPtr p, std::vector<Row> right, decltype(fn_) fn)
        : Unary(std::move(p)), right_(std::move(right)), fn_(std::move(fn)) {}
    void Init() override {
        Unary::Init();
        has_ = false;
        pos_ = 0;
    }
    bool Next(Row &row) override {
        for (;;) {
            if (!has_) {
                if (!input_->Next(left_))
                    return false;
                has_ = true;
                pos_ = 0;
            }
            while (pos_ < right_.size()) {
                row = left_;
                const auto &rr = right_[pos_++];
                row.insert(row.end(), rr.begin(), rr.end());
                if (fn_(row))
                    return true;
            }
            has_ = false;
        }
    }
};
} // namespace
ExecutorPtr rows_executor(std::vector<Row> r) {
    return std::make_unique<Rows>(std::move(r));
}
ExecutorPtr filter_executor(ExecutorPtr p, std::function<bool(const Row &)> f) {
    return std::make_unique<Filter>(std::move(p), std::move(f));
}
ExecutorPtr projection_executor(ExecutorPtr p, std::function<Row(const Row &)> f) {
    return std::make_unique<Projection>(std::move(p), std::move(f));
}
ExecutorPtr limit_executor(ExecutorPtr p, size_t o, std::optional<size_t> n) {
    return std::make_unique<Limit>(std::move(p), o, n);
}
ExecutorPtr sort_executor(ExecutorPtr p, std::function<bool(const Row &, const Row &)> f) {
    return std::make_unique<Sort>(std::move(p), std::move(f));
}
ExecutorPtr join_executor(ExecutorPtr p, std::vector<Row> r, std::function<bool(const Row &)> f) {
    return std::make_unique<JoinExecutor>(std::move(p), std::move(r), std::move(f));
}
std::vector<Row> collect(ExecutorPtr e) {
    try {
        e->Init();
        std::vector<Row> out;
        Row r;
        while (e->Next(r))
            out.push_back(r);
        e->Close();
        return out;
    } catch (...) {
        e->Close();
        throw;
    }
}
} // namespace minidb
