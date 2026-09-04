#include "stdout_json_sink.h"

#include <string>

namespace st {

StdoutJsonSink::StdoutJsonSink(size_t max_queued_batches, std::FILE* out)
    : max_queued_(max_queued_batches == 0 ? 1 : max_queued_batches)
    , out_(out) {
    worker_ = std::thread(&StdoutJsonSink::run, this);
}

StdoutJsonSink::~StdoutJsonSink() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void StdoutJsonSink::write(std::vector<Record> batch) {
    if (batch.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Drop the oldest, not the newest: during an overload the recent
        // records are the ones an operator is looking at.
        while (queue_.size() >= max_queued_) {
            queue_.pop_front();
            ++written_batches_; // a dropped batch must not stall flush()
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.push_back(std::move(batch));
        ++queued_batches_;
    }
    cv_.notify_one();
}

void StdoutJsonSink::flush() {
    std::unique_lock<std::mutex> lock(mu_);
    const uint64_t target = queued_batches_;
    drained_cv_.wait(lock, [&] { return written_batches_ >= target || stopping_; });
    std::fflush(out_);
}

void StdoutJsonSink::run() {
    for (;;) {
        std::vector<Record> batch;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) {
                if (stopping_)
                    break;
                continue;
            }
            batch = std::move(queue_.front());
            queue_.pop_front();
        }

        // Serialize outside the lock; only the write itself needs ordering,
        // and this thread is the sole writer.
        std::string out;
        for (const Record& r : batch) {
            out += to_json(r);
            out += '\n';
        }
        std::fwrite(out.data(), 1, out.size(), out_);
        std::fflush(out_);

        {
            std::lock_guard<std::mutex> lock(mu_);
            ++written_batches_;
        }
        drained_cv_.notify_all();
    }

    drained_cv_.notify_all();
}

} // namespace st
