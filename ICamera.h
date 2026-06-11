#pragma once

#include "CoreTypes.h"

class ICamera
{
public:
    virtual ~ICamera() = default;

    virtual bool open() = 0;
    virtual bool configure(const AcquisitionConfig& config) = 0;
    virtual bool start() = 0;
    virtual bool getFrame(Frame& frame) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
};