#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <vector>
#include <filesystem>
#include <chrono>
#include <thread>
#include <atomic>

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

HWND cs2Window = nullptr;
RECT cs2Rect{};
int captureX = 0, captureY = 0;
int captureWidth = 0, captureHeight = 0;

uint64_t lastPresentTime = 0;
std::chrono::steady_clock::time_point lastSentTime = std::chrono::steady_clock::now();

std::string FindFFmpeg()
{
    auto path = std::filesystem::current_path() / "ffmpeg" / "bin" / "ffmpeg.exe";
    std::cout << "Szukam FFmpeg: " << path << std::endl;
    return std::filesystem::exists(path) ? path.string() : "";
}

std::string OutputPath()
{
    std::filesystem::create_directories("C:\\CS2Recordings");
    return "C:\\CS2Recordings\\cs2_recording_" + std::to_string(std::time(nullptr)) + ".mp4";
}

void StartFFmpeg()
{
    std::string ffmpeg = FindFFmpeg();
    if (ffmpeg.empty()) {
        std::cout << "FFmpeg nie znaleziony!\n";
        return;
    }

    std::string out = OutputPath();

    std::string cmd =
        "\"" + ffmpeg + "\" -y "
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
        "\"" + out + "\"";

    std::cout << "Uruchamiam FFmpeg (stabilne 60 fps dla Fullscreen)...\n";

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
        std::cout << "D3D11CreateDevice nie powiodło się!\n";
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
        std::cout << "DuplicateOutput nie powiodło się (hr = 0x" << std::hex << hr << ")\n";
        return false;
    }

    DXGI_OUTDUPL_DESC desc;
    duplication->GetDesc(&desc);
    WIDTH = desc.ModeDesc.Width;
    HEIGHT = desc.ModeDesc.Height;

    std::cout << "DXGI gotowy (" << WIDTH << "x" << HEIGHT << ")\n";
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

            context->Unmap(staging, 0);
        }
        tex->Release();
    }

    duplication->ReleaseFrame();
    res->Release();
}

void Stop()
{
    std::cout << "Zatrzymywanie nagrywania...\n";

    if (ffmpegIn)
    {
        FlushFileBuffers(ffmpegIn);
        CloseHandle(ffmpegIn);
        ffmpegIn = nullptr;
    }

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
}

int main()
{
    std::cout << "=== CS2 Recorder - Fullscreen (stabilne 60 fps) ===\n\n";
    std::cout << "Uruchom CS2 w trybie Fullscreen i czekaj...\n";

    while (running)
    {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            break;

        if (!cs2Window || !IsWindow(cs2Window))
        {
            if (UpdateCS2Window())
            {
                std::cout << "CS2 znaleziony! Rozmiar: " << captureWidth << "x" << captureHeight << "\n";
                if (InitDXGI())
                {
                    CreateStaging();
                    StartFFmpeg();
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
            std::cout << "Rozmiar CS2 zmieniony - restart nagrywania...\n";
            Stop();
            cs2Window = nullptr;
            continue;
        }

        CaptureFrame();

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    Stop();
    std::cout << "Nagrywanie zakończone. Pliki znajdują się w C:\\CS2Recordings\n";
    return 0;
}