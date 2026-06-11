#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ===============================
// 采集参数结构体
// ===============================

enum class BinningMode
{
    Bin1x1,
    Bin2x2,
    Bin3x3,
    Bin4x4
};

enum class TriggerMode
{
    Continuous,
    RisingEdge,
    FallingEdge,
    LowLevel,
    HighLevel,
    Snapshot
};

enum class SpeedControlMode
{
    FPS,
    ExposureTime
};

enum class PreviewRenderMode
{
    Instant,
    PeakHold
};


struct Roi
{
    int x = 0;
    int y = 0;
    int width = 1024;
    int height = 768;
};

struct AcquisitionConfig
{
    BinningMode binning = BinningMode::Bin1x1;
    TriggerMode trigger = TriggerMode::Continuous;
    SpeedControlMode speedControlMode = SpeedControlMode::FPS;
    PreviewRenderMode previewRenderMode = PreviewRenderMode::Instant;


    double exposureMs = 10.0;
    double gain = 1.0;
    double fps = 30.0;
    double previewFps = 15.0;

    Roi roi;

    std::string saveDir = "acq_output/test001";

    // Step10 以后默认走 WebSocket + Canvas 预览，不依赖 BMP 文件
    bool savePreviewBmpDebug = false;
    bool saveLatestBmpDebug = false;
    bool generateBmpViewerHtml = false;
};

// ===============================
// 原始图像帧结构体
// ===============================

struct Frame
{
    uint64_t frameId = 0;
    int width = 0;
    int height = 0;
    int bitDepth = 16;

    std::chrono::steady_clock::time_point timestamp;

    std::vector<uint16_t> pixels;
};

using FramePtr = std::shared_ptr<Frame>;

// ===============================
// 8 位预览帧结构体
// ===============================

struct PreviewFrame8
{
    uint64_t frameId = 0;
    int width = 0;
    int height = 0;

    // 相对于本次采集任务开始时间的微秒时间戳
    int64_t timestampUs = 0;

    // 8 位灰度数据，大小 = width * height
    std::vector<uint8_t> gray8;
};

using PreviewFrame8Ptr = std::shared_ptr<PreviewFrame8>;