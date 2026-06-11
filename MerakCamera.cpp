#include "MerakCamera.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>

#include "Log.h"

// MerakSDK headers
#include "nvTypes.h"
#include "nvDentalDet.h"

MerakCamera::MerakCamera()
{
}

MerakCamera::~MerakCamera()
{
    close();
}

std::string MerakCamera::getLastSdkErrorText()
{
    char msg[1024] = { 0 };

    NV_StatusType ret = NV_LastErrorMsg(
        msg,
        static_cast<unsigned int>(sizeof(msg))
    );

    if (ret == NV_SC_SUCCESS && msg[0] != '\0')
    {
        return std::string(msg);
    }

    return "";
}

bool MerakCamera::checkSdkStatus(int ret, const char* apiName)
{
    if (ret == NV_SC_SUCCESS)
    {
        logLine("[MerakCamera] ", apiName, " success.");
        return true;
    }

    logLine("[MerakCamera] ", apiName, " failed. ret = ", ret);

    std::string err = getLastSdkErrorText();

    if (!err.empty())
    {
        logLine("[MerakCamera] SDK last error: ", err);
    }

    return false;
}

void MerakCamera::sdkEventCallback(
    void* lpCallbackData,
    NV_EVENTTYPE eventID,
    void* lpEventData)
{
    MerakCamera* self = static_cast<MerakCamera*>(lpCallbackData);

    if (self == nullptr)
    {
        return;
    }

    switch (eventID)
    {
    case NV_EVENTTYPE_ACQ:
        // Step17 will convert NV_ImageInfo to Frame here.
        // Do not process image data in Step16.
        break;

    case NV_EVENTTYPE_SYSTEMSTATUS:
    {
        auto* status = static_cast<NV_EVENTDATA_STATUS*>(lpEventData);

        if (status != nullptr)
        {
            // Avoid too much logging. Only keep this for initial debugging if needed.
            // logLine("[MerakCamera] status event: conn=", (int)status->_cConn,
            //         ", open=", (int)status->_cOpen,
            //         ", acq=", (int)status->_cAcq);
        }

        break;
    }

    case NV_EVENTTYPE_CONNBREAK:
        logLine("[MerakCamera] event: detector connection break.");
        break;

    case NV_EVENTTYPE_MAXFRAME:
        logLine("[MerakCamera] event: max frame reached.");
        break;

    default:
        break;
    }
}

bool MerakCamera::open()
{
    logLine("[MerakCamera] opening detector by NV_OpenDet(DEFAULT_DETID)...");

    NV_DET arrDet[8] = {};
    int detCount = 0;

    NV_StatusType ret = NV_EnumDets(arrDet, 8, &detCount);

    if (!checkSdkStatus(ret, "NV_EnumDets"))
    {
        opened_ = false;
        return false;
    }

    if (detCount <= 0)
    {
        logLine("[MerakCamera] no detector found.");
        opened_ = false;
        return false;
    }

    logLine("[MerakCamera] detector count = ", detCount);
    logLine("[MerakCamera] opening first detector, serial = ", arrDet[0].SerialNumber);

    ret = NV_OpenDet(arrDet[0]._uDetID);

    if (!checkSdkStatus(ret, "NV_OpenDet"))
    {
        opened_ = false;
        return false;
    }

    opened_ = true;

    char serialBuffer[128] = { 0 };
    unsigned long serialBufferSize = static_cast<unsigned long>(sizeof(serialBuffer));

    ret = NV_GetSerialNum(serialBuffer, &serialBufferSize);

    if (ret == NV_SC_SUCCESS)
    {
        serialNumber_ = serialBuffer;
        logLine("[MerakCamera] serial number: ", serialNumber_);
    }
    else
    {
        checkSdkStatus(ret, "NV_GetSerialNum");
    }

    ret = NV_GetSensorSize(&sensorWidth_, &sensorHeight_, &sensorBits_);

    if (!checkSdkStatus(ret, "NV_GetSensorSize"))
    {
        close();
        return false;
    }

    logLine("[MerakCamera] sensor size: ",
        sensorWidth_,
        " x ",
        sensorHeight_,
        ", bits = ",
        sensorBits_);

    ret = NV_SetEventCallback(&MerakCamera::sdkEventCallback, this);

    if (!checkSdkStatus(ret, "NV_SetEventCallback"))
    {
        close();
        return false;
    }

    logLine("[MerakCamera] open finished.");
    return true;
}

bool MerakCamera::configure(const AcquisitionConfig& config)
{
    if (!opened_)
    {
        logLine("[MerakCamera] configure failed: detector is not opened.");
        return false;
    }

    config_ = config;

    logLine("[MerakCamera] configuring detector...");

    NV_StatusType ret = NV_SC_SUCCESS;

    NV_AcquisitionMode sdkAcqMode = NV_CONTINUE;

    switch (config_.trigger)
    {
    case TriggerMode::RisingEdge:
        sdkAcqMode = NV_RISING_EDGE_TRIGGER;
        break;

    case TriggerMode::FallingEdge:
        sdkAcqMode = NV_FALLING_EDGE_TRIGGER;
        break;

    case TriggerMode::LowLevel:
        sdkAcqMode = NV_LOW_LEVEL_TRIGGER;
        break;

    case TriggerMode::HighLevel:
        sdkAcqMode = NV_HIGH_LEVEL_TRIGGER;
        break;

    case TriggerMode::Snapshot:
        sdkAcqMode = NV_SNAP_SHOT_TRIGGER;
        break;

    case TriggerMode::Continuous:
    default:
        sdkAcqMode = NV_CONTINUE;
        break;
    }

    ret = NV_SetAcquisitionMode(sdkAcqMode);

    if (!checkSdkStatus(ret, "NV_SetAcquisitionMode"))
    {
        return false;
    }

    NV_BinningMode sdkBinning = NV_BINNING_1X1;

    switch (config_.binning)
    {
    case BinningMode::Bin2x2:
        sdkBinning = NV_BINNING_2X2;
        break;

    case BinningMode::Bin3x3:
        sdkBinning = NV_BINNING_3X3;
        break;

    case BinningMode::Bin4x4:
        sdkBinning = NV_BINNING_4X4;
        break;

    case BinningMode::Bin1x1:
    default:
        sdkBinning = NV_BINNING_1X1;
        break;
    }

    ret = NV_SetBinningMode(sdkBinning);

    if (!checkSdkStatus(ret, "NV_SetBinningMode"))
    {
        return false;
    }

    int gainIndex = static_cast<int>(std::round(config_.gain));

    if (gainIndex < 0)
    {
        gainIndex = 0;
    }

    ret = NV_SetGainEx(gainIndex);

    if (!checkSdkStatus(ret, "NV_SetGainEx"))
    {
        return false;
    }

    int roiX = config_.roi.x;
    int roiY = config_.roi.y;
    int roiW = config_.roi.width;
    int roiH = config_.roi.height;

    if (roiW <= 0 || roiW > sensorWidth_)
    {
        roiW = sensorWidth_;
    }

    if (roiH <= 0 || roiH > sensorHeight_)
    {
        roiH = sensorHeight_;
    }

    if (roiX < 0)
    {
        roiX = 0;
    }

    if (roiY < 0)
    {
        roiY = 0;
    }

    if (roiX + roiW > sensorWidth_)
    {
        roiX = 0;
        roiW = sensorWidth_;
    }

    if (roiY + roiH > sensorHeight_)
    {
        roiY = 0;
        roiH = sensorHeight_;
    }

    logLine("[MerakCamera] trying ROI: ",
        roiX,
        ", ",
        roiY,
        ", ",
        roiW,
        " x ",
        roiH,
        ", sensor = ",
        sensorWidth_,
        " x ",
        sensorHeight_);

    ret = NV_SetImageRange(roiX, roiY, roiW, roiH);

    if (!checkSdkStatus(ret, "NV_SetImageRange"))
    {
        return false;
    }

    config_.roi.x = roiX;
    config_.roi.y = roiY;
    config_.roi.width = roiW;
    config_.roi.height = roiH;

    int speedParam = 30;

    if (config_.speedControlMode == SpeedControlMode::ExposureTime)
    {
        ret = NV_SetAcqSpeedCtrlMode(NV_ACQSPEED_CTRL_EXPTIME);

        if (!checkSdkStatus(ret, "NV_SetAcqSpeedCtrlMode"))
        {
            return false;
        }

        // SDK exposure-time unit is 0.1 ms.
        // Example: 10 ms -> 100.
        speedParam = static_cast<int>(std::round(config_.exposureMs * 10.0));

        if (speedParam <= 0)
        {
            speedParam = 100;
        }

        ret = NV_SetAcqSpeedCtrlParam(speedParam);

        if (!checkSdkStatus(ret, "NV_SetAcqSpeedCtrlParam"))
        {
            return false;
        }

        logLine("[MerakCamera] speed control: exposure time, param = ",
            speedParam,
            " (0.1 ms unit)");
    }
    else
    {
        ret = NV_SetAcqSpeedCtrlMode(NV_ACQSPEED_CTRL_FPS);

        if (!checkSdkStatus(ret, "NV_SetAcqSpeedCtrlMode"))
        {
            return false;
        }

        int fps = static_cast<int>(std::round(config_.fps));

        if (fps <= 0)
        {
            fps = 30;
        }

        speedParam = fps;

        ret = NV_SetAcqSpeedCtrlParam(speedParam);

        if (!checkSdkStatus(ret, "NV_SetAcqSpeedCtrlParam"))
        {
            return false;
        }

        logLine("[MerakCamera] speed control: FPS, target fps = ", speedParam);
    }

    logLine("[MerakCamera] configure finished.");
    logLine("  ROI: ", config_.roi.x, ", ", config_.roi.y, ", ",
        config_.roi.width, " x ", config_.roi.height);
    logLine("  ROI: ", config_.roi.x, ", ", config_.roi.y, ", ",
        config_.roi.width, " x ", config_.roi.height);
    logLine("  Target FPS: ", config_.fps);
    logLine("  Exposure ms: ", config_.exposureMs);
    logLine("  Gain index: ", gainIndex);

    return true;
}

bool MerakCamera::convertImageInfoToFrame(const NV_ImageInfo& imageInfo, Frame& frame)
{
    if (imageInfo.pImageBuffer == nullptr)
    {
        logLine("[MerakCamera] image buffer is null.");
        return false;
    }

    if (imageInfo.iSizeX == 0 || imageInfo.iSizeY == 0)
    {
        logLine("[MerakCamera] invalid image size: ",
            imageInfo.iSizeX,
            " x ",
            imageInfo.iSizeY);
        return false;
    }

    const int width = static_cast<int>(imageInfo.iSizeX);
    const int height = static_cast<int>(imageInfo.iSizeY);
    const size_t pixelCount = static_cast<size_t>(width) * height;

    frame.frameId = frameId_++;
    frame.width = width;
    frame.height = height;
    frame.timestamp = std::chrono::steady_clock::now();

    if (imageInfo.iPixelType == NV_PF_Mono16 ||
        imageInfo.iPixelType == NV_PF_Mono14)
    {
        frame.bitDepth = (imageInfo.iPixelType == NV_PF_Mono14) ? 14 : 16;
        frame.pixels.resize(pixelCount);

        const size_t expectedBytes = pixelCount * sizeof(uint16_t);

        if (imageInfo.iImageSize < expectedBytes)
        {
            logLine("[MerakCamera] image size is smaller than expected. imageSize = ",
                imageInfo.iImageSize,
                ", expected = ",
                expectedBytes);
            return false;
        }

        std::memcpy(
            frame.pixels.data(),
            imageInfo.pImageBuffer,
            expectedBytes
        );

        return true;
    }

    if (imageInfo.iPixelType == NV_PF_Mono8)
    {
        frame.bitDepth = 8;
        frame.pixels.resize(pixelCount);

        if (imageInfo.iImageSize < pixelCount)
        {
            logLine("[MerakCamera] Mono8 image size is smaller than expected.");
            return false;
        }

        const uint8_t* src = reinterpret_cast<const uint8_t*>(imageInfo.pImageBuffer);

        for (size_t i = 0; i < pixelCount; ++i)
        {
            // Convert 8-bit to 16-bit container for existing pipeline.
            frame.pixels[i] = static_cast<uint16_t>(src[i]) << 8;
        }

        return true;
    }

    logLine("[MerakCamera] unsupported pixel format: ", imageInfo.iPixelType);
    return false;
}

bool MerakCamera::start()
{
    if (!opened_)
    {
        logLine("[MerakCamera] start failed: detector is not opened.");
        return false;
    }

    if (running_)
    {
        logLine("[MerakCamera] start ignored: already running.");
        return true;
    }

    frameId_ = 0;

    if (!bufferCreated_)
    {
        // SDK internal acquisition buffer.
        // Official AcqWithBuffer example uses 30 frames.
        NV_StatusType ret = NV_AcqBufferCreate(30);

        if (!checkSdkStatus(ret, "NV_AcqBufferCreate"))
        {
            return false;
        }

        bufferCreated_ = true;
    }

    NV_StatusType ret = NV_StartAcq();

    if (!checkSdkStatus(ret, "NV_StartAcq"))
    {
        if (bufferCreated_)
        {
            NV_AcqBufferDestroy();
            bufferCreated_ = false;
        }

        running_ = false;
        return false;
    }

    running_ = true;

    logLine("[MerakCamera] acquisition started.");
    return true;
}

bool MerakCamera::getFrame(Frame& frame)
{
    if (!running_ || !bufferCreated_)
    {
        return false;
    }

    NV_ImageInfo imageInfo{};

    // Wait up to 100 ms for one image.
    NV_StatusType ret = NV_AcqBufferReadImage(&imageInfo, 100);

    if (ret == NV_SC_SUCCESS)
    {
        return convertImageInfoToFrame(imageInfo, frame);
    }

    if (ret == NV_SC_TIMEOUT || ret == NV_SC_NO_DATA)
    {
        return false;
    }

    logLine("[MerakCamera] NV_AcqBufferReadImage failed. ret = ", ret);

    std::string err = getLastSdkErrorText();

    if (!err.empty())
    {
        logLine("[MerakCamera] SDK last error: ", err);
    }

    return false;
}

void MerakCamera::stop()
{
    if (running_)
    {
        logLine("[MerakCamera] stopping acquisition by NV_StopAcq...");

        NV_StatusType ret = NV_StopAcq();
        checkSdkStatus(ret, "NV_StopAcq");
    }

    running_ = false;

    if (bufferCreated_)
    {
        logLine("[MerakCamera] destroying acquisition buffer...");

        NV_StatusType ret = NV_AcqBufferDestroy();
        checkSdkStatus(ret, "NV_AcqBufferDestroy");

        bufferCreated_ = false;
    }
}

void MerakCamera::close()
{
    if (!opened_)
    {
        return;
    }

    stop();

    NV_SetEventCallback(nullptr, nullptr);

    logLine("[MerakCamera] closing detector by NV_CloseDet...");

    NV_StatusType ret = NV_CloseDet();
    checkSdkStatus(ret, "NV_CloseDet");

    opened_ = false;
    running_ = false;
}