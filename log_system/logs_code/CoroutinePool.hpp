#pragma once
#include <coroutine>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <stdexcept>

class CoroutinePool
{
public:
    // 协程任务承诺类型
    struct TaskPromise
    {
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() { std::terminate(); }
        void return_void() {}

        std::coroutine_handle<> continuation; // 后续协程
    };

    // 协程任务类型
    struct Task
    {
        using promise_type = TaskPromise;

        std::coroutine_handle<promise_type> handle;

        explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}

        ~Task()
        {
            if (handle)
                handle.destroy();
        }
    };

    explicit CoroutinePool(size_t threads) : stop(false)
    {
        for (size_t i = 0; i < threads; ++i)
        {
            workers.emplace_back([this]
                                 { worker_loop(); });
        }
    }

    ~CoroutinePool()
    {
        {
            std::unique_lock lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (auto &worker : workers)
        {
            worker.join();
        }
    }

    // 协程任务提交接口
    template <typename F>
    auto enqueue(F &&f)
    {
        struct Awaitable
        {
            CoroutinePool &pool;
            F &&func;

            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> h)
            {
                {
                    std::unique_lock lock(pool.queue_mutex);
                    pool.tasks.emplace([this, h]
                                       {
                        func();
                        if (h) h.resume(); });
                }
                pool.condition.notify_one();
            }

            void await_resume() {}
        };

        return Awaitable{*this, std::forward<F>(f)};
    }

private:
    void worker_loop()
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock lock(queue_mutex);
                condition.wait(lock, [this]
                               { return stop || !tasks.empty(); });
                if (stop && tasks.empty())
                    return;
                task = std::move(tasks.front());
                tasks.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};