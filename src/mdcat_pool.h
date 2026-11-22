// Image worker thread pool (header only).
#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace mdcat {

// ---------------------------------------------------------------------------
// Image worker thread pool (ADR 0003)
// ---------------------------------------------------------------------------
//
// A fixed pool of N = hardware-concurrency workers that run image-conversion tasks (timg / mmdc
// subprocesses) off the render thread. Each task is a std::function returning void and captures its
// own inputs/outputs, so the pool stays agnostic about ImageJob shape; callers submit() a packaged
// task and wait on the returned future. For now (step 2) call sites submit and immediately .get(),
// so behaviour and output are unchanged — later steps overlap the work.
class ThreadPool {
public:
    explicit ThreadPool(unsigned n) {
        for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { workerLoop(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    // Enqueue `fn` and return a future for its result. The task runs on some worker thread.
    // R is deduced with decltype rather than std::invoke_result/result_of: result_of was removed
    // in C++20, and some libc++ builds gate invoke_result behind -std=c++20, so decltype is the
    // portable spelling that works across the toolchains this project is built with.
    template <class F>
    auto submit(F&& fn) -> std::future<decltype(fn())> {
        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop();
            }
            job();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
};

// The process-wide image pool, created on first use (defined in mdcat.cpp for now).
ThreadPool& imagePool();

}  // namespace mdcat
