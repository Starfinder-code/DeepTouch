#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "CoreTypes.h"

std::vector<uint8_t> convert16To8AutoWindow(const Frame& frame);
std::vector<uint8_t> convertFrameTo8FixedLinear(const Frame& frame);

bool saveGray8AsBmp(
    const std::vector<uint8_t>& gray8,
    int width,
    int height,
    const std::filesystem::path& filePath
);