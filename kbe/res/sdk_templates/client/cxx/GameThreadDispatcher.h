//
// Created by KBEngineLab on 2025/12/14.
//
#pragma once

#include <queue>
#include <mutex>
#include <functional>
#include <utility>

class GameThreadDispatcher {
public:
    static GameThreadDispatcher& Instance()
    {
        static GameThreadDispatcher Inst;
        return Inst;
    }

    void Post(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(fn));
    }

    void Pump()
    {
        std::queue<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(local, queue_);
        }

        while (!local.empty())
        {
            local.front()();
            local.pop();
        }
    }

private:
    std::mutex mutex_;
    std::queue<std::function<void()>> queue_;
};
