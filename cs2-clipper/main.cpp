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
#include <sstream>
#include <iomanip>
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
std::string segmentDirPath;
std::string audioPath;
std::string stitchedVideoPath;
std::string finalPath;
std::string concatListPath;
std::string mergeLogPath;
std::string sessionId;
bool videoStarted = false;
bool audioStarted = false;
CaptureTiming timing;
LARGE_INTEGER qpcFrequency{};
LARGE_INTEGER sessionStartQpc{};
constexpr int kDefaultReplayBufferSeconds = 15;
constexpr double kDynamicSyncBiasMs = 0.0;

HWND cs2Window = nullptr;
RECT cs2Rect{};
int captureX = 0, captureY = 0;
int captureWidth = 0, captureHeight = 0;

uint64_t lastPresentTime = 0;
int replayBufferSeconds = kDefaultReplayBufferSeconds;
double audioAdvanceMs = kDynamicSyncBiasMs;

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

std::string FormatSeconds(double seconds)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3) << seconds;
    return ss.str();
}

std::string BuildSessionId()
{
    return std::to_string(std::time(nullptr));
}

double GetAudioAdvanceMs()
{
    return kDynamicSyncBiasMs;
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

bool RunLoggedCommand(const std::string& command, const std::string& logPath, const std::string& stage, DWORD& exitCode)
{
    {
        std::ofstream log(logPath, std::ios::app);
        if (log.is_open()) {
            log << "\n==== " << stage << " ====\n";
            log << command << "\n";
        }
    }

    std::string wrapped =
        "cmd.exe /C \"(" + command + ") >> \"" + logPath + "\" 2>&1\"";

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::vector<char> buf(wrapped.begin(), wrapped.end());
    buf.push_back('\0');

    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        {
            std::ofstream log(logPath, std::ios::app);
            if (log.is_open()) {
                log << "CreateProcess failed for stage " << stage
                    << ", error=" << GetLastError() << "\n";
            }
        }
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
    segmentDirPath = "C:\\CS2Recordings\\segments_" + sessionId;
    audioPath = "C:\\CS2Recordings\\audio_" + sessionId + ".wav";
    stitchedVideoPath = "C:\\CS2Recordings\\video_stitched_" + sessionId + ".mp4";
    finalPath = "C:\\CS2Recordings\\cs2_recording_" + sessionId + ".mp4";
    concatListPath = "C:\\CS2Recordings\\segments_" + sessionId + ".txt";
    mergeLogPath = "C:\\CS2Recordings\\merge_" + sessionId + ".log";
    std::filesystem::create_directories(segmentDirPath);
}

void StartFFmpeg()
{
    ffmpegPath = FindFFmpeg();
    if (ffmpegPath.empty()) {
        std::cout << "FFmpeg not found\n";
        return;
    }

    const int segmentWrapCount = replayBufferSeconds + 3;
    const std::string segmentPattern = segmentDirPath + "\\seg_%03d.ts";

    std::string cmd =
        "\"" + ffmpegPath + "\" -y "
        "-f rawvideo -pix_fmt bgra "
        "-s " + std::to_string(WIDTH) + "x" + std::to_string(HEIGHT) + " "
        "-r 60 -i - "
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
        CaptureAudioLoop(audioPath, audioRunning, timing, replayBufferSeconds);
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
    if (!std::filesystem::exists(audioPath)) {
        std::cout << "Skipping merging - missing audio file.\n";
        return;
    }
    if (!std::filesystem::exists(concatListPath)) {
        std::cout << "Skipping merging - missing segment list.\n";
        return;
    }
    std::cout << "Merging replay buffer using FFmpeg segment list + audio\n";
    const std::string resolvedConcatListPath = "C:\\CS2Recordings\\segments_" + sessionId + "_resolved.txt";
    if (!BuildResolvedConcatList(concatListPath, resolvedConcatListPath)) {
        std::cout << "Failed to build resolved concat list\n";
        return;
    }
    {
        std::ofstream log(mergeLogPath, std::ios::trunc);
        if (log.is_open()) {
            log << "Session: " << sessionId << "\n";
            log << "Replay buffer seconds: " << replayBufferSeconds << "\n";
        }
    }

    std::string stitchCmd =
        "\"" + ffmpegPath + "\" -y "
        "-f concat -safe 0 -i \"" + resolvedConcatListPath + "\" "
        "-fflags +genpts "
        "-c:v h264_nvenc -preset p5 -tune ll -rc cbr -b:v 20M -pix_fmt yuv420p "
        "\"" + stitchedVideoPath + "\"";

    DWORD stitchCode = 1;
    if (!RunLoggedCommand(stitchCmd, mergeLogPath, "stitch_segments", stitchCode)) {
        std::cout << "Segment stitching failed to start\n";
        std::cout << "Merge log: " << mergeLogPath << "\n";
        return;
    }

    if (stitchCode != 0 || !std::filesystem::exists(stitchedVideoPath)) {
        std::cout << "Segment stitching failed (code " << stitchCode << ")\n";
        std::cout << "Merge log: " << mergeLogPath << "\n";
        return;
    }

    const auto qpcToMs = [](long long qpcValue) -> double {
        if (qpcValue < 0 || qpcFrequency.QuadPart <= 0) {
            return -1.0;
        }
        return (1000.0 * static_cast<double>(qpcValue - sessionStartQpc.QuadPart))
            / static_cast<double>(qpcFrequency.QuadPart);
    };

    const double audioStartMs = qpcToMs(timing.firstAudioQpc.load(std::memory_order_relaxed));
    const double videoStartMs = qpcToMs(timing.firstVideoQpc.load(std::memory_order_relaxed));
    const double audioEndMs = qpcToMs(timing.lastAudioQpc.load(std::memory_order_relaxed));
    const double videoEndMs = qpcToMs(timing.lastVideoQpc.load(std::memory_order_relaxed));

    double trimAudioStartMs = 0.0;
    double trimVideoStartMs = 0.0;

    // Automatic sync (no hardcode):
    // - primary signal: end delta (replay buffer correctness)
    // - optional start delta only if it is close to end delta
    double estimatedOffsetMs = 0.0;
    double endDeltaMs = 0.0;
    bool hasEndDelta = false;
    if (audioEndMs >= 0.0 && videoEndMs >= 0.0) {
        endDeltaMs = (videoEndMs - audioEndMs);
        estimatedOffsetMs = endDeltaMs;
        hasEndDelta = true;
    }

    if (audioStartMs >= 0.0 && videoStartMs >= 0.0) {
        const double startDeltaMs = (videoStartMs - audioStartMs);
        if (!hasEndDelta) {
            estimatedOffsetMs = startDeltaMs;
        }
        else {
            // Ignore session-start warmup skew unless start/end tell similar story.
            if (std::abs(startDeltaMs - endDeltaMs) <= 120.0) {
                estimatedOffsetMs = (0.25 * startDeltaMs) + (0.75 * endDeltaMs);
            }
        }
    }

    estimatedOffsetMs += audioAdvanceMs;

    if (estimatedOffsetMs > 0.0) {
        trimAudioStartMs += estimatedOffsetMs;
    }
    else {
        trimVideoStartMs += -estimatedOffsetMs;
    }
    if (trimAudioStartMs < 0.0) trimAudioStartMs = 0.0;
    if (trimVideoStartMs < 0.0) trimVideoStartMs = 0.0;

    std::cout << "A/V timing [ms]: audioStart=" << audioStartMs
        << ", videoStart=" << videoStartMs
        << ", audioEnd=" << audioEndMs
        << ", videoEnd=" << videoEndMs
        << ", estOffset=" << estimatedOffsetMs
        << ", trimAudio=" << trimAudioStartMs
        << ", trimVideo=" << trimVideoStartMs << "\n";
    {
        std::ofstream log(mergeLogPath, std::ios::app);
        if (log.is_open()) {
            log << "A/V timing [ms]: audioStart=" << audioStartMs
                << ", videoStart=" << videoStartMs
                << ", audioEnd=" << audioEndMs
                << ", videoEnd=" << videoEndMs
                << ", estOffset=" << estimatedOffsetMs
                << ", trimAudio=" << trimAudioStartMs
                << ", trimVideo=" << trimVideoStartMs << "\n";
        }
    }

    std::string muxCmd =
        "\"" + ffmpegPath + "\" -y "
        "-i \"" + stitchedVideoPath + "\" "
        "-i \"" + audioPath + "\" "
        "-filter_complex \"[0:v]trim=start=" + FormatSeconds(trimVideoStartMs / 1000.0) + ",setpts=PTS-STARTPTS[v];"
        "[1:a]atrim=start=" + FormatSeconds(trimAudioStartMs / 1000.0) + ",asetpts=PTS-STARTPTS,aresample=async=1:first_pts=0[a]\" "
        "-map \"[v]\" -map \"[a]\" "
        "-c:v h264_nvenc -preset p5 -tune ll -rc cbr -b:v 20M -pix_fmt yuv420p "
        "-c:a aac -b:a 192k -shortest "
        "\"" + finalPath + "\"";

    DWORD code = 1;
    if (!RunLoggedCommand(muxCmd, mergeLogPath, "mux_audio_video", code)) {
        std::cout << "Merge FFmpeg failed\n";
        std::cout << "Merge log: " << mergeLogPath << "\n";
        return;
    }

    if (code == 0 && std::filesystem::exists(finalPath)) {
        std::cout << "Merge finished: " << finalPath << "\n";
        std::cout << "Merge log: " << mergeLogPath << "\n";
        std::error_code ec;
        if (std::filesystem::exists(segmentDirPath)) {
            for (const auto& entry : std::filesystem::directory_iterator(segmentDirPath)) {
                std::filesystem::remove(entry.path(), ec);
            }
            std::filesystem::remove(segmentDirPath, ec);
        }
        std::filesystem::remove(concatListPath, ec);
        std::filesystem::remove(resolvedConcatListPath, ec);
        std::filesystem::remove(stitchedVideoPath, ec);
        std::filesystem::remove(audioPath, ec);
    }
    else {
        std::cout << "Merge failed (code " << code << "). Temporary files left.\n";
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
                LARGE_INTEGER qpcNow{};
                QueryPerformanceCounter(&qpcNow);
                if (timing.firstVideoQpc.load(std::memory_order_relaxed) < 0) {
                    timing.firstVideoQpc.store(qpcNow.QuadPart, std::memory_order_relaxed);
                }
                timing.lastVideoQpc.store(qpcNow.QuadPart, std::memory_order_relaxed);
            }

            context->Unmap(staging, 0);
        }
        tex->Release();
    }

    duplication->ReleaseFrame();
    res->Release();
}

void Stop()
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

    MergeAudioVideo();
}

int main()
{
    replayBufferSeconds = GetReplayBufferSeconds();
    audioAdvanceMs = GetAudioAdvanceMs();
    std::cout << "=== CS2 Recorder - Fullscreen (60 fps) ===\n\n";
    std::cout << "Replay buffer length: " << replayBufferSeconds << "s\n";
    std::cout << "Dynamic A/V sync bias: " << audioAdvanceMs << "ms\n";
    std::cout << "Launch CS2 in Fullscreen mode and wait...\n";
    std::cout << "Save clip hotkey: Alt+F12\n";

    while (running)
    {
        const bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool f12PressedNow = (GetAsyncKeyState(VK_F12) & 0x0001) != 0;
        if (altHeld && f12PressedNow) {
            std::cout << "Alt+F12 detected - saving clip...\n";
            break;
        }

        if (!cs2Window || !IsWindow(cs2Window))
        {
            if (UpdateCS2Window())
            {
                std::cout << "CS2 found, resolution: " << captureWidth << "x" << captureHeight << "\n";
                if (InitDXGI())
                {
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
            std::cout << "CS2 window resolution changed - restarting recording...\n";
            Stop();
            cs2Window = nullptr;
            continue;
        }

        CaptureFrame();

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    Stop();
    std::cout << "Recording finished. Result: " << finalPath << "\n";
    return 0;
}