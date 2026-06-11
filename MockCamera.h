#pragma once

#include <cstdint>

#include "ICamera.h"

class MockCamera : public ICamera
{
public:
    bool open() override;
    bool configure(const AcquisitionConfig& config) override;
    bool start() override;
    bool getFrame(Frame& frame) override;
    void stop() override;
    void close() override;

private:
    bool opened_ = false;
    bool running_ = false;
    uint64_t frameId_ = 0;
    AcquisitionConfig config_;
};