#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <vector>
#include <filesystem>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>

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
std::string videoPath;
std::string audioPath;
std::string finalPath;
std::string sessionId;
bool videoStarted = false;
bool audioStarted = false;
CaptureTiming timing;
LARGE_INTEGER qpcFrequency{};
LARGE_INTEGER sessionStartQpc{};
constexpr int kAudioPrerollMs = 220;
constexpr int kAudioWarmupTimeoutMs = 1200;
constexpr double kAudioLateMicroCompensationMs = 12.0;

HWND cs2Window = nullptr;
RECT cs2Rect{};
int captureX = 0, captureY = 0;
int captureWidth = 0, captureHeight = 0;

uint64_t lastPresentTime = 0;

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

void PrepareOutputPaths()
{
    std::filesystem::create_directories("C:\\CS2Recordings");
    sessionId = BuildSessionId();
    videoPath = "C:\\CS2Recordings\\video_" + sessionId + ".mp4";
    audioPath = "C:\\CS2Recordings\\audio_" + sessionId + ".wav";
    finalPath = "C:\\CS2Recordings\\cs2_recording_" + sessionId + ".mp4";
}

void StartFFmpeg()
{
    ffmpegPath = FindFFmpeg();
    if (ffmpegPath.empty()) {
        std::cout << "FFmpeg not found\n";
        return;
    }

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
        "-pix_fmt yuv420p "
        "\"" + videoPath + "\"";

    std::cout << "Launching FFmpeg (60 fps fullscreen)...\n";

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
        CaptureAudioLoop(audioPath, audioRunning, timing);
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
    if (!std::filesystem::exists(videoPath) || !std::filesystem::exists(audioPath)) {
        std::cout << "Skipping merging - missing audio or video file.\n";
        return;
    }

    const auto qpcToMs = [](long long qpcValue) -> double {
        if (qpcValue < 0 || qpcFrequency.QuadPart <= 0) {
            return -1.0;
        }
        return (1000.0 * static_cast<double>(qpcValue - sessionStartQpc.QuadPart))
            / static_cast<double>(qpcFrequency.QuadPart);
    };
    const auto secString = [](double ms) -> std::string {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3) << (ms / 1000.0);
        return ss.str();
    };

    const double audioStartMs = qpcToMs(timing.firstAudioQpc.load(std::memory_order_relaxed));
    const double videoStartMs = qpcToMs(timing.firstVideoQpc.load(std::memory_order_relaxed));
    const double audioEndMs = qpcToMs(timing.lastAudioQpc.load(std::memory_order_relaxed));
    const double videoEndMs = qpcToMs(timing.lastVideoQpc.load(std::memory_order_relaxed));

    double trimAudioStartMs = 0.0;
    double trimVideoStartMs = 0.0;
    if (audioStartMs >= 0.0 && videoStartMs >= 0.0) {
        if (audioStartMs > videoStartMs) {
            trimVideoStartMs = audioStartMs - videoStartMs;
        }
        else {
            trimAudioStartMs = videoStartMs - audioStartMs;
        }
    }
    trimVideoStartMs += kAudioLateMicroCompensationMs;

    std::cout << "A/V timing [ms]: audioStart=" << audioStartMs
        << ", videoStart=" << videoStartMs
        << ", audioEnd=" << audioEndMs
        << ", videoEnd=" << videoEndMs
        << ", trimAudio=" << trimAudioStartMs
        << ", trimVideo=" << trimVideoStartMs << "\n";

    std::string cmd =
        "\"" + ffmpegPath + "\" -y "
        "-ss " + secString(trimVideoStartMs) + " -i \"" + videoPath + "\" "
        "-ss " + secString(trimAudioStartMs) + " -i \"" + audioPath + "\" "
        "-c:v copy -c:a aac -b:a 192k -af aresample=async=1:first_pts=0 -shortest "
        "\"" + finalPath + "\"";

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::cout << "Merge FFmpeg failed\n";
        return;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (code == 0 && std::filesystem::exists(finalPath)) {
        std::cout << "Merge finished: " << finalPath << "\n";
        std::error_code ec;
        std::filesystem::remove(videoPath, ec);
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
    std::cout << "=== CS2 Recorder - Fullscreen (60 fps) ===\n\n";
    std::cout << "Launch CS2 in Fullscreen mode and wait...\n";

    while (running)
    {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            break;

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
                    StartAudio();
                    if (audioRunning) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(kAudioPrerollMs));
                        const auto waitStart = std::chrono::steady_clock::now();
                        while (audioRunning && timing.firstAudioQpc.load(std::memory_order_relaxed) < 0) {
                            const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - waitStart).count();
                            if (waitedMs >= kAudioWarmupTimeoutMs) {
                                std::cout << "Audio warmup timeout - startuje video mimo braku pierwszego pakietu\n";
                                break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }
                    }
                    StartFFmpeg();
                    if (!ffmpegIn) {
                        StopAudio();
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