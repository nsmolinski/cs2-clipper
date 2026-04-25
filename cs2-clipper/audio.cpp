#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <iostream>
#include <fstream>
#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

#include "audio.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

struct WAVHeader
{
    char riff[4] = { 'R','I','F','F' };
    uint32_t chunkSize;
    char wave[4] = { 'W','A','V','E' };

    char fmt[4] = { 'f','m','t',' ' };
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 3; // FLOAT
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;

    char data[4] = { 'd','a','t','a' };
    uint32_t dataSize;
};

bool CaptureAudioLoop(
    const std::string& outputPath,
    std::atomic<bool>& runningFlag,
    CaptureTiming& timing,
    int bufferSeconds,
    long long qpcFrequency
)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInitCom = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        std::cout << "Audio: can't create MMDeviceEnumerator\n";
        if (didInitCom) CoUninitialize();
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        std::cout << "Audio: no audio device\n";
        enumerator->Release();
        if (didInitCom) CoUninitialize();
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr) || !client) {
        std::cout << "Audio: can't activate IAudioClient\n";
        device->Release();
        enumerator->Release();
        if (didInitCom) CoUninitialize();
        return false;
    }

    WAVEFORMATEX* format = nullptr;
    hr = client->GetMixFormat(&format);
    if (FAILED(hr) || !format) {
        std::cout << "Audio: GetMixFormat failed\n";
        client->Release();
        device->Release();
        enumerator->Release();
        if (didInitCom) CoUninitialize();
        return false;
    }

    std::cout << "=== AUDIO FORMAT ===\n";
    std::cout << "Channels: " << format->nChannels << "\n";
    std::cout << "SampleRate: " << format->nSamplesPerSec << "\n";
    std::cout << "Bits: " << format->wBitsPerSample << "\n\n";

    hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        10000000,
        0,
        format,
        nullptr
    );
    if (FAILED(hr)) {
        std::cout << "Audio: Initialize loopback failed\n";
        CoTaskMemFree(format);
        client->Release();
        device->Release();
        enumerator->Release();
        if (didInitCom) CoUninitialize();
        return false;
    }

    hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    if (FAILED(hr) || !capture) {
        std::cout << "Audio: GetService(IAudioCaptureClient) failed\n";
        CoTaskMemFree(format);
        client->Release();
        device->Release();
        enumerator->Release();
        if (didInitCom) CoUninitialize();
        return false;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        std::cout << "Audio: Start failed\n";
        capture->Release();
        CoTaskMemFree(format);
        client->Release();
        device->Release();
        enumerator->Release();
        if (didInitCom) CoUninitialize();
        return false;
    }

    WAVHeader header{};
    header.channels = format->nChannels;
    header.sampleRate = format->nSamplesPerSec;
    header.bitsPerSample = format->wBitsPerSample;
    header.blockAlign = header.channels * header.bitsPerSample / 8;
    header.byteRate = header.sampleRate * header.blockAlign;
    header.dataSize = 0;

    if (bufferSeconds <= 0) {
        bufferSeconds = 15;
    }

    const size_t ringCapacityBytes = static_cast<size_t>(header.byteRate) * static_cast<size_t>(bufferSeconds);
    std::vector<char> ringBuffer(ringCapacityBytes);
    size_t writePos = 0;
    size_t bufferedBytes = 0;

    std::cout << "Audio: buffering last " << bufferSeconds << " seconds...\n";

    while (runningFlag)
    {
        UINT32 packetSize = 0;
        capture->GetNextPacketSize(&packetSize);

        while (packetSize > 0)
        {
            BYTE* data;
            UINT32 frames;
            DWORD flags;

            UINT64 devicePosition = 0;
            UINT64 qpcPosition100ns = 0;
            capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition100ns);

            uint32_t bytes = frames * header.blockAlign;
            if (bytes > 0 && !ringBuffer.empty()) {
                const char* src = reinterpret_cast<const char*>(data);
                size_t bytesLeft = bytes;
                while (bytesLeft > 0) {
                    const size_t tailSpace = ringCapacityBytes - writePos;
                    const size_t chunk = (std::min)(bytesLeft, tailSpace);
                    memcpy(ringBuffer.data() + writePos, src, chunk);
                    writePos = (writePos + chunk) % ringCapacityBytes;
                    src += chunk;
                    bytesLeft -= chunk;
                }
                bufferedBytes = (std::min)(bufferedBytes + static_cast<size_t>(bytes), ringCapacityBytes);
            }
            if (bytes > 0) {
                long long packetStartQpc = -1;
                if (qpcFrequency > 0 && qpcPosition100ns > 0) {
                    packetStartQpc = static_cast<long long>(
                        (qpcPosition100ns * static_cast<unsigned long long>(qpcFrequency) + 5000000ULL) / 10000000ULL
                        );
                }

                long long packetEndQpc = packetStartQpc;
                if (packetStartQpc >= 0 && header.sampleRate > 0 && frames > 0) {
                    const long long packetDurationQpc =
                        static_cast<long long>((static_cast<long double>(frames) * static_cast<long double>(qpcFrequency))
                            / static_cast<long double>(header.sampleRate));
                    packetEndQpc = packetStartQpc + packetDurationQpc;
                }

                LARGE_INTEGER qpcNow{};
                QueryPerformanceCounter(&qpcNow);
                const long long effectiveStartQpc = (packetStartQpc >= 0) ? packetStartQpc : qpcNow.QuadPart;
                const long long effectiveEndQpc = (packetEndQpc >= 0) ? packetEndQpc : qpcNow.QuadPart;

                if (timing.firstAudioQpc.load(std::memory_order_relaxed) < 0) {
                    timing.firstAudioQpc.store(effectiveStartQpc, std::memory_order_relaxed);
                }
                timing.lastAudioQpc.store(effectiveEndQpc, std::memory_order_relaxed);
            }
            capture->ReleaseBuffer(frames);
            capture->GetNextPacketSize(&packetSize);
        }

        Sleep(2);
    }

    std::cout << "Audio: stop\n";

    client->Stop();

    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        std::cout << "Audio: can't save file: " << outputPath << "\n";
        capture->Release();
        client->Release();
        device->Release();
        enumerator->Release();
        CoTaskMemFree(format);
        if (didInitCom) CoUninitialize();
        return false;
    }

    header.dataSize = static_cast<uint32_t>(bufferedBytes);
    header.chunkSize = 36 + header.dataSize;
    file.write(reinterpret_cast<char*>(&header), sizeof(header));

    if (bufferedBytes > 0 && !ringBuffer.empty()) {
        const size_t readStart = (writePos + ringCapacityBytes - bufferedBytes) % ringCapacityBytes;
        const size_t firstPart = (std::min)(bufferedBytes, ringCapacityBytes - readStart);
        file.write(ringBuffer.data() + readStart, firstPart);
        if (bufferedBytes > firstPart) {
            file.write(ringBuffer.data(), bufferedBytes - firstPart);
        }
    }

    capture->Release();
    client->Release();
    device->Release();
    enumerator->Release();

    CoTaskMemFree(format);
    if (didInitCom) CoUninitialize();

    std::cout << "Audio saved: " << outputPath << "\n";
    return true;
}

bool GetLoopbackMixFormat(
    int& sampleRate,
    int& channels,
    int& bitsPerSample,
    bool& isFloat
)
{
    sampleRate = 0;
    channels = 0;
    bitsPerSample = 0;
    isFloat = false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInitCom = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    WAVEFORMATEX* format = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) goto cleanup;

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) goto cleanup;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr) || !client) goto cleanup;

    hr = client->GetMixFormat(&format);
    if (FAILED(hr) || !format) goto cleanup;

    sampleRate = static_cast<int>(format->nSamplesPerSec);
    channels = static_cast<int>(format->nChannels);
    bitsPerSample = static_cast<int>(format->wBitsPerSample);
    isFloat = (format->wBitsPerSample == 32);

cleanup:
    if (format) CoTaskMemFree(format);
    if (client) client->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (didInitCom) CoUninitialize();

    return (sampleRate > 0 && channels > 0 && bitsPerSample > 0);
}

bool CaptureAudioToPipeLoop(
    const std::string& pipePath,
    std::atomic<bool>& runningFlag,
    CaptureTiming& timing,
    long long qpcFrequency
)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInitCom = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;
    HANDLE pipeHandle = INVALID_HANDLE_VALUE;

    pipeHandle = CreateNamedPipeA(
        pipePath.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,
        1 << 20,
        1 << 20,
        0,
        nullptr
    );
    if (pipeHandle == INVALID_HANDLE_VALUE) {
        std::cout << "Audio pipe create failed\n";
        if (didInitCom) CoUninitialize();
        return false;
    }

    if (!ConnectNamedPipe(pipeHandle, nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            std::cout << "Audio pipe connect failed\n";
            CloseHandle(pipeHandle);
            if (didInitCom) CoUninitialize();
            return false;
        }
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) goto cleanup;

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) goto cleanup;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr) || !client) goto cleanup;

    hr = client->GetMixFormat(&format);
    if (FAILED(hr) || !format) goto cleanup;

    hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        10000000,
        0,
        format,
        nullptr
    );
    if (FAILED(hr)) goto cleanup;

    hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    if (FAILED(hr) || !capture) goto cleanup;

    hr = client->Start();
    if (FAILED(hr)) goto cleanup;

    while (runningFlag)
    {
        UINT32 packetSize = 0;
        capture->GetNextPacketSize(&packetSize);

        while (packetSize > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition100ns = 0;

            capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition100ns);

            const UINT32 bytes = frames * format->nBlockAlign;
            if (bytes > 0 && data != nullptr) {
                DWORD written = 0;
                WriteFile(pipeHandle, data, bytes, &written, nullptr);

                long long packetStartQpc = -1;
                if (qpcFrequency > 0 && qpcPosition100ns > 0) {
                    packetStartQpc = static_cast<long long>(
                        (qpcPosition100ns * static_cast<unsigned long long>(qpcFrequency) + 5000000ULL) / 10000000ULL
                        );
                }

                long long packetEndQpc = packetStartQpc;
                if (packetStartQpc >= 0 && format->nSamplesPerSec > 0 && frames > 0) {
                    const long long packetDurationQpc =
                        static_cast<long long>((static_cast<long double>(frames) * static_cast<long double>(qpcFrequency))
                            / static_cast<long double>(format->nSamplesPerSec));
                    packetEndQpc = packetStartQpc + packetDurationQpc;
                }

                LARGE_INTEGER qpcNow{};
                QueryPerformanceCounter(&qpcNow);
                const long long effectiveStartQpc = (packetStartQpc >= 0) ? packetStartQpc : qpcNow.QuadPart;
                const long long effectiveEndQpc = (packetEndQpc >= 0) ? packetEndQpc : qpcNow.QuadPart;

                if (timing.firstAudioQpc.load(std::memory_order_relaxed) < 0) {
                    timing.firstAudioQpc.store(effectiveStartQpc, std::memory_order_relaxed);
                }
                timing.lastAudioQpc.store(effectiveEndQpc, std::memory_order_relaxed);
            }

            capture->ReleaseBuffer(frames);
            capture->GetNextPacketSize(&packetSize);
        }

        Sleep(2);
    }

cleanup:
    if (client) client->Stop();
    if (capture) capture->Release();
    if (client) client->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (format) CoTaskMemFree(format);
    if (pipeHandle != INVALID_HANDLE_VALUE) CloseHandle(pipeHandle);
    if (didInitCom) CoUninitialize();

    return true;
}