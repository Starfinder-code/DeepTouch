#pragma once

#include <atomic>
#include <string>

#include "AcquisitionTask.h"

void httpServerLoop(
    AcquisitionTask& task,
    std::atomic<bool>& running,
    int port,
    const std::string& outputBaseDir
);