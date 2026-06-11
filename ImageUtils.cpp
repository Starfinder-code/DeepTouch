#include "ImageUtils.h"

#include <algorithm>
#include <fstream>

#include "Log.h"

std::vector<uint8_t> convert16To8AutoWindow(const Frame& frame)
{
    std::vector<uint8_t> gray8;
    gray8.resize(frame.pixels.size());

    if (frame.pixels.empty())
    {
        return gray8;
    }

    auto minmax = std::minmax_element(frame.pixels.begin(), frame.pixels.end());
    uint16_t minValue = *minmax.first;
    uint16_t maxValue = *minmax.second;

    if (maxValue == minValue)
    {
        std::fill(gray8.begin(), gray8.end(), 0);
        return gray8;
    }

    double scale = 255.0 / static_cast<double>(maxValue - minValue);

    for (size_t i = 0; i < frame.pixels.size(); ++i)
    {
        int value = static_cast<int>((frame.pixels[i] - minValue) * scale);

        if (value < 0)
        {
            value = 0;
        }
        else if (value > 255)
        {
            value = 255;
        }

        gray8[i] = static_cast<uint8_t>(value);
    }

    return gray8;
}

std::vector<uint8_t> convertFrameTo8FixedLinear(const Frame& frame)
{
    std::vector<uint8_t> gray8;
    gray8.resize(frame.pixels.size());

    int bitDepth = frame.bitDepth;

    if (bitDepth <= 0)
    {
        bitDepth = 16;
    }

    for (size_t i = 0; i < frame.pixels.size(); ++i)
    {
        uint16_t v = frame.pixels[i];

        int out = 0;

        if (bitDepth >= 16)
        {
            // 16-bit: 0~65535 -> 0~255
            out = static_cast<int>(v >> 8);
        }
        else if (bitDepth == 14)
        {
            // 14-bit: 0~16383 -> 0~255
            out = static_cast<int>(v >> 6);
        }
        else if (bitDepth == 12)
        {
            // 12-bit: 0~4095 -> 0~255
            out = static_cast<int>(v >> 4);
        }
        else if (bitDepth == 8)
        {
            // 如果 Mono8 已经被扩展成 value << 8，这里右移 8
            out = static_cast<int>(v >> 8);
        }
        else
        {
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

namespace
{
    void writeU16(std::ofstream& out, uint16_t value)
    {
        out.put(static_cast<char>(value & 0xFF));
        out.put(static_cast<char>((value >> 8) & 0xFF));
    }

    void writeU32(std::ofstream& out, uint32_t value)
    {
        out.put(static_cast<char>(value & 0xFF));
        out.put(static_cast<char>((value >> 8) & 0xFF));
        out.put(static_cast<char>((value >> 16) & 0xFF));
        out.put(static_cast<char>((value >> 24) & 0xFF));
    }
}

bool saveGray8AsBmp(
    const std::vector<uint8_t>& gray8,
    int width,
    int height,
    const std::filesystem::path& filePath)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    if (gray8.size() != static_cast<size_t>(width) * height)
    {
        return false;
    }

    std::ofstream out(filePath, std::ios::binary);

    if (!out.is_open())
    {
        logLine("[PreviewWorker] failed to open bmp file: ", filePath.string());
        return false;
    }

    int rowBytes = width * 3;
    int paddedRowBytes = (rowBytes + 3) / 4 * 4;
    int paddingBytes = paddedRowBytes - rowBytes;

    uint32_t pixelDataSize = static_cast<uint32_t>(paddedRowBytes * height);
    uint32_t fileSize = 54 + pixelDataSize;

    out.put('B');
    out.put('M');
    writeU32(out, fileSize);
    writeU16(out, 0);
    writeU16(out, 0);
    writeU32(out, 54);

    writeU32(out, 40);
    writeU32(out, static_cast<uint32_t>(width));
    writeU32(out, static_cast<uint32_t>(height));
    writeU16(out, 1);
    writeU16(out, 24);
    writeU32(out, 0);
    writeU32(out, pixelDataSize);
    writeU32(out, 0);
    writeU32(out, 0);
    writeU32(out, 0);
    writeU32(out, 0);

    std::vector<uint8_t> padding(static_cast<size_t>(paddingBytes), 0);

    for (int y = height - 1; y >= 0; --y)
    {
        for (int x = 0; x < width; ++x)
        {
            uint8_t v = gray8[static_cast<size_t>(y) * width + x];

            out.put(static_cast<char>(v));
            out.put(static_cast<char>(v));
            out.put(static_cast<char>(v));
        }

        if (paddingBytes > 0)
        {
            out.write(reinterpret_cast<const char*>(padding.data()), paddingBytes);
        }
    }

    return out.good();
}