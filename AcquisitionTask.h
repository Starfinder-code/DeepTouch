#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>

#include "CoreTypes.h"
#include "FrameQueue.h"
#include "ICamera.h"
#include "LatestPreviewBuffer.h"
#include "ImageUtils.h"

enum class CameraBackend
{
    Mock,
    Merak
};

enum class TaskState
{
    Idle,
    Running,
    Stopping,
    Completed,
    Error
};

class AcquisitionTask
{
public:
    explicit AcquisitionTask(CameraBackend backend);
    ~AcquisitionTask();

    bool start(const AcquisitionConfig& config, int targetFrameCount);
    void stopAndWait();

    bool isRunning();

    int getSavedCount() const;
    int getPreviewSavedCount() const;
    int getPeakPreviewCount() const;
    double getActualCaptureFps() const;

    bool getLatestPreviewFrame(PreviewFrame8& outFrame);
    bool getLatestInstantPreviewFrame(PreviewFrame8& outFrame);
    bool getLatestPeakPreviewFrame(PreviewFrame8& outFrame);

    std::string getStateName();
    std::string getSaveDir();

private:
    CameraBackend backend_;
    AcquisitionConfig config_;

    int targetFrameCount_ = 0;

    std::unique_ptr<ICamera> camera_;
    std::unique_ptr<SharedFrameQueue> saveQueue_;
    std::unique_ptr<SharedFrameQueue> previewQueue_;

    LatestPreviewBuffer instantPreviewBuffer_;
    LatestPreviewBuffer peakPreviewBuffer_;

    std::thread captureThread_;
    std::thread saveThread_;
    std::thread previewThread_;

    std::atomic<bool> running_ = false;

    std::atomic<int> savedCount_ = 0;
    std::atomic<int> previewSavedCount_ = 0;
    std::atomic<int> peakPreviewCount_ = 0;
    std::atomic<double> actualCaptureFps_ = 0.0;

    std::chrono::steady_clock::time_point taskStartTime_;

    mutable std::mutex stateMutex_;
    TaskState state_ = TaskState::Idle;
};
