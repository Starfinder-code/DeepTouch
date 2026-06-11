#include "MockCamera.h"

#include <chrono>
#include <cmath>
#include <thread>

#include "Log.h"

bool MockCamera::open()
{
    logLine("[MockCamera] open success.");
    opened_ = true;
    return true;
}

bool MockCamera::configure(const AcquisitionConfig& config)
{
    if (!opened_)
    {
        logLine("[MockCamera] configure failed: camera is not opened.");
        return false;
    }

    config_ = config;

    logLine("[MockCamera] configure success.");
    logLine("  ROI: ", config_.roi.x, ", ", config_.roi.y, ", ",
        config_.roi.width, " x ", config_.roi.height);
    logLine("  Acquisition FPS: ", config_.fps);
    logLine("  Preview FPS: ", config_.previewFps);
    logLine("  Exposure: ", config_.exposureMs, " ms");
    logLine("  Gain: ", config_.gain);

    return true;
}

bool MockCamera::start()
{
    if (!opened_)
    {
        logLine("[MockCamera] start failed: camera is not opened.");
        return false;
    }

    running_ = true;
    frameId_ = 0;

    logLine("[MockCamera] acquisition started.");
    return true;
}

bool MockCamera::getFrame(Frame& frame)
{
    if (!running_)
    {
        return false;
    }

    const int width = config_.roi.width;
    const int height = config_.roi.height;

    frame.frameId = frameId_++;
    frame.width = width;
    frame.height = height;
    frame.bitDepth = 16;
    frame.timestamp = std::chrono::steady_clock::now();
    frame.pixels.resize(static_cast<size_t>(width) * height);

    const int sensorWidth = 1274;
    const int sensorHeight = 1024;

    const int roiX = config_.roi.x;
    const int roiY = config_.roi.y;

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            int globalX = roiX + x;
            int globalY = roiY + y;

            if (globalX < 0)
            {
                globalX = 0;
            }
            else if (globalX >= sensorWidth)
            {
                globalX = sensorWidth - 1;
            }

            if (globalY < 0)
            {
                globalY = 0;
            }
            else if (globalY >= sensorHeight)
            {
                globalY = sensorHeight - 1;
            }

            double gradient =
                static_cast<double>(globalX) / static_cast<double>(sensorWidth - 1);

            double wave =
                0.5 + 0.5 * std::sin((globalX + frame.frameId * 5) * 0.03);

            double value =
                (gradient * 0.7 + wave * 0.3) * 65535.0;

            if (value < 0.0)
            {
                value = 0.0;
            }
            else if (value > 65535.0)
            {
                value = 65535.0;
            }

            frame.pixels[static_cast<size_t>(y) * width + x] =
                static_cast<uint16_t>(value);
        }
    }

    int sleepMs = static_cast<int>(1000.0 / config_.fps);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

    return true;
}

void MockCamera::stop()
{
    running_ = false;
    logLine("[MockCamera] acquisition stopped.");
}

void MockCamera::close()
{
    opened_ = false;
    logLine("[MockCamera] closed.");
}