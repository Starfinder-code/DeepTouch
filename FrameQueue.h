#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

#include "CoreTypes.h"

enum class QueueFullPolicy
{
    Block,
    DropOldest
};

class SharedFrameQueue
{
public:
    SharedFrameQueue(size_t maxSize, QueueFullPolicy policy)
        : maxSize_(maxSize), policy_(policy)
    {
    }

    bool push(FramePtr frame)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (policy_ == QueueFullPolicy::Block)
        {
            notFull_.wait(lock, [this]() {
                return stopped_ || queue_.size() < maxSize_;
                });

            if (stopped_)
            {
                return false;
            }
        }
        else
        {
            while (!queue_.empty() && queue_.size() >= maxSize_)
            {
                queue_.pop();
            }

            if (stopped_)
            {
                return false;
            }
        }

        queue_.push(frame);
        notEmpty_.notify_one();
        return true;
    }

    bool pop(FramePtr& frame)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        notEmpty_.wait(lock, [this]() {
            return stopped_ || !queue_.empty();
            });

        if (queue_.empty())
        {
            return false;
        }

        frame = queue_.front();
        queue_.pop();

        notFull_.notify_one();
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;

        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    size_t size()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<FramePtr> queue_;
    size_t maxSize_ = 0;
    QueueFullPolicy policy_;

    std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;

    bool stopped_ = false;
};