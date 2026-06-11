#define NOMINMAX

#include "HttpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Bcrypt.lib")

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Log.h"

namespace fs = std::filesystem;

namespace
{
    void appendU32LE(std::vector<uint8_t>& out, uint32_t value)
    {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    }

    void appendU64LE(std::vector<uint8_t>& out, uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
        {
            out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }

    std::vector<uint8_t> buildPreviewPayload(const PreviewFrame8& frame)
    {
        std::vector<uint8_t> payload;

        payload.reserve(24 + frame.gray8.size());

        appendU32LE(payload, static_cast<uint32_t>(frame.width));
        appendU32LE(payload, static_cast<uint32_t>(frame.height));
        appendU64LE(payload, static_cast<uint64_t>(frame.frameId));
        appendU64LE(payload, static_cast<uint64_t>(frame.timestampUs));

        payload.insert(payload.end(), frame.gray8.begin(), frame.gray8.end());

        return payload;
    }

    bool socketSendAll(SOCKET client, const uint8_t* data, size_t length)
    {
        size_t sentTotal = 0;

        while (sentTotal < length)
        {
            size_t remain = length - sentTotal;
            int chunkSize = static_cast<int>(std::min<size_t>(remain, 64 * 1024));

            int sent = send(
                client,
                reinterpret_cast<const char*>(data + sentTotal),
                chunkSize,
                0
            );

            if (sent <= 0)
            {
                return false;
            }

            sentTotal += static_cast<size_t>(sent);
        }

        return true;
    }

    bool socketSendAll(SOCKET client, const std::string& text)
    {
        return socketSendAll(
            client,
            reinterpret_cast<const uint8_t*>(text.data()),
            text.size()
        );
    }

    std::string trimString(const std::string& s)
    {
        size_t start = 0;

        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        {
            ++start;
        }

        size_t end = s.size();

        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        {
            --end;
        }

        return s.substr(start, end - start);
    }

    std::string toLowerString(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
            });

        return s;
    }

    std::string getHttpHeaderValue(const std::string& request, const std::string& headerName)
    {
        std::istringstream iss(request);
        std::string line;

        std::string target = toLowerString(headerName);

        while (std::getline(iss, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            size_t colon = line.find(':');

            if (colon == std::string::npos)
            {
                continue;
            }

            std::string name = toLowerString(trimString(line.substr(0, colon)));
            std::string value = trimString(line.substr(colon + 1));

            if (name == target)
            {
                return value;
            }
        }

        return "";
    }

    std::vector<uint8_t> sha1Bytes(const std::string& input)
    {
        std::vector<uint8_t> result;

        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;

        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &hAlg,
            BCRYPT_SHA1_ALGORITHM,
            nullptr,
            0
        );

        if (status < 0)
        {
            return result;
        }

        DWORD objectLength = 0;
        DWORD hashLength = 0;
        DWORD cbData = 0;

        status = BCryptGetProperty(
            hAlg,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(DWORD),
            &cbData,
            0
        );

        if (status < 0)
        {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return result;
        }

        status = BCryptGetProperty(
            hAlg,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength),
            sizeof(DWORD),
            &cbData,
            0
        );

        if (status < 0)
        {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return result;
        }

        std::vector<uint8_t> hashObject(objectLength);
        std::vector<uint8_t> hash(hashLength);

        status = BCryptCreateHash(
            hAlg,
            &hHash,
            hashObject.data(),
            objectLength,
            nullptr,
            0,
            0
        );

        if (status < 0)
        {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return result;
        }

        status = BCryptHashData(
            hHash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
            static_cast<ULONG>(input.size()),
            0
        );

        if (status >= 0)
        {
            status = BCryptFinishHash(
                hHash,
                hash.data(),
                hashLength,
                0
            );
        }

        if (status >= 0)
        {
            result = hash;
        }

        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);

        return result;
    }

    std::string base64Encode(const std::vector<uint8_t>& data)
    {
        static const char table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string output;

        size_t i = 0;

        while (i + 2 < data.size())
        {
            uint32_t value =
                (static_cast<uint32_t>(data[i]) << 16) |
                (static_cast<uint32_t>(data[i + 1]) << 8) |
                static_cast<uint32_t>(data[i + 2]);

            output.push_back(table[(value >> 18) & 0x3F]);
            output.push_back(table[(value >> 12) & 0x3F]);
            output.push_back(table[(value >> 6) & 0x3F]);
            output.push_back(table[value & 0x3F]);

            i += 3;
        }

        if (i < data.size())
        {
            uint32_t value = static_cast<uint32_t>(data[i]) << 16;

            output.push_back(table[(value >> 18) & 0x3F]);

            if (i + 1 < data.size())
            {
                value |= static_cast<uint32_t>(data[i + 1]) << 8;

                output.push_back(table[(value >> 12) & 0x3F]);
                output.push_back(table[(value >> 6) & 0x3F]);
                output.push_back('=');
            }
            else
            {
                output.push_back(table[(value >> 12) & 0x3F]);
                output.push_back('=');
                output.push_back('=');
            }
        }

        return output;
    }

    std::string makeWebSocketAcceptKey(const std::string& clientKey)
    {
        const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::vector<uint8_t> sha1 = sha1Bytes(clientKey + magic);
        return base64Encode(sha1);
    }

    void sendHttpResponse(
        SOCKET client,
        const std::string& statusLine,
        const std::string& contentType,
        const std::vector<uint8_t>& body)
    {
        std::ostringstream header;

        header << statusLine << "\r\n"
            << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Cache-Control: no-store\r\n"
            << "Connection: close\r\n"
            << "\r\n";

        std::string headerText = header.str();

        socketSendAll(client, headerText);

        if (!body.empty())
        {
            socketSendAll(client, body.data(), body.size());
        }
    }

    void sendHttpText(
        SOCKET client,
        const std::string& statusLine,
        const std::string& contentType,
        const std::string& text)
    {
        std::vector<uint8_t> body(text.begin(), text.end());
        sendHttpResponse(client, statusLine, contentType, body);
    }

    void sendNoContent(SOCKET client)
    {
        std::string response =
            "HTTP/1.1 204 No Content\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n"
            "\r\n";

        socketSendAll(client, response);
    }

    bool sendWebSocketBinary(SOCKET client, const std::vector<uint8_t>& payload)
    {
        std::vector<uint8_t> header;

        header.push_back(0x82);

        uint64_t length = static_cast<uint64_t>(payload.size());

        if (length <= 125)
        {
            header.push_back(static_cast<uint8_t>(length));
        }
        else if (length <= 65535)
        {
            header.push_back(126);
            header.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
            header.push_back(static_cast<uint8_t>(length & 0xFF));
        }
        else
        {
            header.push_back(127);

            for (int i = 7; i >= 0; --i)
            {
                header.push_back(static_cast<uint8_t>((length >> (8 * i)) & 0xFF));
            }
        }

        if (!socketSendAll(client, header.data(), header.size()))
        {
            return false;
        }

        if (!payload.empty())
        {
            if (!socketSendAll(client, payload.data(), payload.size()))
            {
                return false;
            }
        }

        return true;
    }

    std::string makeTaskSaveDir(const std::string& outputBaseDir)
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);

        std::tm tmValue{};
        localtime_s(&tmValue, &t);

        std::ostringstream name;
        name << "task_"
            << std::put_time(&tmValue, "%Y%m%d_%H%M%S");

        fs::path baseDir;

        if (outputBaseDir.empty())
        {
            baseDir = fs::path("acq_output");
        }
        else
        {
            baseDir = fs::path(outputBaseDir);
        }

        return (baseDir / name.str()).string();
    }

    std::string jsonEscape(const std::string& s)
    {
        std::string out;

        for (char c : s)
        {
            switch (c)
            {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
            }
        }

        return out;
    }

    std::map<std::string, std::string> parseQueryString(const std::string& query)
    {
        std::map<std::string, std::string> result;

        size_t start = 0;

        while (start < query.size())
        {
            size_t amp = query.find('&', start);

            std::string item;

            if (amp == std::string::npos)
            {
                item = query.substr(start);
                start = query.size();
            }
            else
            {
                item = query.substr(start, amp - start);
                start = amp + 1;
            }

            size_t eq = item.find('=');

            if (eq != std::string::npos)
            {
                std::string key = item.substr(0, eq);
                std::string value = item.substr(eq + 1);
                result[key] = value;
            }
        }

        return result;
    }

    int queryInt(
        const std::map<std::string, std::string>& query,
        const std::string& key,
        int defaultValue)
    {
        auto it = query.find(key);

        if (it == query.end())
        {
            return defaultValue;
        }

        try
        {
            return std::stoi(it->second);
        }
        catch (...)
        {
            return defaultValue;
        }
    }

    double queryDouble(
        const std::map<std::string, std::string>& query,
        const std::string& key,
        double defaultValue)
    {
        auto it = query.find(key);

        if (it == query.end())
        {
            return defaultValue;
        }

        try
        {
            return std::stod(it->second);
        }
        catch (...)
        {
            return defaultValue;
        }
    }

    std::string queryString(
        const std::map<std::string, std::string>& query,
        const std::string& key,
        const std::string& defaultValue)
    {
        auto it = query.find(key);

        if (it == query.end())
        {
            return defaultValue;
        }

        return it->second;
    }



    AcquisitionConfig buildConfigFromQuery(
        const std::map<std::string, std::string>& query,
        const std::string& outputBaseDir)
    {
        AcquisitionConfig config;

        std::string acquisitionMode =
            queryString(query, "acquisitionMode", "continuous");

        if (acquisitionMode == "rising")
        {
            config.trigger = TriggerMode::RisingEdge;
        }
        else if (acquisitionMode == "falling")
        {
            config.trigger = TriggerMode::FallingEdge;
        }
        else if (acquisitionMode == "low")
        {
            config.trigger = TriggerMode::LowLevel;
        }
        else if (acquisitionMode == "high")
        {
            config.trigger = TriggerMode::HighLevel;
        }
        else if (acquisitionMode == "snapshot")
        {
            config.trigger = TriggerMode::Snapshot;
        }
        else
        {
            config.trigger = TriggerMode::Continuous;
        }

        std::string binning = queryString(query, "binning", "1x1");

        if (binning == "2x2")
        {
            config.binning = BinningMode::Bin2x2;
        }
        else if (binning == "3x3")
        {
            config.binning = BinningMode::Bin3x3;
        }
        else if (binning == "4x4")
        {
            config.binning = BinningMode::Bin4x4;
        }
        else
        {
            config.binning = BinningMode::Bin1x1;
        }

        config.roi.x = queryInt(query, "offsetX", 0);
        config.roi.y = queryInt(query, "offsetY", 0);
        config.roi.width = queryInt(query, "width", 1024);
        config.roi.height = queryInt(query, "height", 768);

        std::string speedControlMode =
            queryString(query, "speedControlMode", "fps");

        if (speedControlMode == "exposure")
        {
            config.speedControlMode = SpeedControlMode::ExposureTime;
        }
        else
        {
            config.speedControlMode = SpeedControlMode::FPS;
        }

        config.fps = queryDouble(query, "fps", 30.0);
        config.previewFps = queryDouble(query, "previewFps", 15.0);
        config.exposureMs = queryDouble(query, "exposureMs", 10.0);
        config.gain = queryDouble(query, "gain", 1.0);


        std::string previewMode =
            queryString(query, "previewMode", "instant");

        if (previewMode == "peak")
        {
            config.previewRenderMode = PreviewRenderMode::PeakHold;
        }
        else
        {
            config.previewRenderMode = PreviewRenderMode::Instant;
        }

        if (config.roi.width <= 0)
        {
            config.roi.width = 1024;
        }

        if (config.roi.height <= 0)
        {
            config.roi.height = 768;
        }

        if (config.roi.x < 0)
        {
            config.roi.x = 0;
        }

        if (config.roi.y < 0)
        {
            config.roi.y = 0;
        }

        if (config.fps <= 0)
        {
            config.fps = 30.0;
        }

        if (config.previewFps <= 0)
        {
            config.previewFps = 15.0;
        }

        config.saveDir = makeTaskSaveDir(outputBaseDir);

        config.savePreviewBmpDebug = false;
        config.saveLatestBmpDebug = false;
        config.generateBmpViewerHtml = false;

        return config;
    }

    std::string buildStatusJson(AcquisitionTask& task)
    {
        PreviewFrame8 latest;
        bool hasLatest = task.getLatestPreviewFrame(latest);

        std::ostringstream oss;

        oss << "{\n";
        oss << "  \"state\": \"" << task.getStateName() << "\",\n";
        oss << "  \"running\": " << (task.isRunning() ? "true" : "false") << ",\n";
        oss << "  \"saved_count\": " << task.getSavedCount() << ",\n";
        oss << "  \"preview_frames_processed\": " << task.getPreviewSavedCount() << ",\n";
        oss << "  \"save_dir\": \"" << jsonEscape(task.getSaveDir()) << "\",\n";
        oss << "  \"has_latest_preview\": " << (hasLatest ? "true" : "false");

        if (hasLatest)
        {
            oss << ",\n";
            oss << "  \"latest_frame_id\": " << latest.frameId << ",\n";
            oss << "  \"latest_width\": " << latest.width << ",\n";
            oss << "  \"latest_height\": " << latest.height << ",\n";
            oss << "  \"latest_timestamp_us\": " << latest.timestampUs << "\n";
        }
        else
        {
            oss << "\n";
        }

        oss << "}";

        return oss.str();
    }

    std::string buildCanvasHtml()
    {
        std::string html;

        html += R"HTML(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <title>DeepTouch可视化操作系统</title>
    <style>
        body {
            margin: 0;
            background: #dff3ff;
            color: #123;
            font-family: "Microsoft YaHei", "Segoe UI", Arial, sans-serif;
        }

        .container {
            padding: 20px;
        }

        .title-bar {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 16px;
        }

        h1 {
            font-size: 28px;
            margin: 0;
            color: #0b3f75;
            letter-spacing: 1px;
        }
    )HTML";

        html += R"HTML(
        .ws-status {
            padding: 8px 14px;
            border-radius: 999px;
            background: #ffffffcc;
            border: 1px solid #9cc8e6;
            font-weight: bold;
            color: #b00020;
        }

        .ws-status.connected {
            color: #087a24;
        }

        .panel {
            display: grid;
            grid-template-columns: repeat(6, minmax(120px, 1fr));
            gap: 12px;
            background: #ffffffcc;
            border: 1px solid #9cc8e6;
            border-radius: 14px;
            padding: 16px;
            margin-bottom: 16px;
            box-shadow: 0 4px 14px rgba(25, 80, 130, 0.12);
        }

        label {
            display: block;
            color: #245;
            font-size: 14px;
            margin-bottom: 5px;
            font-weight: bold;
        }
    )HTML";

        html += R"HTML(
        input, select {
            width: 100%;
            box-sizing: border-box;
            padding: 8px;
            background: #f7fcff;
            color: #123;
            border: 1px solid #8ab9d8;
            border-radius: 6px;
            font-size: 14px;
        }

        .button-row {
            display: flex;
            gap: 12px;
            align-items: end;
        }

        button {
            padding: 10px 18px;
            background: #1e88e5;
            color: #fff;
            border: none;
            border-radius: 8px;
            cursor: pointer;
            font-size: 15px;
            font-weight: bold;
        }

        button.stop {
            background: #d64545;
        }

        button:hover {
            opacity: 0.9;
        }
    )HTML";

        html += R"HTML(
        .preview-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 16px;
        }

        .preview-card {
            background: #ffffffcc;
            border: 1px solid #9cc8e6;
            border-radius: 14px;
            padding: 12px;
            box-shadow: 0 4px 14px rgba(25, 80, 130, 0.12);
        }

        .preview-card h2 {
            margin: 0 0 10px 0;
            color: #0b3f75;
            font-size: 18px;
        }

        .canvas-wrap {
            position: relative;
            width: 100%;
            max-height: 72vh;
        }

        canvas {
            width: 100%;
            max-height: 72vh;
            border: 1px solid #7aaed0;
            background: #000;
            image-rendering: pixelated;
        }

        #roiOverlayCanvas {
            position: absolute;
            left: 0;
            top: 0;
            background: transparent;
            cursor: crosshair;
        }

        @media (max-width: 1100px) {
            .panel {
                grid-template-columns: repeat(3, minmax(120px, 1fr));
            }

            .preview-grid {
                grid-template-columns: 1fr;
            }
        }
    </style>
</head>
    )HTML";

        html += R"HTML(
<body>
    <div class="container">
        <div class="title-bar">
            <h1>DeepTouch可视化操作系统</h1>
            <div id="wsStatus" class="ws-status">WebSocket未连接</div>
        </div>

        <div class="panel">
            <div>
                <label>采集模式</label>
                <select id="acquisitionMode">
                    <option value="continuous">连续采集</option>
                    <option value="rising">上升沿触发</option>
                    <option value="falling">下降沿触发</option>
                    <option value="low">低电平触发</option>
                    <option value="high">高电平触发</option>
                    <option value="snapshot">快�
�触发</option>
                </select>
            </div>

            <div>
                <label>Binning模式</label>
                <select id="binning">
                    <option value="1x1">Bin 1×1</option>
                    <option value="2x2">Bin 2×2</option>
                </select>
            </div>

            <div>
                <label>ROI起点X</label>
                <input id="offsetX" type="number" value="0">
            </div>

            <div>
                <label>ROI起点Y</label>
                <input id="offsetY" type="number" value="0">
            </div>

            <div>
                <label>ROI宽度</label>
                <input id="width" type="number" value="1024">
            </div>

            <div>
                <label>ROI高度</label>
                <input id="height" type="number" value="768">
            </div>
    )HTML";

        html += R"HTML(
            <div>
                <label>速度控制方式</label>
                <select id="speedControlMode">
                    <option value="fps">按帧率控制</option>
                    <option value="exposure">按曝�
�时间控制</option>
                </select>
            </div>

            <div>
                <label>采集帧率 FPS</label>
                <input id="fps" type="number" value="30">
            </div>

            <div>
                <label>曝�
�时间 ms</label>
                <input id="exposureMs" type="number" value="10">
            </div>

            <div>
                <label>增益档位</label>
                <input id="gain" type="number" value="1">
            </div>

<div>
    <label>瞬时图显示模式</label>
    <select id="instantDisplayMode">
        <option value="gray">瞬时图灰度显示</option>
        <option value="heat">瞬时图热力显示</option>
    </select>
</div>

<div>
    <label>峰值图显示模式</label>
    <select id="peakDisplayMode">
        <option value="gray">峰值图灰度显示</option>
        <option value="heat">峰值图热力显示</option>
    </select>
</div>



            <div>
                <label>预览帧率 FPS</label>
                <input id="previewFps" type="number" value="15">
            </div>

            <div class="button-row">
                <button onclick="startAcq()">开始采集</button>
                <button class="stop" onclick="stopAcq()">停止采集</button>
                <button onclick="setFullFrameRoi()">�
��
ROI</button>
                <button onclick="clearRoiBox()">�
除框选</button>
            </div>
        </div>

        <div class="preview-grid">
            <div class="preview-card">
                <h2>瞬时图像（可拖拽框选ROI）</h2>
                <div class="canvas-wrap" id="instantCanvasWrap">
                    <canvas id="instantCanvas"></canvas>
                    <canvas id="roiOverlayCanvas"></canvas>
                </div>
            </div>

            <div class="preview-card">
                <h2>峰值保持图</h2>
                <canvas id="peakCanvas"></canvas>
            </div>
        </div>
    </div>
    )HTML";

        html += R"HTML(
    <script>
        const instantCanvas = document.getElementById("instantCanvas");
        const peakCanvas = document.getElementById("peakCanvas");
        const roiOverlayCanvas = document.getElementById("roiOverlayCanvas");
        const wsStatus = document.getElementById("wsStatus");

        const SENSOR_WIDTH = 1274;
        const SENSOR_HEIGHT = 1024;

        let instantConnected = false;
        let peakConnected = false;

        let isSelectingRoi = false;
        let roiStartX = 0;
        let roiStartY = 0;
        let roiCurrentX = 0;
        let roiCurrentY = 0;

        let latestInstantWidth = SENSOR_WIDTH;
        let latestInstantHeight = SENSOR_HEIGHT;

        function updateWsStatus() {
            if (instantConnected && peakConnected) {
                wsStatus.textContent = "WebSocket已连接";
                wsStatus.classList.add("connected");
            } else {
                wsStatus.textContent = "WebSocket未连接";
                wsStatus.classList.remove("connected");
            }
        }

        function getValue(id) {
            return document.getElementById(id).value;
        }
    )HTML";

        html += R"HTML(
        async function startAcq() {


            const params = new URLSearchParams();
            params.set("instantDisplayMode", getValue("instantDisplayMode"));
            params.set("peakDisplayMode", getValue("peakDisplayMode"));
            params.set("acquisitionMode", getValue("acquisitionMode"));
            params.set("binning", getValue("binning"));
            params.set("offsetX", getValue("offsetX"));
            params.set("offsetY", getValue("offsetY"));
            params.set("width", getValue("width"));
            params.set("height", getValue("height"));
            params.set("speedControlMode", getValue("speedControlMode"));
            params.set("fps", getValue("fps"));
            params.set("exposureMs", getValue("exposureMs"));
            params.set("gain", getValue("gain"));
            params.set("previewFps", getValue("previewFps"));

            const res = await fetch("/api/start?" + params.toString(), {
                cache: "no-store"
            });

            if (!res.ok) {
                alert("开始采集失败，请查看后端控制台日志。");
            }
        }

        async function stopAcq() {
    const res = await fetch("/api/stop", {
        cache: "no-store"
    });

    if (!res.ok) {
        alert("停止采集失败，请查看后端控制台日志。");
        return;
    }

}
        
    )HTML";

        html += R"HTML(
function heatColor(v) {
    const t = Math.max(0.0, Math.min(1.0, v / 255.0));

    const points = [
        [0.00,   0,   8,  60],
        [0.20,   0,  50, 180],
        [0.40,   0, 190, 255],
        [0.55,   0, 230, 120],
        [0.70, 255, 240,   0],
        [0.85, 255, 100,   0],
        [1.00, 255,   0,   0]
    ];

    for (let i = 0; i < points.length - 1; ++i) {
        const p0 = points[i];
        const p1 = points[i + 1];

        if (t >= p0[0] && t <= p1[0]) {
            const u = (t - p0[0]) / (p1[0] - p0[0]);

            return [
                Math.round(p0[1] + u * (p1[1] - p0[1])),
                Math.round(p0[2] + u * (p1[2] - p0[2])),
                Math.round(p0[3] + u * (p1[3] - p0[3]))
            ];
        }
    }

    return [255, 0, 0];
}

function buildHeatLut() {
    const lut = new Uint8ClampedArray(256 * 3);

    for (let v = 0; v < 256; ++v) {
        const c = heatColor(v);
        const k = v * 3;

        lut[k] = c[0];
        lut[k + 1] = c[1];
        lut[k + 2] = c[2];
    }

    return lut;
}

const deepTouchHeatLut = buildHeatLut();

function drawFrameToCanvas(buffer, canvas, displayMode) {
    if (buffer.byteLength < 24) {
        return;
    }

    const view = new DataView(buffer);

    const width = view.getUint32(0, true);
    const height = view.getUint32(4, true);

    const expectedSize = 24 + width * height;

    if (buffer.byteLength < expectedSize) {
        return;
    }

    if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
    }

    if (canvas === instantCanvas) {
        latestInstantWidth = width;
        latestInstantHeight = height;

        if (roiOverlayCanvas.width !== width || roiOverlayCanvas.height !== height) {
            roiOverlayCanvas.width = width;
            roiOverlayCanvas.height = height;
        }

        syncOverlayCanvasCssSize();
    }

    const ctx = canvas.getContext("2d");
    const gray8 = new Uint8Array(buffer, 24, width * height);
    const imageData = ctx.createImageData(width, height);
    const rgba = imageData.data;

    for (let i = 0, j = 0; i < gray8.length; ++i, j += 4) {
        const v = gray8[i];

        if (displayMode === "heat") {
            const k = v * 3;

            rgba[j] = deepTouchHeatLut[k];
            rgba[j + 1] = deepTouchHeatLut[k + 1];
            rgba[j + 2] = deepTouchHeatLut[k + 2];
            rgba[j + 3] = 255;
        } else {
            rgba[j] = v;
            rgba[j + 1] = v;
            rgba[j + 2] = v;
            rgba[j + 3] = 255;
        }
    }

    ctx.putImageData(imageData, 0, 0);
}

    )HTML";

        html += R"HTML(
        function syncOverlayCanvasCssSize() {
            const rect = instantCanvas.getBoundingClientRect();

            roiOverlayCanvas.style.width = rect.width + "px";
            roiOverlayCanvas.style.height = rect.height + "px";
        }

        function clamp(value, minValue, maxValue) {
            return Math.max(minValue, Math.min(maxValue, value));
        }

        function getCanvasPixelPosition(event) {
            const rect = roiOverlayCanvas.getBoundingClientRect();

            const x = (event.clientX - rect.left) * roiOverlayCanvas.width / rect.width;
            const y = (event.clientY - rect.top) * roiOverlayCanvas.height / rect.height;

            return {
                x: clamp(Math.round(x), 0, roiOverlayCanvas.width - 1),
                y: clamp(Math.round(y), 0, roiOverlayCanvas.height - 1)
            };
        }

        function drawRoiOverlay() {
            const ctx = roiOverlayCanvas.getContext("2d");

            ctx.clearRect(0, 0, roiOverlayCanvas.width, roiOverlayCanvas.height);

            if (!isSelectingRoi) {
                return;
            }

            const x = Math.min(roiStartX, roiCurrentX);
            const y = Math.min(roiStartY, roiCurrentY);
            const w = Math.abs(roiCurrentX - roiStartX);
            const h = Math.abs(roiCurrentY - roiStartY);

            ctx.lineWidth = 3;
            ctx.strokeStyle = "#ff2b2b";
            ctx.setLineDash([8, 4]);
            ctx.strokeRect(x, y, w, h);

            ctx.fillStyle = "rgba(255, 43, 43, 0.15)";
            ctx.fillRect(x, y, w, h);
        }
    )HTML";

        html += R"HTML(
        function applySelectedRoi() {
            let x = Math.min(roiStartX, roiCurrentX);
            let y = Math.min(roiStartY, roiCurrentY);
            let w = Math.abs(roiCurrentX - roiStartX);
            let h = Math.abs(roiCurrentY - roiStartY);

            if (w < 8 || h < 8) {
                return;
            }

            const currentOffsetX = parseInt(getValue("offsetX") || "0", 10);
            const currentOffsetY = parseInt(getValue("offsetY") || "0", 10);

            let sensorX = currentOffsetX + x;
            let sensorY = currentOffsetY + y;
            let sensorW = w;
            let sensorH = h;

            sensorX = clamp(sensorX, 0, SENSOR_WIDTH - 1);
            sensorY = clamp(sensorY, 0, SENSOR_HEIGHT - 1);

            if (sensorX + sensorW > SENSOR_WIDTH) {
                sensorW = SENSOR_WIDTH - sensorX;
            }

            if (sensorY + sensorH > SENSOR_HEIGHT) {
                sensorH = SENSOR_HEIGHT - sensorY;
            }

            sensorW = Math.max(8, Math.floor(sensorW / 2) * 2);
            sensorH = Math.max(8, Math.floor(sensorH / 2) * 2);

            document.getElementById("offsetX").value = sensorX;
            document.getElementById("offsetY").value = sensorY;
            document.getElementById("width").value = sensorW;
            document.getElementById("height").value = sensorH;

            drawFinalRoiBox(x, y, w, h);
        }

        function drawFinalRoiBox(x, y, w, h) {
            const ctx = roiOverlayCanvas.getContext("2d");

            ctx.clearRect(0, 0, roiOverlayCanvas.width, roiOverlayCanvas.height);

            ctx.lineWidth = 3;
            ctx.strokeStyle = "#ff2b2b";
            ctx.setLineDash([]);
            ctx.strokeRect(x, y, w, h);

            ctx.fillStyle = "rgba(255, 43, 43, 0.12)";
            ctx.fillRect(x, y, w, h);
        }
    )HTML";

        html += R"HTML(
        roiOverlayCanvas.addEventListener("mousedown", function (event) {
            const p = getCanvasPixelPosition(event);

            isSelectingRoi = true;

            roiStartX = p.x;
            roiStartY = p.y;
            roiCurrentX = p.x;
            roiCurrentY = p.y;

            drawRoiOverlay();
        });

        roiOverlayCanvas.addEventListener("mousemove", function (event) {
            if (!isSelectingRoi) {
                return;
            }

            const p = getCanvasPixelPosition(event);

            roiCurrentX = p.x;
            roiCurrentY = p.y;

            drawRoiOverlay();
        });

        roiOverlayCanvas.addEventListener("mouseup", function (event) {
            if (!isSelectingRoi) {
                return;
            }

            const p = getCanvasPixelPosition(event);

            roiCurrentX = p.x;
            roiCurrentY = p.y;

            isSelectingRoi = false;

            applySelectedRoi();
        });

        roiOverlayCanvas.addEventListener("mouseleave", function () {
            if (!isSelectingRoi) {
                return;
            }

            isSelectingRoi = false;
            applySelectedRoi();
        });
    )HTML";

        html += R"HTML(
        function setFullFrameRoi() {
            document.getElementById("offsetX").value = 0;
            document.getElementById("offsetY").value = 0;
            document.getElementById("width").value = SENSOR_WIDTH;
            document.getElementById("height").value = SENSOR_HEIGHT;

            clearRoiBox();
        }

        function clearRoiBox() {
            const ctx = roiOverlayCanvas.getContext("2d");
            ctx.clearRect(0, 0, roiOverlayCanvas.width, roiOverlayCanvas.height);
        }

        function connectPreviewSocket(path, canvas, type) {
            const wsUrl = "ws://" + window.location.host + path;
            const ws = new WebSocket(wsUrl);

            ws.binaryType = "arraybuffer";

            ws.onopen = function () {
                if (type === "instant") {
                    instantConnected = true;
                } else {
                    peakConnected = true;
                }

                updateWsStatus();
            };

            ws.onmessage = function (event) {
    let displayMode = "gray";

    if (type === "instant") {
        displayMode = document.getElementById("instantDisplayMode").value;
    } else {
        displayMode = document.getElementById("peakDisplayMode").value;
    }

    drawFrameToCanvas(event.data, canvas, displayMode);
};

            ws.onerror = function () {
                if (type === "instant") {
                    instantConnected = false;
                } else {
                    peakConnected = false;
                }

                updateWsStatus();
            };

            ws.onclose = function () {
                if (type === "instant") {
                    instantConnected = false;
                } else {
                    peakConnected = false;
                }

                updateWsStatus();

                setTimeout(function () {
                    connectPreviewSocket(path, canvas, type);
                }, 1000);
            };
        }

        window.addEventListener("resize", function () {
            syncOverlayCanvasCssSize();
        });

        updateWsStatus();
        connectPreviewSocket("/ws/instant", instantCanvas, "instant");
        connectPreviewSocket("/ws/peak", peakCanvas, "peak");
    </script>
</body>
</html>
    )HTML";

        return html;
    }

    void handleWebSocketClient(
        SOCKET client,
        const std::string& request,
        AcquisitionTask& task,
        std::atomic<bool>& serverRunning,
        int pushFps,
        const std::string& streamType)
    {
        std::string clientKey = getHttpHeaderValue(request, "Sec-WebSocket-Key");

        if (clientKey.empty())
        {
            sendHttpText(
                client,
                "HTTP/1.1 400 Bad Request",
                "text/plain; charset=utf-8",
                "Missing Sec-WebSocket-Key"
            );
            return;
        }

        std::string acceptKey = makeWebSocketAcceptKey(clientKey);

        std::ostringstream response;

        response << "HTTP/1.1 101 Switching Protocols\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Accept: " << acceptKey << "\r\n"
            << "\r\n";

        if (!socketSendAll(client, response.str()))
        {
            return;
        }

        logLine("[WebSocket] client connected: ", streamType);

        uint64_t lastFrameId = static_cast<uint64_t>(-1);

        if (pushFps <= 0)
        {
            pushFps = 30;
        }

        int intervalMs = static_cast<int>(1000.0 / pushFps);

        if (intervalMs < 1)
        {
            intervalMs = 1;
        }

        while (serverRunning)
        {
            PreviewFrame8 latest;

            bool hasFrame = false;

            if (streamType == "peak")
            {
                hasFrame = task.getLatestPeakPreviewFrame(latest);
            }
            else
            {
                hasFrame = task.getLatestInstantPreviewFrame(latest);
            }

            if (hasFrame)
            {
                if (latest.frameId != lastFrameId)
                {
                    lastFrameId = latest.frameId;

                    std::vector<uint8_t> payload = buildPreviewPayload(latest);

                    if (!sendWebSocketBinary(client, payload))
                    {
                        break;
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        }

        logLine("[WebSocket] client disconnected: ", streamType);
    }

    void handleHttpClient(
        SOCKET client,
        AcquisitionTask& task,
        std::atomic<bool>& serverRunning,
        const std::string& outputBaseDir)
    {
        char buffer[4096] = { 0 };

        int received = recv(client, buffer, sizeof(buffer) - 1, 0);

        if (received <= 0)
        {
            return;
        }

        std::string request(buffer, received);

        size_t firstSpace = request.find(' ');
        size_t secondSpace = std::string::npos;

        if (firstSpace != std::string::npos)
        {
            secondSpace = request.find(' ', firstSpace + 1);
        }

        if (firstSpace == std::string::npos || secondSpace == std::string::npos)
        {
            sendHttpText(
                client,
                "HTTP/1.1 400 Bad Request",
                "text/plain; charset=utf-8",
                "Bad Request"
            );
            return;
        }

        std::string fullPath = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);

        std::string path = fullPath;
        std::string queryString;

        size_t queryPos = fullPath.find('?');

        if (queryPos != std::string::npos)
        {
            path = fullPath.substr(0, queryPos);
            queryString = fullPath.substr(queryPos + 1);
        }

        std::map<std::string, std::string> query = parseQueryString(queryString);

        if (path == "/" || path == "/index.html")
        {
            sendHttpText(
                client,
                "HTTP/1.1 200 OK",
                "text/html; charset=utf-8",
                buildCanvasHtml()
            );
            return;
        }

        if (path == "/api/start")
        {
            if (task.isRunning())
            {
                sendHttpText(
                    client,
                    "HTTP/1.1 409 Conflict",
                    "application/json; charset=utf-8",
                    "{ \"ok\": false, \"message\": \"task is already running\" }"
                );
                return;
            }

            AcquisitionConfig config = buildConfigFromQuery(query, outputBaseDir);

            int targetFrameCount = queryInt(query, "targetFrameCount", 0);

            bool ok = task.start(config, targetFrameCount);

            if (ok)
            {
                std::ostringstream oss;
                oss << "{\n";
                oss << "  \"ok\": true,\n";
                oss << "  \"message\": \"task started\",\n";
                oss << "  \"save_dir\": \"" << jsonEscape(config.saveDir) << "\"\n";
                oss << "}";

                sendHttpText(
                    client,
                    "HTTP/1.1 200 OK",
                    "application/json; charset=utf-8",
                    oss.str()
                );
            }
            else
            {
                sendHttpText(
                    client,
                    "HTTP/1.1 500 Internal Server Error",
                    "application/json; charset=utf-8",
                    "{ \"ok\": false, \"message\": \"failed to start task\" }"
                );
            }

            return;
        }

        if (path == "/api/stop")
        {
            task.stopAndWait();

            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"ok\": true,\n";
            oss << "  \"message\": \"task stopped\",\n";
            oss << "  \"status\": " << buildStatusJson(task) << "\n";
            oss << "}";

            sendHttpText(
                client,
                "HTTP/1.1 200 OK",
                "application/json; charset=utf-8",
                oss.str()
            );

            return;
        }

        if (path == "/api/status")
        {
            sendHttpText(
                client,
                "HTTP/1.1 200 OK",
                "application/json; charset=utf-8",
                buildStatusJson(task)
            );

            return;
        }

        if (path == "/ws" || path == "/ws/instant")
        {
            handleWebSocketClient(
                client,
                request,
                task,
                serverRunning,
                60,
                "instant"
            );

            return;
        }

        if (path == "/ws/peak")
        {
            handleWebSocketClient(
                client,
                request,
                task,
                serverRunning,
                60,
                "peak"
            );

            return;
        }

        if (path == "/api/latest")
        {
            PreviewFrame8 latest;

            if (!task.getLatestPreviewFrame(latest))
            {
                sendNoContent(client);
                return;
            }

            std::vector<uint8_t> payload = buildPreviewPayload(latest);

            sendHttpResponse(
                client,
                "HTTP/1.1 200 OK",
                "application/octet-stream",
                payload
            );

            return;
        }

        sendHttpText(
            client,
            "HTTP/1.1 404 Not Found",
            "text/plain; charset=utf-8",
            "Not Found"
        );
    }
}

void httpServerLoop(
    AcquisitionTask& task,
    std::atomic<bool>& running,
    int port,
    const std::string& outputBaseDir)
{
    WSADATA wsaData{};

    int wsaRet = WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (wsaRet != 0)
    {
        logLine("[HttpServer] WSAStartup failed: ", wsaRet);
        return;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listenSock == INVALID_SOCKET)
    {
        logLine("[HttpServer] socket creation failed.");
        WSACleanup();
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));

    int bindRet = bind(
        listenSock,
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr)
    );

    if (bindRet == SOCKET_ERROR)
    {
        logLine("[HttpServer] bind failed. Is port ", port, " already used?");
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    int listenRet = listen(listenSock, SOMAXCONN);

    if (listenRet == SOCKET_ERROR)
    {
        logLine("[HttpServer] listen failed.");
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    u_long nonBlocking = 1;
    ioctlsocket(listenSock, FIONBIO, &nonBlocking);

    logLine("[HttpServer] started at http://127.0.0.1:", port, "/");

    std::vector<std::thread> clientThreads;

    while (running)
    {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);

        SOCKET client = accept(
            listenSock,
            reinterpret_cast<sockaddr*>(&clientAddr),
            &clientLen
        );

        if (client == INVALID_SOCKET)
        {
            int err = WSAGetLastError();

            if (err == WSAEWOULDBLOCK)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            logLine("[HttpServer] accept failed: ", err);
            break;
        }

        clientThreads.emplace_back([client, &task, &running, outputBaseDir]() {
            handleHttpClient(client, task, running, outputBaseDir);
            closesocket(client);
            });
    }

    for (auto& t : clientThreads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    closesocket(listenSock);
    WSACleanup();

    logLine("[HttpServer] stopped.");
}
