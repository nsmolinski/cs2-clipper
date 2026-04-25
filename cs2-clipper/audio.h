#pragma once

#include <atomic>
#include <string>

struct CaptureTiming
{
    std::atomic<long long> firstAudioQpc{ -1 };
    std::atomic<long long> lastAudioQpc{ -1 };
    std::atomic<long long> firstVideoQpc{ -1 };
    std::atomic<long long> lastVideoQpc{ -1 };
};

bool CaptureAudioLoop(
    const std::string& outputPath,
    std::atomic<bool>& runningFlag,
    CaptureTiming& timing,
    int bufferSeconds
);
