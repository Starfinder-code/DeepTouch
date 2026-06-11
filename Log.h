#pragma once

#include <iostream>
#include <mutex>

inline std::mutex g_logMutex;

template <typename... Args>
void logLine(Args&&... args)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    ((std::cout << args), ...);
    std::cout << std::endl;
}