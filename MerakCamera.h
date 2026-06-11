#pragma once

#include <cstdint>
#include <string>

#include "ICamera.h"

// Forward declarations from MerakSDK headers
enum _NV_CALLBACKTYPE;
typedef enum _NV_CALLBACKTYPE NV_EVENTTYPE;

struct _NV_ImageInfo;
typedef struct _NV_ImageInfo NV_ImageInfo;

class MerakCamera : public ICamera
{
public:
    MerakCamera();
    ~MerakCamera() override;

    bool open() override;
    bool configure(const AcquisitionConfig& config) override;
    bool start() override;
    bool getFrame(Frame& frame) override;
    void stop() override;
    void close() override;

private:
    bool opened_ = false;
    bool running_ = false;
    bool bufferCreated_ = false;

    uint64_t frameId_ = 0;

    int sensorWidth_ = 0;
    int sensorHeight_ = 0;
    int sensorBits_ = 0;

    std::string serialNumber_;

    AcquisitionConfig config_;

private:
    std::string getLastSdkErrorText();
    bool checkSdkStatus(int ret, const char* apiName);

    bool convertImageInfoToFrame(const NV_ImageInfo& imageInfo, Frame& frame);

    static void sdkEventCallback(
        void* lpCallbackData,
        NV_EVENTTYPE eventID,
        void* lpEventData
    );
};