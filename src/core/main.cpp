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
#include <deque>
#include <mutex>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <TlHelp32.h>
#include <cwchar>
#include <cstdio>

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
std::atomic<bool> ipcReaderRunning{ false };
std::thread ipcReaderThread;
std::mutex ipcQueueMutex;

struct IpcCommand
{
    std::string id;
    std::string cmd;
    int seconds = 0;
    bool hasSeconds = false;
    std::string chord;
    bool hasChord = false;
};

std::deque<IpcCommand> ipcQueue;

bool IsProcessRunning(const wchar_t* processName);

struct HotkeyConfig
{
    bool alt = true;
    bool ctrl = false;
    bool shift = false;
    int vk = VK_F12;
    std::string display = "ALT+F12";
};

HotkeyConfig saveClipHotkey{};

std::string JsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 16);
    for (char c : value) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

void EmitJson(const std::string& line)
{
    std::fwrite(line.c_str(), 1, line.size(), stdout);
    std::fwrite("\n", 1, 1, stdout);
    std::fflush(stdout);
}

void EmitAck(const std::string& id, bool ok, const std::string& error = "")
{
    std::string msg = "{\"type\":\"ack\",\"id\":\"" + JsonEscape(id) + "\",\"ok\":";
    msg += ok ? "true" : "false";
    if (!ok && !error.empty()) {
        msg += ",\"error\":\"" + JsonEscape(error) + "\"";
    }
    msg += "}";
    EmitJson(msg);
}

void EmitStatusEvent(const std::string& state)
{
    const bool cs2Running = IsProcessRunning(L"cs2.exe");
    std::string msg =
        "{\"type\":\"event\",\"event\":\"status\",\"protocol\":\"1.0\",\"state\":\"" + JsonEscape(state) +
        "\",\"cs2Running\":" + std::string(cs2Running ? "true" : "false") +
        ",\"bufferSeconds\":" + std::to_string(replayBufferSeconds) +
        ",\"saveHotkey\":\"" + JsonEscape(saveClipHotkey.display) + "\"" +
        ",\"bufferReady\":" + std::string((ffmpegIn != nullptr && audioRunning) ? "true" : "false") +
        "}";
    EmitJson(msg);
}

void EmitClipSavedEvent(const std::string& path)
{
    EmitJson(
        "{\"type\":\"event\",\"event\":\"clip_saved\",\"path\":\"" + JsonEscape(path) +
        "\",\"bufferSeconds\":" + std::to_string(replayBufferSeconds) + "}"
    );
}

void EmitWarningEvent(const std::string& message)
{
    EmitJson("{\"type\":\"event\",\"event\":\"warning\",\"message\":\"" + JsonEscape(message) + "\"}");
}

void EmitErrorEvent(const std::string& code, const std::string& message)
{
    EmitJson(
        "{\"type\":\"event\",\"event\":\"error\",\"code\":\"" + JsonEscape(code) +
        "\",\"message\":\"" + JsonEscape(message) + "\"}"
    );
}

bool ExtractJsonString(const std::string& jsonLine, const std::string& key, std::string& out)
{
    const std::string token = "\"" + key + "\":";
    size_t pos = jsonLine.find(token);
    if (pos == std::string::npos) return false;
    pos = jsonLine.find('"', pos + token.size());
    if (pos == std::string::npos) return false;
    const size_t start = pos + 1;
    size_t end = start;
    while (end < jsonLine.size()) {
        if (jsonLine[end] == '"' && (end == start || jsonLine[end - 1] != '\\')) break;
        end++;
    }
    if (end >= jsonLine.size()) return false;
    out = jsonLine.substr(start, end - start);
    return true;
}

bool ExtractJsonInt(const std::string& jsonLine, const std::string& key, int& out)
{
    const std::string token = "\"" + key + "\":";
    size_t pos = jsonLine.find(token);
    if (pos == std::string::npos) return false;
    size_t start = jsonLine.find_first_of("-0123456789", pos + token.size());
    if (start == std::string::npos) return false;
    size_t end = start + 1;
    while (end < jsonLine.size() && std::isdigit(static_cast<unsigned char>(jsonLine[end]))) end++;
    out = std::atoi(jsonLine.substr(start, end - start).c_str());
    return true;
}

bool ParseIpcCommand(const std::string& line, IpcCommand& out)
{
    std::string type;
    if (!ExtractJsonString(line, "type", type) || type != "cmd") return false;
    if (!ExtractJsonString(line, "id", out.id)) return false;
    if (!ExtractJsonString(line, "cmd", out.cmd)) return false;
    out.hasSeconds = ExtractJsonInt(line, "seconds", out.seconds);
    out.hasChord = ExtractJsonString(line, "chord", out.chord);
    return true;
}

void StartIpcReader()
{
    ipcReaderRunning = true;
    ipcReaderThread = std::thread([] {
        std::string line;
        while (ipcReaderRunning && std::getline(std::cin, line)) {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
                continue;
            }
            IpcCommand cmd{};
            if (!ParseIpcCommand(line, cmd)) {
                EmitErrorEvent("invalid_json_command", "Malformed command or missing fields.");
                continue;
            }
            std::lock_guard<std::mutex> lock(ipcQueueMutex);
            ipcQueue.push_back(cmd);
        }
    });
}

void StopIpcReader()
{
    ipcReaderRunning = false;
    if (ipcReaderThread.joinable()) {
        ipcReaderThread.detach();
    }
}

std::string ToUpperAscii(std::string value)
{
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string TrimAscii(const std::string& value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        start++;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        end--;
    }
    return value.substr(start, end - start);
}

bool ParseKeyTokenToVk(const std::string& tokenUpper, int& vk)
{
    if (tokenUpper.size() == 1) {
        const char c = tokenUpper[0];
        if (c >= 'A' && c <= 'Z') {
            vk = static_cast<int>(c);
            return true;
        }
        if (c >= '0' && c <= '9') {
            vk = static_cast<int>(c);
            return true;
        }
    }

    if (tokenUpper.size() >= 2 && tokenUpper[0] == 'F') {
        const int fn = std::atoi(tokenUpper.substr(1).c_str());
        if (fn >= 1 && fn <= 24) {
            vk = VK_F1 + (fn - 1);
            return true;
        }
    }

    if (tokenUpper == "SPACE") { vk = VK_SPACE; return true; }
    if (tokenUpper == "TAB") { vk = VK_TAB; return true; }
    if (tokenUpper == "ENTER") { vk = VK_RETURN; return true; }
    if (tokenUpper == "ESC" || tokenUpper == "ESCAPE") { vk = VK_ESCAPE; return true; }
    return false;
}

bool ParseHotkeyChord(const std::string& chord, HotkeyConfig& outConfig, std::string& errorCode)
{
    HotkeyConfig parsed{};
    parsed.alt = false;
    parsed.ctrl = false;
    parsed.shift = false;
    parsed.vk = 0;
    parsed.display = "";

    size_t start = 0;
    bool haveKeyToken = false;
    while (start <= chord.size()) {
        size_t plusPos = chord.find('+', start);
        const std::string rawToken = (plusPos == std::string::npos)
            ? chord.substr(start)
            : chord.substr(start, plusPos - start);
        const std::string token = ToUpperAscii(TrimAscii(rawToken));

        if (token.empty()) {
            errorCode = "invalid_hotkey_format";
            return false;
        }

        if (token == "ALT") {
            parsed.alt = true;
        }
        else if (token == "CTRL" || token == "CONTROL") {
            parsed.ctrl = true;
        }
        else if (token == "SHIFT") {
            parsed.shift = true;
        }
        else {
            if (haveKeyToken) {
                errorCode = "invalid_hotkey_multiple_keys";
                return false;
            }
            int parsedVk = 0;
            if (!ParseKeyTokenToVk(token, parsedVk)) {
                errorCode = "invalid_hotkey_key";
                return false;
            }
            parsed.vk = parsedVk;
            haveKeyToken = true;
        }

        if (plusPos == std::string::npos) {
            break;
        }
        start = plusPos + 1;
    }

    if (!haveKeyToken) {
        errorCode = "invalid_hotkey_missing_key";
        return false;
    }

    std::string display;
    if (parsed.ctrl) display += "CTRL+";
    if (parsed.shift) display += "SHIFT+";
    if (parsed.alt) display += "ALT+";
    display += ToUpperAscii(TrimAscii(chord.substr(chord.find_last_of('+') == std::string::npos ? 0 : chord.find_last_of('+') + 1)));
    parsed.display = display;

    outConfig = parsed;
    return true;
}

bool IsSaveHotkeyPressedNow()
{
    const bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool keyPressedNow = (GetAsyncKeyState(saveClipHotkey.vk) & 0x0001) != 0;

    const bool modifiersOk =
        (!saveClipHotkey.alt || altHeld) &&
        (!saveClipHotkey.ctrl || ctrlHeld) &&
        (!saveClipHotkey.shift || shiftHeld);

    return modifiersOk && keyPressedNow;
}

bool IsProcessRunning(const wchar_t* processName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

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

std::string BuildClipOutputPath()
{
    const auto now = std::chrono::system_clock::now();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return "C:\\CS2Recordings\\cs2_recording_" + std::to_string(nowMs) + ".mp4";
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

bool SaveCurrentReplayBuffer(std::string& outputPath)
{
    if (ffmpegPath.empty()) return false;
    if (!std::filesystem::exists(concatListPath)) {
        std::cout << "Skipping merging - missing segment list.\n";
        return false;
    }
    std::cout << "Saving current replay buffer (audio+video)...\n";
    const std::string resolvedConcatListPath =
        (std::filesystem::path(tempSessionPath) / "segments_resolved.ffconcat").string();
    if (!BuildResolvedConcatList(concatListPath, resolvedConcatListPath)) {
        std::cout << "Failed to build resolved concat list\n";
        return false;
    }

    outputPath = BuildClipOutputPath();
    std::string finalizeCmd =
        "\"" + ffmpegPath + "\" -y "
        "-f concat -safe 0 -i \"" + resolvedConcatListPath + "\" "
        "-fflags +genpts -avoid_negative_ts make_zero "
        "-c copy "
        "\"" + outputPath + "\"";

    DWORD code = 1;
    if (!RunCommand(finalizeCmd, code)) {
        std::cout << "Finalizing clip failed to start\n";
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(resolvedConcatListPath, ec);

    if (code == 0 && std::filesystem::exists(outputPath)) {
        std::cout << "Clip finalized: " << outputPath << "\n";
        finalPath = outputPath;
        return true;
    }
    
    std::cout << "Finalizing failed (code " << code << ").\n";
    return false;
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

void ResetCaptureTimingAndState()
{
    videoStarted = false;
    audioStarted = false;
    timing.firstAudioQpc.store(-1, std::memory_order_relaxed);
    timing.lastAudioQpc.store(-1, std::memory_order_relaxed);
    timing.firstVideoQpc.store(-1, std::memory_order_relaxed);
    timing.lastVideoQpc.store(-1, std::memory_order_relaxed);
}

void StopActiveReplayPipeline()
{
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
        ffmpegProc.hProcess = nullptr;
        ffmpegProc.hThread = nullptr;
    }

    videoStarted = false;
    audioStarted = false;
}

void MergeAudioVideoOnStop()
{
    std::string outputPath;
    if (!SaveCurrentReplayBuffer(outputPath)) {
        std::cout << "Finalizing on stop failed.\n";
    }
    CleanupTemporaryCaptureFiles();
    ResetCaptureTimingAndState();
}

void Stop(bool saveClip)
{
    std::cout << "Stoping recording...\n";

    StopActiveReplayPipeline();

    if (staging) { staging->Release();     staging = nullptr; }
    if (duplication) { duplication->Release(); duplication = nullptr; }
    if (context) { context->Release();     context = nullptr; }
    if (device) { device->Release();      device = nullptr; }

    if (saveClip) {
        MergeAudioVideoOnStop();
    }
    else {
        std::cout << "Capture stopped without saving clip.\n";
        CleanupTemporaryCaptureFiles();
        ResetCaptureTimingAndState();
    }
}

bool IsCapturePipelineReady()
{
    return ffmpegIn != nullptr && ffmpegProc.hProcess != nullptr && videoStarted && audioStarted;
}

void ResetReplayBufferPipeline()
{
    StopActiveReplayPipeline();

    CleanupTemporaryCaptureFiles();

    PrepareOutputPaths();
    timing.firstAudioQpc.store(-1, std::memory_order_relaxed);
    timing.lastAudioQpc.store(-1, std::memory_order_relaxed);
    timing.firstVideoQpc.store(-1, std::memory_order_relaxed);
    timing.lastVideoQpc.store(-1, std::memory_order_relaxed);

    StartFFmpeg();
    if (!ffmpegIn) {
        std::cout << "Video pipeline failed to restart.\n";
        return;
    }
    StartAudio();
}

bool HandleSaveClipRequest(std::string& outputPath, const char* sourceTag)
{
    if (!IsCapturePipelineReady()) {
        std::cout << sourceTag << ": replay buffer is not ready yet.\n";
        return false;
    }

    StopActiveReplayPipeline();

    if (!SaveCurrentReplayBuffer(outputPath)) {
        std::cout << sourceTag << ": save failed.\n";
        ResetReplayBufferPipeline();
        return false;
    }

    ResetReplayBufferPipeline();
    return true;
}

void ProcessIpcCommands()
{
    std::deque<IpcCommand> pending;
    {
        std::lock_guard<std::mutex> lock(ipcQueueMutex);
        pending.swap(ipcQueue);
    }

    for (const auto& cmd : pending) {
        if (cmd.cmd == "ping") {
            EmitAck(cmd.id, true);
            continue;
        }

        if (cmd.cmd == "get_status") {
            EmitAck(cmd.id, true);
            EmitStatusEvent(IsCapturePipelineReady() ? "recording" : "idle");
            continue;
        }

        if (cmd.cmd == "save_clip") {
            std::string path;
            if (HandleSaveClipRequest(path, "IPC save_clip")) {
                EmitAck(cmd.id, true);
                EmitClipSavedEvent(path);
                EmitStatusEvent(IsCapturePipelineReady() ? "recording" : "idle");
            }
            else {
                EmitAck(cmd.id, false, "buffer_not_ready_or_save_failed");
            }
            continue;
        }

        if (cmd.cmd == "set_buffer") {
            if (!cmd.hasSeconds) {
                EmitAck(cmd.id, false, "missing_seconds");
                continue;
            }
            if (cmd.seconds != 15 && cmd.seconds != 30) {
                EmitAck(cmd.id, false, "invalid_buffer_seconds");
                continue;
            }

            replayBufferSeconds = cmd.seconds;
            if (IsCapturePipelineReady()) {
                ResetReplayBufferPipeline();
            }
            EmitAck(cmd.id, true);
            EmitStatusEvent(IsCapturePipelineReady() ? "recording" : "idle");
            continue;
        }

        if (cmd.cmd == "set_hotkey") {
            if (!cmd.hasChord) {
                EmitAck(cmd.id, false, "missing_chord");
                continue;
            }

            HotkeyConfig parsed{};
            std::string hotkeyError;
            if (!ParseHotkeyChord(cmd.chord, parsed, hotkeyError)) {
                EmitAck(cmd.id, false, hotkeyError.empty() ? "invalid_hotkey" : hotkeyError);
                continue;
            }

            saveClipHotkey = parsed;
            EmitAck(cmd.id, true);
            EmitStatusEvent(IsCapturePipelineReady() ? "recording" : "idle");
            continue;
        }

        if (cmd.cmd == "shutdown") {
            EmitAck(cmd.id, true);
            running = false;
            continue;
        }

        EmitAck(cmd.id, false, "unknown_command");
    }
}

int main()
{
    std::cout.rdbuf(std::cerr.rdbuf());

    replayBufferSeconds = GetReplayBufferSeconds();
    std::cout << "=== CS2 Recorder - Fullscreen (60 fps) ===\n\n";
    std::cout << "Replay buffer length: " << replayBufferSeconds << "s\n";
    std::cout << "A/V mode: single FFmpeg timeline (shared sync)\n";
    std::cout << "Launch CS2 in Fullscreen mode and wait...\n";
    std::cout << "Save clip hotkey: " << saveClipHotkey.display << "\n";

    if (!IsProcessRunning(L"cs2.exe")) {
        std::cout << "CS2 is not running. Start Counter-Strike 2 first, then run this program.\n";
        return 1;
    }

    StartIpcReader();
    EmitStatusEvent("starting");

    while (running)
    {
        ProcessIpcCommands();

        if (IsSaveHotkeyPressedNow()) {
            std::string outputPath;
            if (HandleSaveClipRequest(outputPath, "Hotkey")) {
                EmitClipSavedEvent(outputPath);
                EmitStatusEvent(IsCapturePipelineReady() ? "recording" : "idle");
            }
        }

        if (!cs2Window || !IsWindow(cs2Window))
        {
            if (videoStarted || audioStarted || ffmpegIn || audioRunning) {
                std::cout << "CS2 closed/lost - stopping without save.\n";
                Stop(false);
                EmitWarningEvent("cs2_lost");
                EmitStatusEvent("idle");
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
                        EmitErrorEvent("video_pipeline_start_failed", "Video pipeline failed to start.");
                    }
                    else {
                        StartAudio();
                        EmitStatusEvent(IsCapturePipelineReady() ? "recording" : "idle");
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
            EmitWarningEvent("cs2_resolution_changed");
            EmitStatusEvent("idle");
            continue;
        }

        CaptureFrame();

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    Stop(false);
    StopIpcReader();
    EmitStatusEvent("stopped");
    std::cout << "Recording finished.\n";
    return 0;
}