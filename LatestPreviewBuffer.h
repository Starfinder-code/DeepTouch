#pragma once

#include <mutex>

#include "CoreTypes.h"

class LatestPreviewBuffer
{
public:
    void update(PreviewFrame8Ptr frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latestFrame_ = frame;
    }

    bool getLatest(PreviewFrame8& outFrame)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!latestFrame_)
        {
            return false;
        }

        outFrame = *latestFrame_;
        return true;
    }

    bool hasFrame()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return latestFrame_ != nullptr;
    }

private:
    std::mutex mutex_;
    PreviewFrame8Ptr latestFrame_;
};