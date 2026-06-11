#include "AcquisitionTask.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>

#include "ImageUtils.h"
#include "Log.h"
#include "MockCamera.h"
#include "MerakCamera.h"

namespace fs = std::filesystem;

// ===============================
// Camera factory
// ===============================

std::unique_ptr<ICamera> createCamera(CameraBackend backend)
{
    if (backend == CameraBackend::Mock)
    {
        return std::make_unique<MockCamera>();
    }

    if (backend == CameraBackend::Merak)
    {
        return std::make_unique<MerakCamera>();
    }

    return std::make_unique<MockCamera>();
}

// ===============================
// Debug BMP filename helper
// ===============================

std::string makePreviewBmpFileName(uint64_t frameId)
{
    std::ostringstream oss;

    oss << "preview_"
        << std::setw(6)
        << std::setfill('0')
        << frameId
        << ".bmp";

    return oss.str();
}

std::string makeTiffFileName(uint64_t frameId)
{
    std::ostringstream oss;

    oss << "frame_"
        << std::setw(6)
        << std::setfill('0')
        << frameId
        << ".tif";

    return oss.str();
}

std::string makeHeatmapTiffFileName(uint64_t frameId)
{
    std::ostringstream oss;

    oss << "heatmap_"
        << std::setw(6)
        << std::setfill('0')
        << frameId
        << ".tif";

    return oss.str();
}

std::vector<uint8_t> convertPeakTo8FixedLinear(
    const std::vector<uint16_t>& peakPixels,
    int bitDepth)
{
    std::vector<uint8_t> gray8;
    gray8.resize(peakPixels.size());

    if (peakPixels.empty())
    {
        return gray8;
    }

    if (bitDepth <= 0)
    {
        bitDepth = 16;
    }

    for (size_t i = 0; i < peakPixels.size(); ++i)
    {
        uint16_t v = peakPixels[i];

        int out = 0;

        if (bitDepth >= 16)
        {
            // 16 bit: 0~65535 -> 0~255
            out = static_cast<int>(v >> 8);
        }
        else if (bitDepth == 14)
        {
            // 14 bit: 0~16383 -> 0~255
            out = static_cast<int>(v >> 6);
        }
        else if (bitDepth == 12)
        {
            // 12 bit: 0~4095 -> 0~255
            out = static_cast<int>(v >> 4);
        }
        else if (bitDepth == 8)
        {
            // 当前项目里 Mono8 已经被扩展到 uint16: value << 8
            // 所以这里再右移 8 位还原显示灰度
            out = static_cast<int>(v >> 8);
        }
        else
        {
            // 兜底：按真实 bitDepth 做固定线性映射
            const int maxValue = (1 << bitDepth) - 1;
            out = static_cast<int>(
                static_cast<double>(v) * 255.0 / static_cast<double>(maxValue)
                );
        }

        if (out < 0)
        {
            out = 0;
        }
        else if (out > 255)
        {
            out = 255;
        }

        gray8[i] = static_cast<uint8_t>(out);
    }

    return gray8;
}
// ===============================
// Legacy BMP viewer helper
// ===============================

bool writeLivePreviewHtml(const fs::path& htmlPath, int refreshMs)
{
    std::ofstream html(htmlPath);

    if (!html.is_open())
    {
        logLine("[PreviewWorker] failed to create viewer html: ", htmlPath.string());
        return false;
    }

    html << R"(<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <title>MLAcqCore Live Preview</title>
</head>
<body style="background:#111;color:#eee;font-family:Arial;">
    <h2>MLAcqCore Live Preview</h2>
    <p>This page refreshes latest.bmp. WebSocket preview is preferred.</p>
    <img id="preview" src="latest.bmp" style="max-width:95vw;max-height:80vh;border:1px solid #555;">
    <div id="status"></div>

    <script>
        const refreshMs = )" << refreshMs << R"(;
        const img = document.getElementById("preview");
        const status = document.getElementById("status");
        let count = 0;

        function refreshImage() {
            const next = new Image();

            next.onload = function () {
                count += 1;
                img.src = next.src;
                status.textContent = "refresh count = " + count;
            };

            next.src = "latest.bmp?t=" + Date.now();
        }

        setInterval(refreshImage, refreshMs);
    </script>
</body>
</html>
)";

    return true;
}

// ===============================
// Capture thread
// ===============================


void writeU16LE(std::ofstream& os, uint16_t v)
{
    char b[2];
    b[0] = static_cast<char>(v & 0xFF);
    b[1] = static_cast<char>((v >> 8) & 0xFF);
    os.write(b, 2);
}

void writeU32LE(std::ofstream& os, uint32_t v)
{
    char b[4];
    b[0] = static_cast<char>(v & 0xFF);
    b[1] = static_cast<char>((v >> 8) & 0xFF);
    b[2] = static_cast<char>((v >> 16) & 0xFF);
    b[3] = static_cast<char>((v >> 24) & 0xFF);
    os.write(b, 4);
}

void writeTiffEntry(
    std::ofstream& os,
    uint16_t tag,
    uint16_t type,
    uint32_t count,
    uint32_t value)
{
    writeU16LE(os, tag);
    writeU16LE(os, type);
    writeU32LE(os, count);
    writeU32LE(os, value);
}

bool saveGray16Tiff(
    const fs::path& filePath,
    int width,
    int height,
    const std::vector<uint16_t>& pixels)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    const size_t expectedPixels =
        static_cast<size_t>(width) * static_cast<size_t>(height);

    if (pixels.size() != expectedPixels)
    {
        return false;
    }

    std::ofstream os(filePath, std::ios::binary);

    if (!os.is_open())
    {
        return false;
    }

    const uint16_t entryCount = 11;

    const uint32_t ifdOffset = 8;
    const uint32_t dataOffset =
        ifdOffset + 2 + static_cast<uint32_t>(entryCount) * 12 + 4;

    const uint32_t imageByteCount =
        static_cast<uint32_t>(pixels.size() * sizeof(uint16_t));

    // TIFF header: little endian
    os.put('I');
    os.put('I');
    writeU16LE(os, 42);
    writeU32LE(os, ifdOffset);

    // IFD
    writeU16LE(os, entryCount);

    // Tags must be sorted by tag ID
    writeTiffEntry(os, 256, 4, 1, static_cast<uint32_t>(width));          // ImageWidth
    writeTiffEntry(os, 257, 4, 1, static_cast<uint32_t>(height));         // ImageLength
    writeTiffEntry(os, 258, 3, 1, 16);                                   // BitsPerSample = 16
    writeTiffEntry(os, 259, 3, 1, 1);                                    // Compression = none
    writeTiffEntry(os, 262, 3, 1, 1);                                    // PhotometricInterpretation = BlackIsZero
    writeTiffEntry(os, 273, 4, 1, dataOffset);                           // StripOffsets
    writeTiffEntry(os, 277, 3, 1, 1);                                    // SamplesPerPixel = 1
    writeTiffEntry(os, 278, 4, 1, static_cast<uint32_t>(height));         // RowsPerStrip
    writeTiffEntry(os, 279, 4, 1, imageByteCount);                       // StripByteCounts
    writeTiffEntry(os, 284, 3, 1, 1);                                    // PlanarConfiguration = chunky
    writeTiffEntry(os, 339, 3, 1, 1);                                    // SampleFormat = unsigned integer

    // next IFD offset = 0
    writeU32LE(os, 0);

    // image data
    os.write(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(imageByteCount)
    );

    return os.good();
}

bool saveRgb8Tiff(
    const fs::path& filePath,
    int width,
    int height,
    const std::vector<uint8_t>& rgb)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    const size_t expectedBytes =
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        3;

    if (rgb.size() != expectedBytes)
    {
        return false;
    }

    std::ofstream os(filePath, std::ios::binary);

    if (!os.is_open())
    {
        return false;
    }

    const uint16_t entryCount = 10;

    const uint32_t ifdOffset = 8;

    const uint32_t bitsPerSampleOffset =
        ifdOffset + 2 + static_cast<uint32_t>(entryCount) * 12 + 4;

    const uint32_t dataOffset = bitsPerSampleOffset + 6;

    const uint32_t imageByteCount =
        static_cast<uint32_t>(rgb.size());

    // TIFF header: little endian
    os.put('I');
    os.put('I');
    writeU16LE(os, 42);
    writeU32LE(os, ifdOffset);

    // IFD
    writeU16LE(os, entryCount);

    // Tags sorted by tag ID
    writeTiffEntry(os, 256, 4, 1, static_cast<uint32_t>(width));      // ImageWidth
    writeTiffEntry(os, 257, 4, 1, static_cast<uint32_t>(height));     // ImageLength
    writeTiffEntry(os, 258, 3, 3, bitsPerSampleOffset);              // BitsPerSample offset
    writeTiffEntry(os, 259, 3, 1, 1);                                // Compression = none
    writeTiffEntry(os, 262, 3, 1, 2);                                // PhotometricInterpretation = RGB
    writeTiffEntry(os, 273, 4, 1, dataOffset);                       // StripOffsets
    writeTiffEntry(os, 277, 3, 1, 3);                                // SamplesPerPixel = 3
    writeTiffEntry(os, 278, 4, 1, static_cast<uint32_t>(height));     // RowsPerStrip
    writeTiffEntry(os, 279, 4, 1, imageByteCount);                   // StripByteCounts
    writeTiffEntry(os, 284, 3, 1, 1);                                // PlanarConfiguration = chunky

    // next IFD offset = 0
    writeU32LE(os, 0);

    // BitsPerSample values: 8, 8, 8
    writeU16LE(os, 8);
    writeU16LE(os, 8);
    writeU16LE(os, 8);

    // RGB image data
    os.write(
        reinterpret_cast<const char*>(rgb.data()),
        static_cast<std::streamsize>(imageByteCount)
    );

    return os.good();
}

struct Rgb8
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

struct HeatmapLut
{
    std::vector<uint8_t> rgbTable;
    int bitDepth = 16;
};

Rgb8 heatColorFromNormalized(double t)
{
    if (t < 0.0)
    {
        t = 0.0;
    }

    if (t > 1.0)
    {
        t = 1.0;
    }

    struct Point
    {
        double t;
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    static const Point points[] =
    {
        {0.00,   0,   8,  60},
        {0.20,   0,  50, 180},
        {0.40,   0, 190, 255},
        {0.55,   0, 230, 120},
        {0.70, 255, 240,   0},
        {0.85, 255, 100,   0},
        {1.00, 255,   0,   0}
    };

    const int pointCount =
        static_cast<int>(sizeof(points) / sizeof(points[0]));

    for (int i = 0; i < pointCount - 1; ++i)
    {
        const Point& p0 = points[i];
        const Point& p1 = points[i + 1];

        if (t >= p0.t && t <= p1.t)
        {
            double u = (t - p0.t) / (p1.t - p0.t);

            Rgb8 c;

            c.r = static_cast<uint8_t>(p0.r + u * static_cast<double>(p1.r - p0.r));
            c.g = static_cast<uint8_t>(p0.g + u * static_cast<double>(p1.g - p0.g));
            c.b = static_cast<uint8_t>(p0.b + u * static_cast<double>(p1.b - p0.b));

            return c;
        }
    }

    return { 255, 0, 0 };
}

HeatmapLut buildHeatmapLut(int bitDepth)
{
    HeatmapLut lut;
    lut.bitDepth = bitDepth;

    int maxValue = 65535;

    if (bitDepth == 14)
    {
        maxValue = 16383;
    }
    else if (bitDepth == 12)
    {
        maxValue = 4095;
    }
    else if (bitDepth == 8)
    {
        maxValue = 255;
    }

    lut.rgbTable.resize(static_cast<size_t>(maxValue + 1) * 3);

    for (int v = 0; v <= maxValue; ++v)
    {
        double t = static_cast<double>(v) / static_cast<double>(maxValue);
        Rgb8 c = heatColorFromNormalized(t);

        size_t j = static_cast<size_t>(v) * 3;

        lut.rgbTable[j] = c.r;
        lut.rgbTable[j + 1] = c.g;
        lut.rgbTable[j + 2] = c.b;
    }

    return lut;
}

std::vector<uint8_t> convertU16ToHeatRgbWithLut(
    const std::vector<uint16_t>& pixels,
    const HeatmapLut& lut)
{
    std::vector<uint8_t> rgb;
    rgb.resize(pixels.size() * 3);

    int maxValue = 65535;

    if (lut.bitDepth == 14)
    {
        maxValue = 16383;
    }
    else if (lut.bitDepth == 12)
    {
        maxValue = 4095;
    }
    else if (lut.bitDepth == 8)
    {
        maxValue = 255;
    }

    for (size_t i = 0; i < pixels.size(); ++i)
    {
        int v = static_cast<int>(pixels[i]);

        if (v < 0)
        {
            v = 0;
        }
        else if (v > maxValue)
        {
            v = maxValue;
        }

        size_t lutIndex = static_cast<size_t>(v) * 3;
        size_t outIndex = i * 3;

        rgb[outIndex] = lut.rgbTable[lutIndex];
        rgb[outIndex + 1] = lut.rgbTable[lutIndex + 1];
        rgb[outIndex + 2] = lut.rgbTable[lutIndex + 2];
    }

    return rgb;
}

std::string heatmapColorMapToString()
{
    return "COLORMAP_DEEPTOUCH";
}

void captureLoop(
    ICamera& camera,
    SharedFrameQueue& saveQueue,
    SharedFrameQueue& previewQueue,
    std::atomic<bool>& running,
    int targetFrameCount,
    bool enableInstantPreview)
{
    int capturedCount = 0;

    while (running)
    {
        if (targetFrameCount > 0 && capturedCount >= targetFrameCount)
        {
            break;
        }

        auto frame = std::make_shared<Frame>();

        if (camera.getFrame(*frame))
        {
            bool pushedToSave = saveQueue.push(frame);

            if (!pushedToSave)
            {
                break;
            }

            previewQueue.push(frame);

            ++capturedCount;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    saveQueue.stop();
    previewQueue.stop();

    logLine("[CaptureThread] exit. captured count = ", capturedCount);
}

// ===============================
// Save thread
// ===============================

void saveLoop(
    SharedFrameQueue& saveQueue,
    const AcquisitionConfig& config,
    std::chrono::steady_clock::time_point taskStartTime,
    std::atomic<int>& savedCount,
    LatestPreviewBuffer& peakPreviewBuffer,
    std::atomic<int>& peakPreviewCount,
    std::atomic<double>& actualCaptureFps)
{
    const bool saveFrameHeatmapTiff = true;
    fs::path saveDir(config.saveDir);
    fs::path tiffDir = saveDir / "tiff_frames";
    fs::path heatmapDir = saveDir / "heatmap_tiff";

    try
    {
        fs::create_directories(saveDir);
        fs::create_directories(tiffDir);
        fs::create_directories(heatmapDir);
    }
    catch (const std::exception& e)
    {
        logLine("[SaveWorker] failed to create save directory: ", saveDir.string());
        logLine("[SaveWorker] exception: ", e.what());
        logLine("[SaveWorker] heatmap TIFF directory: ",
            fs::absolute(heatmapDir).string());
        saveQueue.stop();
        return;
    }

    logLine("[SaveWorker] save directory: ", fs::absolute(saveDir).string());
    logLine("[SaveWorker] tiff frames directory: ", fs::absolute(tiffDir).string());
    logLine("[SaveWorker] heatmap colormap: ",
        heatmapColorMapToString());

    fs::path heatmapInfoPath = saveDir / "heatmap_info.txt";

    std::ofstream heatmapInfo(heatmapInfoPath);

    if (heatmapInfo.is_open())
    {
        heatmapInfo << "heatmap_directory=heatmap_tiff\n";
        heatmapInfo << "heatmap_format=uncompressed 8-bit RGB TIFF\n";
        heatmapInfo << "heatmap_colormap="
            << heatmapColorMapToString()
            << "\n";
        heatmapInfo << "peak_heatmap_file=peak_hold_heatmap.tif\n";
    }

    fs::path csvPath = saveDir / "timestamps.csv";

    std::ofstream csv(csvPath);

    if (!csv.is_open())
    {
        logLine("[SaveWorker] failed to open timestamps.csv");
        saveQueue.stop();
        return;
    }

    csv << "frame_id,"
        << "timestamp_us,"
        << "width,"
        << "height,"
        << "bit_depth,"
        << "pixel_byte_size,"
        << "image_file,"
        << "heatmap_file"
        << "\n";
    
    const bool peakMode = true;

    std::vector<uint16_t> peakPixels;
    bool peakInitialized = false;

    const double previewIntervalMs =
        config.previewFps > 0 ? 1000.0 / config.previewFps : 1000.0 / 15.0;

    auto nextPeakPreviewTime =
        std::chrono::steady_clock::time_point::min();

    int peakWidth = 0;
    int peakHeight = 0;
    int peakBitDepth = 16;
    bool heatmapLutInitialized = false;
    HeatmapLut heatmapLut;

    bool fpsInitialized = false;
    std::chrono::steady_clock::time_point firstFrameTime;

    FramePtr frame;

    while (saveQueue.pop(frame))
    {
        auto timestampUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                frame->timestamp - taskStartTime
            ).count();

        if (!heatmapLutInitialized)
        {
            heatmapLut = buildHeatmapLut(
                frame->bitDepth
            );

            heatmapLutInitialized = true;

            logLine("[SaveWorker] heatmap LUT initialized. colormap = ",
                heatmapColorMapToString(),
                ", bitDepth = ",
                frame->bitDepth);
        }

        std::string tiffFileName = makeTiffFileName(frame->frameId);
        fs::path tiffPath = tiffDir / tiffFileName;

        const uint64_t pixelByteSize =
            static_cast<uint64_t>(frame->pixels.size() * sizeof(uint16_t));

        bool ok = saveGray16Tiff(
            tiffPath,
            frame->width,
            frame->height,
            frame->pixels
        );

        if (!ok)
        {
            logLine("[SaveWorker] failed to write TIFF file: ", tiffPath.string());
            continue;
        }

        std::string heatmapFileName = makeHeatmapTiffFileName(frame->frameId);
        fs::path heatmapPath = heatmapDir / heatmapFileName;

        std::vector<uint8_t> heatRgb =
            convertU16ToHeatRgbWithLut(
                frame->pixels,
                heatmapLut
            );

        bool heatOk = saveRgb8Tiff(
            heatmapPath,
            frame->width,
            frame->height,
            heatRgb
        );

        if (!heatOk)
        {
            logLine("[SaveWorker] failed to write heatmap TIFF file: ",
                heatmapPath.string());
        }

        csv << frame->frameId << ","
            << timestampUs << ","
            << frame->width << ","
            << frame->height << ","
            << frame->bitDepth << ","
            << pixelByteSize << ","
            << "tiff_frames/" << tiffFileName << ","
            << "heatmap_tiff/" << heatmapFileName
            << "\n";

        if (peakMode)
        {
            const size_t pixelCount = frame->pixels.size();

            if (!peakInitialized ||
                peakPixels.size() != pixelCount)
            {
                peakPixels.assign(pixelCount, 0);
                peakInitialized = true;

                peakWidth = frame->width;
                peakHeight = frame->height;
                peakBitDepth = frame->bitDepth;

                nextPeakPreviewTime = frame->timestamp;

                logLine("[PeakHold] peak buffer initialized: ",
                    frame->width,
                    " x ",
                    frame->height,
                    ", pixels = ",
                    pixelCount);
            }

            for (size_t i = 0; i < pixelCount; ++i)
            {
                if (frame->pixels[i] > peakPixels[i])
                {
                    peakPixels[i] = frame->pixels[i];
                }
            }

            if (frame->timestamp >= nextPeakPreviewTime)
            {
                auto previewFrame = std::make_shared<PreviewFrame8>();

                previewFrame->frameId = frame->frameId;
                previewFrame->width = frame->width;
                previewFrame->height = frame->height;
                previewFrame->timestampUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        frame->timestamp - taskStartTime
                    ).count();

                previewFrame->gray8 = convertPeakTo8FixedLinear(
                    peakPixels,
                    frame->bitDepth
                );

                peakPreviewBuffer.update(previewFrame);

                ++peakPreviewCount;

                if (savedCount.load() % 10 == 0)
                {
                    logLine("[SaveWorker] saved ",
                        savedCount.load(),
                        " 16-bit TIFF frame files. saveQueue size = ",
                        saveQueue.size());
                }

                nextPeakPreviewTime =
                    frame->timestamp +
                    std::chrono::milliseconds(
                        static_cast<int>(previewIntervalMs)
                    );
            }
        }

        int newSavedCount = ++savedCount;

        if (!fpsInitialized)
        {
            firstFrameTime = frame->timestamp;
            fpsInitialized = true;
        }

        if (fpsInitialized && newSavedCount > 1)
        {
            auto elapsedUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    frame->timestamp - firstFrameTime
                ).count();

            if (elapsedUs > 0)
            {
                double fps =
                    static_cast<double>(newSavedCount - 1) * 1000000.0 /
                    static_cast<double>(elapsedUs);

                actualCaptureFps.store(fps);
            }
        }

        if (newSavedCount % 10 == 0)
        {
            logLine("[SaveWorker] saved ",
                newSavedCount,
                " 16-bit TIFF frame files. saveQueue size = ",
                saveQueue.size(),
                ", actual FPS = ",
                actualCaptureFps.load());
        }
    }

    bool peakSaved = false;
    bool peakHeatmapSaved = false;

    if (peakInitialized && !peakPixels.empty())
    {
        fs::path peakTiffPath = saveDir / "peak_hold_16bit.tif";

        peakSaved = saveGray16Tiff(
            peakTiffPath,
            peakWidth,
            peakHeight,
            peakPixels
        );

        if (peakSaved)
        {
            logLine("[PeakHold] saved peak hold 16-bit TIFF: ",
                fs::absolute(peakTiffPath).string());
        }
        else
        {
            logLine("[PeakHold] failed to save peak hold TIFF: ",
                peakTiffPath.string());
        }

        std::vector<uint8_t> peakHeatRgb =
            convertU16ToHeatRgbWithLut(
                peakPixels,
                heatmapLut
            );

        fs::path peakHeatmapPath = saveDir / "peak_hold_heatmap.tif";

        peakHeatmapSaved = saveRgb8Tiff(
            peakHeatmapPath,
            peakWidth,
            peakHeight,
            peakHeatRgb
        );

        if (peakHeatmapSaved)
        {
            logLine("[PeakHold] saved peak hold heatmap TIFF: ",
                fs::absolute(peakHeatmapPath).string());
        }
        else
        {
            logLine("[PeakHold] failed to save peak hold heatmap TIFF: ",
                peakHeatmapPath.string());
        }
    }

    csv.close();

    logLine("[SaveWorker] exit. total saved = ", savedCount.load());
}

// ===============================
// Preview thread
// ===============================

void previewLoop(
    SharedFrameQueue& previewQueue,
    const AcquisitionConfig& config,
    std::chrono::steady_clock::time_point taskStartTime,
    LatestPreviewBuffer& latestPreviewBuffer,
    std::atomic<int>& previewSavedCount)
{
    fs::path previewDir = fs::path(config.saveDir) / "preview_bmp";
    fs::path liveDir = fs::path(config.saveDir) / "live_preview";
    fs::path latestBmpPath = liveDir / "latest.bmp";
    fs::path viewerHtmlPath = liveDir / "viewer.html";

    try
    {
        if (config.savePreviewBmpDebug)
        {
            fs::create_directories(previewDir);
            logLine("[PreviewWorker] preview directory: ", fs::absolute(previewDir).string());
        }

        if (config.saveLatestBmpDebug || config.generateBmpViewerHtml)
        {
            fs::create_directories(liveDir);
        }

        if (config.saveLatestBmpDebug)
        {
            logLine("[PreviewWorker] live preview latest.bmp: ", fs::absolute(latestBmpPath).string());
        }

        if (config.generateBmpViewerHtml)
        {
            int refreshMs = static_cast<int>(1000.0 / config.previewFps);

            if (refreshMs < 50)
            {
                refreshMs = 50;
            }

            if (writeLivePreviewHtml(viewerHtmlPath, refreshMs))
            {
                logLine("[PreviewWorker] live preview viewer.html: ",
                    fs::absolute(viewerHtmlPath).string());
            }
        }
    }
    catch (const std::exception& e)
    {
        logLine("[PreviewWorker] failed to create preview debug directory.");
        logLine("[PreviewWorker] exception: ", e.what());
        previewQueue.stop();
        return;
    }

    const double previewIntervalMs = 1000.0 / config.previewFps;
    auto nextPreviewTime = std::chrono::steady_clock::time_point::min();

    FramePtr frame;

    while (previewQueue.pop(frame))
    {
        if (nextPreviewTime == std::chrono::steady_clock::time_point::min())
        {
            nextPreviewTime = frame->timestamp;
        }

        if (frame->timestamp < nextPreviewTime)
        {
            continue;
        }

        static int debugFrameCount = 0;

        if (debugFrameCount < 20 && frame && !frame->pixels.empty())
        {
            auto minmax = std::minmax_element(
                frame->pixels.begin(),
                frame->pixels.end()
            );

            uint16_t minValue = *minmax.first;
            uint16_t maxValue = *minmax.second;

            uint64_t sum = 0;

            for (uint16_t v : frame->pixels)
            {
                sum += v;
            }

            double meanValue =
                static_cast<double>(sum) /
                static_cast<double>(frame->pixels.size());

            logLine("[FrameDebug] frameId = ",
                frame->frameId,
                ", size = ",
                frame->width,
                " x ",
                frame->height,
                ", bitDepth = ",
                frame->bitDepth,
                ", min = ",
                minValue,
                ", max = ",
                maxValue,
                ", mean = ",
                meanValue);

            ++debugFrameCount;
        }

        auto previewFrame = std::make_shared<PreviewFrame8>();

        previewFrame->frameId = frame->frameId;
        previewFrame->width = frame->width;
        previewFrame->height = frame->height;
        previewFrame->timestampUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                frame->timestamp - taskStartTime
            ).count();



        previewFrame->gray8 = convertFrameTo8FixedLinear(*frame);

        latestPreviewBuffer.update(previewFrame);

        if (config.savePreviewBmpDebug)
        {
            std::string bmpFileName = makePreviewBmpFileName(frame->frameId);
            fs::path bmpPath = previewDir / bmpFileName;

            saveGray8AsBmp(
                previewFrame->gray8,
                previewFrame->width,
                previewFrame->height,
                bmpPath
            );
        }

        if (config.saveLatestBmpDebug)
        {
            saveGray8AsBmp(
                previewFrame->gray8,
                previewFrame->width,
                previewFrame->height,
                latestBmpPath
            );
        }

        ++previewSavedCount;

        if (previewSavedCount.load() % 5 == 0)
        {
            logLine("[PreviewWorker] preview frame ",
                previewFrame->frameId,
                " updated latest buffer. processed preview frames = ",
                previewSavedCount.load());
        }

        nextPreviewTime =
            frame->timestamp +
            std::chrono::milliseconds(static_cast<int>(previewIntervalMs));
    }

    logLine("[PreviewWorker] exit. total preview frames = ",
        previewSavedCount.load());
}

AcquisitionTask::AcquisitionTask(CameraBackend backend)
    : backend_(backend)
{
}

AcquisitionTask::~AcquisitionTask()
{
    stopAndWait();
}

bool AcquisitionTask::start(const AcquisitionConfig& config, int targetFrameCount)
{
    std::lock_guard<std::mutex> lock(stateMutex_);

    if (state_ == TaskState::Running)
    {
        logLine("[AcquisitionTask] task is already running.");
        return false;
    }

    config_ = config;
    targetFrameCount_ = targetFrameCount;

    running_ = true;

    savedCount_ = 0;
    previewSavedCount_ = 0;
    peakPreviewCount_ = 0;
    actualCaptureFps_ = 0.0;

    camera_ = createCamera(backend_);

    saveQueue_ = std::make_unique<SharedFrameQueue>(
        50,
        QueueFullPolicy::Block
    );

    previewQueue_ = std::make_unique<SharedFrameQueue>(
        5,
        QueueFullPolicy::DropOldest
    );

    if (!camera_->open())
    {
        logLine("[AcquisitionTask] camera open failed.");
        state_ = TaskState::Error;
        return false;
    }

    if (!camera_->configure(config_))
    {
        logLine("[AcquisitionTask] camera configure failed.");
        camera_->close();
        state_ = TaskState::Error;
        return false;
    }

    if (!camera_->start())
    {
        logLine("[AcquisitionTask] camera start failed.");
        camera_->close();
        state_ = TaskState::Error;
        return false;
    }

    taskStartTime_ = std::chrono::steady_clock::now();

    saveThread_ = std::thread(
        saveLoop,
        std::ref(*saveQueue_),
        std::cref(config_),
        taskStartTime_,
        std::ref(savedCount_),
        std::ref(peakPreviewBuffer_),
        std::ref(peakPreviewCount_),
        std::ref(actualCaptureFps_)
    );

    previewThread_ = std::thread(
        previewLoop,
        std::ref(*previewQueue_),
        std::cref(config_),
        taskStartTime_,
        std::ref(instantPreviewBuffer_),
        std::ref(previewSavedCount_)
    );

    bool enableInstantPreview = true;

    captureThread_ = std::thread(
        captureLoop,
        std::ref(*camera_),
        std::ref(*saveQueue_),
        std::ref(*previewQueue_),
        std::ref(running_),
        targetFrameCount_,
        enableInstantPreview
    );

    state_ = TaskState::Running;

    logLine("[AcquisitionTask] task started.");
    return true;
}

void AcquisitionTask::stopAndWait()
{
    {
        std::lock_guard<std::mutex> lock(stateMutex_);

        if (state_ != TaskState::Running)
        {
            return;
        }

        state_ = TaskState::Stopping;
        running_ = false;
    }

    logLine("[AcquisitionTask] stopping task...");

    if (camera_)
    {
        camera_->stop();
    }

    if (captureThread_.joinable())
    {
        captureThread_.join();
    }

    logLine("[AcquisitionTask] capture thread joined.");
    logLine("[AcquisitionTask] waiting for save and preview workers to drain queues...");

    if (saveThread_.joinable())
    {
        saveThread_.join();
    }

    if (previewThread_.joinable())
    {
        previewThread_.join();
    }

    if (camera_)
    {
        camera_->close();
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = TaskState::Completed;
    }

    logLine("[AcquisitionTask] task completed.");
}

bool AcquisitionTask::isRunning()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return state_ == TaskState::Running;
}

int AcquisitionTask::getSavedCount() const
{
    return savedCount_.load();
}

int AcquisitionTask::getPreviewSavedCount() const
{
    return previewSavedCount_.load();
}

int AcquisitionTask::getPeakPreviewCount() const
{
    return peakPreviewCount_.load();
}

double AcquisitionTask::getActualCaptureFps() const
{
    return actualCaptureFps_.load();
}

bool AcquisitionTask::getLatestPreviewFrame(PreviewFrame8& outFrame)
{
    return getLatestInstantPreviewFrame(outFrame);
}

bool AcquisitionTask::getLatestInstantPreviewFrame(PreviewFrame8& outFrame)
{
    return instantPreviewBuffer_.getLatest(outFrame);
}

bool AcquisitionTask::getLatestPeakPreviewFrame(PreviewFrame8& outFrame)
{
    return peakPreviewBuffer_.getLatest(outFrame);
}

std::string AcquisitionTask::getStateName()
{
    std::lock_guard<std::mutex> lock(stateMutex_);

    switch (state_)
    {
    case TaskState::Idle:
        return "Idle";
    case TaskState::Running:
        return "Running";
    case TaskState::Stopping:
        return "Stopping";
    case TaskState::Completed:
        return "Completed";
    case TaskState::Error:
        return "Error";
    default:
        return "Unknown";
    }
}

std::string AcquisitionTask::getSaveDir()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return config_.saveDir;
}