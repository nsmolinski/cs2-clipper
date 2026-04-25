#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <cstdlib>
#include <cmath>

#include "audio.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
  
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
IDXGIOutputDuplication* duplication = nullptr;
ID3D11Texture2D* staging = nullptr;

HANDLE ffmpegIn = nullptr;
PROCESS_INFORMATION ffmpegProc{};

int WIDTH = 0, HEIGHT = 0;
std::atomic<bool> running{ true };
std::atomic<bool> audioRunning{ false };
std::thread audioThread;

std::string ffmpegPath;
std::string tempSessionPath;
std::string segmentDirPath;
std::string finalPath;
std::string concatListPath;
std::string audioPipePath;
std::string audioSampleFmt;
int audioSampleRate = 48000;
int audioChannels = 2;
std::string sessionId;
bool videoStarted = false;
bool audioStarted = false;
CaptureTiming timing;
LARGE_INTEGER qpcFrequency{};
LARGE_INTEGER sessionStartQpc{};
constexpr int kDefaultReplayBufferSeconds = 15;
constexpr int kCaptureStartDelayMs = 1000;

HWND cs2Window = nullptr;
RECT cs2Rect{};
int captureX = 0, captureY = 0;
int captureWidth = 0, captureHeight = 0;

uint64_t lastPresentTime = 0;
int replayBufferSeconds = kDefaultReplayBufferSeconds;

int GetReplayBufferSeconds()
{
    char* envValue = nullptr;
    size_t envLen = 0;
    errno_t envResult = _dupenv_s(&envValue, &envLen, "CS2_CLIP_BUFFER_SECONDS");
    if (envResult != 0 || envValue == nullptr || envLen == 0) {
        return kDefaultReplayBufferSeconds;
    }

    const int parsed = std::atoi(envValue);
    free(envValue);

    if (parsed == 15 || parsed == 30) {
        return parsed;
    }

    std::cout << "Unsupported CS2_CLIP_BUFFER_SECONDS=" << parsed
        << " (allowed: 15 or 30). Falling back to 15.\n";
    return kDefaultReplayBufferSeconds;
}

std::string FindFFmpeg()
{
    auto path = std::filesystem::current_path() / "ffmpeg" / "bin" / "ffmpeg.exe";
    std::cout << "Searching for FFmpeg: " << path << std::endl;
    return std::filesystem::exists(path) ? path.string() : "";
}

std::string BuildSessionId()
{
    return std::to_string(std::time(nullptr));
}

bool BuildResolvedConcatList(const std::string& sourcePath, const std::string& targetPath)
{
    std::ifstream in(sourcePath);
    if (!in.is_open()) {
        return false;
    }

    std::ofstream out(targetPath, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << "ffconcat version 1.0\n";

    std::string line;
    int writtenFiles = 0;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        if (line.rfind("ffconcat version", 0) == 0) {
            continue;
        }
        if (line.rfind("file ", 0) != 0) {
            continue;
        }

        std::string entryPath = line.substr(5);
        if (!entryPath.empty() && entryPath.front() == '\'' && entryPath.back() == '\'' && entryPath.size() >= 2) {
            entryPath = entryPath.substr(1, entryPath.size() - 2);
        }

        std::filesystem::path p(entryPath);
        if (p.is_relative()) {
            p = std::filesystem::path(segmentDirPath) / p;
        }

        out << "file '" << p.generic_string() << "'\n";
        writtenFiles++;
    }

    return writtenFiles > 0;
}

bool RunCommand(const std::string& command, DWORD& exitCode)
{
    std::string wrapped = "cmd.exe /C \"" + command + "\"";

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::vector<char> buf(wrapped.begin(), wrapped.end());
    buf.push_back('\0');

    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::cout << "CreateProcess failed, error=" << GetLastError() << "\n";
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void PrepareOutputPaths()
{
    std::filesystem::create_directories("C:\\CS2Recordings");
    sessionId = BuildSessionId();
    const std::filesystem::path tempBase = std::filesystem::temp_directory_path() / ("cs2-clipper-" + sessionId);
    tempSessionPath = tempBase.string();
    std::filesystem::create_directories(tempBase);
    segmentDirPath = (tempBase / "segments").string();
    finalPath = "C:\\CS2Recordings\\cs2_recording_" + sessionId + ".mp4";
    concatListPath = (tempBase / "segments.ffconcat").string();
    audioPipePath = "\\\\.\\pipe\\cs2-clipper-audio-" + sessionId;
    std::filesystem::create_directories(segmentDirPath);
}

void StartFFmpeg()
{
    ffmpegPath = FindFFmpeg();
    if (ffmpegPath.empty()) {
        std::cout << "FFmpeg not found\n";
        return;
    }

    int bitsPerSample = 0;
    bool isFloat = false;
    if (!GetLoopbackMixFormat(audioSampleRate, audioChannels, bitsPerSample, isFloat)) {
        std::cout << "Failed to get loopback audio format\n";
        return;
    }
    if (isFloat && bitsPerSample == 32) {
        audioSampleFmt = "f32le";
    }
    else if (bitsPerSample == 16) {
        audioSampleFmt = "s16le";
    }
    else if (bitsPerSample == 32) {
        audioSampleFmt = "s32le";
    }
    else {
        std::cout << "Unsupported audio format (" << bitsPerSample << " bits)\n";
        return;
    }

    const int segmentWrapCount = replayBufferSeconds + 3;
    const std::string segmentPattern = segmentDirPath + "\\seg_%03d.ts";

    std::string cmd =
        "\"" + ffmpegPath + "\" -y "
        "-f rawvideo -pix_fmt bgra "
        "-s " + std::to_string(WIDTH) + "x" + std::to_string(HEIGHT) + " "
        "-r 60 -i - "
        "-f " + audioSampleFmt + " "
        "-ar " + std::to_string(audioSampleRate) + " "
        "-ac " + std::to_string(audioChannels) + " "
        "-i \"" + audioPipePath + "\" "
        "-vf crop=" +
        std::to_string(captureWidth) + ":" +
        std::to_string(captureHeight) + ":" +
        std::to_string(captureX) + ":" +
        std::to_string(captureY) + " "
        "-c:v h264_nvenc "
        "-preset p5 "
        "-tune ll "
        "-rc cbr "
        "-b:v 20M "
        "-g 60 "
        "-keyint_min 60 "
        "-sc_threshold 0 "
        "-force_key_frames \"expr:gte(t,n_forced*1)\" "
        "-pix_fmt yuv420p "
        "-c:a aac -b:a 192k "
        "-f segment -segment_time 1 -segment_wrap " + std::to_string(segmentWrapCount) + " "
        "-segment_list \"" + concatListPath + "\" "
        "-segment_list_type ffconcat "
        "-segment_list_size " + std::to_string(replayBufferSeconds) + " "
        "-segment_list_flags +live "
        "-reset_timestamps 1 \"" + segmentPattern + "\"";

    std::cout << "Launching FFmpeg segmented replay buffer...\n";

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE r = nullptr, w = nullptr;
    CreatePipe(&r, &w, &sa, 0);
    SetHandleInformation(w, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = r;

    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &ffmpegProc);
    CloseHandle(r);
    ffmpegIn = w;
    videoStarted = true;
}

void StartAudio()
{
    if (audioRunning) return;
    audioRunning = true;
    audioStarted = true;
    audioThread = std::thread([] {
        CaptureAudioToPipeLoop(audioPipePath, audioRunning, timing, qpcFrequency.QuadPart);
    });
}

void StopAudio()
{
    if (!audioRunning && !audioThread.joinable()) return;

    audioRunning = false;
    if (audioThread.joinable()) {
        audioThread.join();
    }
}

void MergeAudioVideo()
{
    if (ffmpegPath.empty()) return;
    if (!std::filesystem::exists(concatListPath)) {
        std::cout << "Skipping merging - missing segment list.\n";
        return;
    }
    std::cout << "Merging replay buffer (audio+video from same FFmpeg timeline)\n";
    const std::string resolvedConcatListPath =
        (std::filesystem::path(tempSessionPath) / "segments_resolved.ffconcat").string();
    if (!BuildResolvedConcatList(concatListPath, resolvedConcatListPath)) {
        std::cout << "Failed to build resolved concat list\n";
        return;
    }

    std::string finalizeCmd =
        "\"" + ffmpegPath + "\" -y "
        "-f concat -safe 0 -i \"" + resolvedConcatListPath + "\" "
        "-fflags +genpts -avoid_negative_ts make_zero "
        "-c copy "
        "\"" + finalPath + "\"";

    DWORD code = 1;
    if (!RunCommand(finalizeCmd, code)) {
        std::cout << "Finalizing clip failed to start\n";
        return;
    }

    if (code == 0 && std::filesystem::exists(finalPath)) {
        std::cout << "Clip finalized: " << finalPath << "\n";
        std::error_code ec;
        if (std::filesystem::exists(segmentDirPath)) {
            for (const auto& entry : std::filesystem::directory_iterator(segmentDirPath)) {
                std::filesystem::remove(entry.path(), ec);
            }
            std::filesystem::remove(segmentDirPath, ec);
        }
        std::filesystem::remove(concatListPath, ec);
        std::filesystem::remove(resolvedConcatListPath, ec);
        if (!tempSessionPath.empty()) {
            std::filesystem::remove_all(tempSessionPath, ec);
        }
    }
    else {
        std::cout << "Finalizing failed (code " << code << "). Temporary files left.\n";
    }

    videoStarted = false;
    audioStarted = false;
    timing.firstAudioQpc.store(-1, std::memory_order_relaxed);
    timing.lastAudioQpc.store(-1, std::memory_order_relaxed);
    timing.firstVideoQpc.store(-1, std::memory_order_relaxed);
    timing.lastVideoQpc.store(-1, std::memory_order_relaxed);
}

bool InitDXGI()
{
    if (duplication) { duplication->Release(); duplication = nullptr; }
    if (staging) { staging->Release(); staging = nullptr; }
    if (context) { context->Release(); context = nullptr; }
    if (device) { device->Release(); device = nullptr; }

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context);

    if (FAILED(hr)) {
        std::cout << "D3D11CreateDevice failed\n";
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);

    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();

    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(0, &output);
    adapter->Release();

    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();

    hr = output1->DuplicateOutput(device, &duplication);
    output1->Release();

    if (FAILED(hr)) {
        std::cout << "DuplicateOutput failed (hr = 0x" << std::hex << hr << ")\n";
        return false;
    }

    DXGI_OUTDUPL_DESC desc;
    duplication->GetDesc(&desc);
    WIDTH = desc.ModeDesc.Width;
    HEIGHT = desc.ModeDesc.Height;

    std::cout << "DXGI completed (" << WIDTH << "x" << HEIGHT << ")\n";
    return true;
}

void CreateStaging()
{
    if (staging) staging->Release();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = WIDTH;
    desc.Height = HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;

    device->CreateTexture2D(&desc, nullptr, &staging);
}

bool UpdateCS2Window()
{
    cs2Window = FindWindowA(nullptr, "Counter-Strike 2");
    if (!cs2Window)
        cs2Window = FindWindowA(nullptr, "Counter-Strike 2 - Direct3D 11");

    if (!cs2Window) return false;

    GetClientRect(cs2Window, &cs2Rect);
    MapWindowPoints(cs2Window, HWND_DESKTOP, (LPPOINT)&cs2Rect, 2);

    captureX = cs2Rect.left;
    captureY = cs2Rect.top;
    captureWidth = cs2Rect.right - cs2Rect.left;
    captureHeight = cs2Rect.bottom - cs2Rect.top;

    return (captureWidth > 200 && captureHeight > 200);
}

void CaptureFrame()
{
    if (!duplication || !staging) return;

    IDXGIResource* res = nullptr;
    DXGI_OUTDUPL_FRAME_INFO info{};

    HRESULT hr = duplication->AcquireNextFrame(5, &info, &res);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        std::cout << "Access lost - reset DXGI...\n";
        InitDXGI();
        CreateStaging();
        return;
    }
    if (FAILED(hr) || !res)
    {
        if (res) res->Release();
        return;
    }

    if (info.LastPresentTime.QuadPart == lastPresentTime && info.AccumulatedFrames == 0)
    {
        duplication->ReleaseFrame();
        res->Release();
        return;
    }

    lastPresentTime = info.LastPresentTime.QuadPart;


    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)))
    {
        context->CopyResource(staging, tex);

        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
        {
            DWORD written = 0;

            WriteFile(
                ffmpegIn,
                map.pData,
                WIDTH * HEIGHT * 4,
                &written,
                nullptr
            );
            if (written > 0) {
                const long long frameQpc = (info.LastPresentTime.QuadPart > 0)
                    ? info.LastPresentTime.QuadPart
                    : [] {
                    LARGE_INTEGER qpcNow{};
                    QueryPerformanceCounter(&qpcNow);
                    return qpcNow.QuadPart;
                }();
                if (timing.firstVideoQpc.load(std::memory_order_relaxed) < 0) {
                    timing.firstVideoQpc.store(frameQpc, std::memory_order_relaxed);
                }
                timing.lastVideoQpc.store(frameQpc, std::memory_order_relaxed);
            }

            context->Unmap(staging, 0);
        }
        tex->Release();
    }

    duplication->ReleaseFrame();
    res->Release();
}

void CleanupTemporaryCaptureFiles()
{
    std::error_code ec;
    if (std::filesystem::exists(segmentDirPath)) {
        for (const auto& entry : std::filesystem::directory_iterator(segmentDirPath)) {
            std::filesystem::remove(entry.path(), ec);
        }
        std::filesystem::remove(segmentDirPath, ec);
    }
    std::filesystem::remove(concatListPath, ec);
    std::filesystem::remove((std::filesystem::path(tempSessionPath) / "segments_resolved.ffconcat").string(), ec);
    if (!tempSessionPath.empty()) {
        std::filesystem::remove_all(tempSessionPath, ec);
    }
}

void Stop(bool saveClip)
{
    std::cout << "Stoping recording...\n";

    if (ffmpegIn)
    {
        FlushFileBuffers(ffmpegIn);
        CloseHandle(ffmpegIn);
        ffmpegIn = nullptr;
    }

    StopAudio();

    if (ffmpegProc.hProcess)
    {
        WaitForSingleObject(ffmpegProc.hProcess, 4000);
        CloseHandle(ffmpegProc.hProcess);
        CloseHandle(ffmpegProc.hThread);
    }

    if (staging) { staging->Release();     staging = nullptr; }
    if (duplication) { duplication->Release(); duplication = nullptr; }
    if (context) { context->Release();     context = nullptr; }
    if (device) { device->Release();      device = nullptr; }

    if (saveClip) {
        MergeAudioVideo();
    }
    else {
        std::cout << "Capture stopped without saving clip.\n";
        CleanupTemporaryCaptureFiles();
        videoStarted = false;
        audioStarted = false;
        timing.firstAudioQpc.store(-1, std::memory_order_relaxed);
        timing.lastAudioQpc.store(-1, std::memory_order_relaxed);
        timing.firstVideoQpc.store(-1, std::memory_order_relaxed);
        timing.lastVideoQpc.store(-1, std::memory_order_relaxed);
    }
}

int main()
{
    bool saveRequested = false;
    replayBufferSeconds = GetReplayBufferSeconds();
    std::cout << "=== CS2 Recorder - Fullscreen (60 fps) ===\n\n";
    std::cout << "Replay buffer length: " << replayBufferSeconds << "s\n";
    std::cout << "A/V mode: single FFmpeg timeline (shared sync)\n";
    std::cout << "Launch CS2 in Fullscreen mode and wait...\n";
    std::cout << "Save clip hotkey: Alt+F12\n";

    while (running)
    {
        const bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool f12PressedNow = (GetAsyncKeyState(VK_F12) & 0x0001) != 0;
        if (altHeld && f12PressedNow) {
            std::cout << "Alt+F12 detected - saving clip...\n";
            saveRequested = true;
            break;
        }

        if (!cs2Window || !IsWindow(cs2Window))
        {
            if (videoStarted || audioStarted || ffmpegIn || audioRunning) {
                std::cout << "CS2 closed/lost - stopping without save.\n";
                Stop(false);
            }

            if (UpdateCS2Window())
            {
                std::cout << "CS2 found, resolution: " << captureWidth << "x" << captureHeight << "\n";
                if (InitDXGI())
                {
                    std::cout << "Arming replay buffer in " << kCaptureStartDelayMs << "ms...\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(kCaptureStartDelayMs));

                    QueryPerformanceFrequency(&qpcFrequency);
                    QueryPerformanceCounter(&sessionStartQpc);
                    CreateStaging();
                    PrepareOutputPaths();
                    timing.firstAudioQpc.store(-1, std::memory_order_relaxed);
                    timing.lastAudioQpc.store(-1, std::memory_order_relaxed);
                    timing.firstVideoQpc.store(-1, std::memory_order_relaxed);
                    timing.lastVideoQpc.store(-1, std::memory_order_relaxed);

                    StartFFmpeg();
                    if (!ffmpegIn) {
                        std::cout << "Video pipeline failed to start.\n";
                    }
                    else {
                        StartAudio();
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        RECT current{};
        GetClientRect(cs2Window, &current);
        MapWindowPoints(cs2Window, HWND_DESKTOP, (LPPOINT)&current, 2);

        if ((current.right - current.left != captureWidth) || (current.bottom - current.top != captureHeight))
        {
            std::cout << "CS2 window resolution changed - stopping without save...\n";
            Stop(false);
            cs2Window = nullptr;
            continue;
        }

        CaptureFrame();

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    Stop(saveRequested);
    if (saveRequested) {
        std::cout << "Recording finished. Result: " << finalPath << "\n";
    }
    else {
        std::cout << "Recording finished without saving clip.\n";
    }
    return 0;
}