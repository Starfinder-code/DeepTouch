#include <atomic>
#include <iostream>
#include <thread>

#include "AcquisitionTask.h"
#include "HttpServer.h"
#include "Log.h"

int main()
{
    logLine("MLAcqCore Step13: Web control start/stop test");

    CameraBackend backend = CameraBackend::Mock;
    AcquisitionTask task(backend);

    const std::string outputBaseDir =
        R"(C:\Users\11157\Desktop\FileBrowser)";

    std::atomic<bool> httpRunning = true;

    std::thread httpThread(
        httpServerLoop,
        std::ref(task),
        std::ref(httpRunning),
        8081,
        outputBaseDir
    );

    logLine("Open browser: http://127.0.0.1:8081/");
    logLine("Output base directory: ", outputBaseDir);
    logLine("Use web page buttons to start/stop acquisition.");
    logLine("Press ENTER in this console to exit program.");

    std::cin.get();

    if (task.isRunning())
    {
        task.stopAndWait();
    }

    httpRunning = false;

    if (httpThread.joinable())
    {
        httpThread.join();
    }

    logLine("Final result:");
    logLine("  raw frames saved          = ", task.getSavedCount());
    logLine("  preview frames processed = ", task.getPreviewSavedCount());
    logLine("  save directory            = ", task.getSaveDir());

    logLine("Step13 finished.");

    return 0;
}