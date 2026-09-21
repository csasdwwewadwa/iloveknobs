#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winmm.lib")

namespace {
constexpr wchar_t kClassName[] = L"A90NativeOverlay";
constexpr int kReferenceWidth = 1920;
constexpr int kReferenceHeight = 1080;
constexpr double kDisplayRatio = 0.7;
constexpr UINT_PTR kRestoreTimer = 90;
constexpr int kPopupPadding = 300;
constexpr int kPopupWidth = 340;
constexpr int kPopupHeight = 128;
constexpr double kAttackCheckSeconds = 0.6;
constexpr int kToggleControl = 1001;
constexpr int kTriggerControl = 1002;
constexpr UINT kTriggerMessage = WM_APP + 90;

struct Config { double minInterval = 20, maxInterval = 60, loadingSeconds = 5; int sampleHz = 100; };
struct Bitmap { int width = 0, height = 0; std::vector<BYTE> pixels; };
struct Popup { HWND hwnd = nullptr; std::wstring title; std::wstring message; };
struct App {
    HINSTANCE instance{}; Config config{}; HWND controller{}; HWND gameWindow{}; HWND background{}; HWND entity{}; std::vector<Popup*> popups;
    std::mt19937 random{std::random_device{}()};
    int popupProgress = 0;
    Bitmap mainImage, blockImage, scareImage; std::array<Bitmap, 3> noise{};
    bool running = true; bool enabled = true; bool attack = false; bool debugTrigger = false;
} *g_app = nullptr;

std::wstring RootPath(const wchar_t* name);

void Log(const std::wstring& message) {
    std::wofstream file(RootPath(L"a90_native.log"), std::ios::app);
    file << message << L"\n";
}

std::wstring RootPath(const wchar_t* name) {
    wchar_t buffer[MAX_PATH]{}; GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer); auto slash = path.find_last_of(L"\\/");
    return path.substr(0, slash + 1) + name;
}

double Number(const std::wstring& text, double fallback) { try { return std::stod(text); } catch (...) { return fallback; } }
void LoadConfig(Config& config) {
    std::wifstream file(RootPath(L"config.txt")); std::wstring line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        auto equals = line.find(L'='); if (equals == std::wstring::npos) continue;
        auto key = line.substr(0, equals), value = line.substr(equals + 1);
        if (key == L"min_interval") config.minInterval = Number(value, config.minInterval);
        else if (key == L"max_interval") config.maxInterval = Number(value, config.maxInterval);
        else if (key == L"sample_hz") config.sampleHz = static_cast<int>(Number(value, config.sampleHz));
        else if (key == L"attack_loading_duration") config.loadingSeconds = Number(value, config.loadingSeconds);
    }
}

bool LoadWebp(const std::wstring& path, Bitmap& output) {
    IWICImagingFactory* factory = nullptr; IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr; IWICFormatConverter* converter = nullptr;
    bool okay = false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) { Log(L"WIC factory failed"); return false; }
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT width = 0, height = 0; converter->GetSize(&width, &height); output.width = static_cast<int>(width); output.height = static_cast<int>(height); output.pixels.resize(width * height * 4);
        okay = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(output.pixels.size()), output.pixels.data()));
    }
    if (converter) converter->Release(); if (frame) frame->Release(); if (decoder) decoder->Release(); if (factory) factory->Release();
    BYTE alphaMinimum = 255;
    BYTE alphaMaximum = 0;
    for (size_t index = 3; index < output.pixels.size(); index += 4) {
        alphaMinimum = (std::min)(alphaMinimum, output.pixels[index]);
        alphaMaximum = (std::max)(alphaMaximum, output.pixels[index]);
    }
    Log(path + L" loaded=" + (okay ? L"true" : L"false") + L" size=" + std::to_wstring(output.width) + L"x" + std::to_wstring(output.height) + L" alpha=" + std::to_wstring(alphaMinimum) + L"-" + std::to_wstring(alphaMaximum));
    return okay;
}

void PlaySoundFile(const std::wstring& name) {
    static int serial = 0; std::wstring alias = L"a90_sound_" + std::to_wstring(++serial);
    std::wstring command = L"open \"" + RootPath((L"assets\\" + name + L".mp3").c_str()) + L"\" type mpegvideo alias " + alias;
    mciSendStringW(command.c_str(), nullptr, 0, nullptr); mciSendStringW((L"play " + alias + L" from 0").c_str(), nullptr, 0, nullptr);
}

void SetOverlayStyle(HWND window, bool clickThrough) {
    LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE); style |= WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    if (clickThrough) style |= WS_EX_TRANSPARENT; SetWindowLongPtrW(window, GWL_EXSTYLE, style);
}

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATEANDEAT;
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowBitmap(HWND window, const Bitmap& bitmap, int x, int y, BYTE opacity) {
    if (!bitmap.width || !bitmap.height) { Log(L"ShowBitmap skipped empty bitmap"); return; }
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = bitmap.width; info.bmiHeader.biHeight = -bitmap.height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen); void* bits = nullptr; HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0); memcpy(bits, bitmap.pixels.data(), bitmap.pixels.size()); HGDIOBJ old = SelectObject(memory, dib);
    POINT position{x, y}; POINT source{0, 0}; SIZE size{bitmap.width, bitmap.height}; BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    BOOL updated = UpdateLayeredWindow(window, screen, &position, &size, memory, &source, 0, &blend, ULW_ALPHA);
    Log(L"UpdateLayeredWindow result=" + std::to_wstring(updated ? 1 : 0) + L" error=" + std::to_wstring(GetLastError()) + L" size=" + std::to_wstring(bitmap.width) + L"x" + std::to_wstring(bitmap.height));
    SelectObject(memory, old); DeleteObject(dib); DeleteDC(memory); ReleaseDC(nullptr, screen); ShowWindow(window, SW_SHOWNOACTIVATE); SetWindowPos(window, HWND_TOPMOST, x, y, bitmap.width, bitmap.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

Bitmap MakeNoise(int width, int height, bool bright) {
    Bitmap noise{width, height, std::vector<BYTE>(static_cast<size_t>(width) * height * 4)};
    std::array<COLORREF, 4> colors = bright
        ? std::array<COLORREF, 4>{RGB(18, 0, 0), RGB(82, 0, 0), RGB(170, 8, 8), RGB(255, 28, 28)}
        : std::array<COLORREF, 4>{RGB(2, 0, 0), RGB(38, 0, 0), RGB(82, 3, 3), RGB(128, 8, 8)};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            COLORREF color = colors[g_app->random() % colors.size()];
            size_t index = (static_cast<size_t>(y) * width + x) * 4;
            noise.pixels[index + 0] = GetBValue(color);
            noise.pixels[index + 1] = GetGValue(color);
            noise.pixels[index + 2] = GetRValue(color);
            noise.pixels[index + 3] = 255;
        }
    }
    return noise;
}

Bitmap ScaleNearest(const Bitmap& source, int width, int height) {
    Bitmap scaled{width, height, std::vector<BYTE>(static_cast<size_t>(width) * height * 4)};
    for (int y = 0; y < height; ++y) {
        int sourceY = y * source.height / height;
        for (int x = 0; x < width; ++x) {
            int sourceX = x * source.width / width;
            size_t sourceIndex = (static_cast<size_t>(sourceY) * source.width + sourceX) * 4;
            size_t targetIndex = (static_cast<size_t>(y) * width + x) * 4;
            std::copy_n(source.pixels.data() + sourceIndex, 4, scaled.pixels.data() + targetIndex);
        }
    }
    return scaled;
}

Bitmap ScaleEntity(const Bitmap& source, double extraScale = 1.0) {
    double widthRatio = static_cast<double>(GetSystemMetrics(SM_CXSCREEN)) / kReferenceWidth;
    double heightRatio = static_cast<double>(GetSystemMetrics(SM_CYSCREEN)) / kReferenceHeight;
    double scale = (std::min)(widthRatio, heightRatio) * kDisplayRatio * extraScale;
    int width = (std::max)(1, static_cast<int>(source.width * scale + 0.5));
    int height = (std::max)(1, static_cast<int>(source.height * scale + 0.5));
    return ScaleNearest(source, width, height);
}

void ShowCenteredEntity(const Bitmap& source, double extraScale = 1.0) {
    Bitmap scaled = ScaleEntity(source, extraScale);
    ShowBitmap(g_app->entity, scaled, (GetSystemMetrics(SM_CXSCREEN) - scaled.width) / 2, (GetSystemMetrics(SM_CYSCREEN) - scaled.height) / 2, 255);
}

void ShowNoise(bool bright, BYTE opacity) {
    Bitmap noise = ScaleNearest(MakeNoise(640, 360, bright), GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    ShowBitmap(g_app->background, noise, 0, 0, opacity);
}

void HideOverlays() { if (!g_app) return; ShowWindow(g_app->background, SW_HIDE); ShowWindow(g_app->entity, SW_HIDE); for (auto* popup : g_app->popups) ShowWindow(popup->hwnd, SW_HIDE); }

void ClosePopup(HWND window) {
    for (auto iterator = g_app->popups.begin(); iterator != g_app->popups.end(); ++iterator) {
        if ((*iterator)->hwnd == window) {
            delete *iterator;
            g_app->popups.erase(iterator);
            DestroyWindow(window);
            return;
        }
    }
}

void ClearPopups() {
    while (!g_app->popups.empty()) {
        HWND window = g_app->popups.back()->hwnd;
        delete g_app->popups.back();
        g_app->popups.pop_back();
        DestroyWindow(window);
    }
}

void DrawPopup(HWND window, HDC dc) {
    RECT rect{}; GetClientRect(window, &rect); HBRUSH background = CreateSolidBrush(RGB(241, 244, 249)); FillRect(dc, &rect, background); DeleteObject(background);
    RECT title{0, 0, rect.right, 28}; HBRUSH blue = CreateSolidBrush(RGB(36, 88, 154)); FillRect(dc, &title, blue); DeleteObject(blue);
    auto popup = reinterpret_cast<Popup*>(GetWindowLongPtrW(window, GWLP_USERDATA)); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255,255,255)); TextOutW(dc, 8, 7, popup->title.c_str(), static_cast<int>(popup->title.size())); SetTextColor(dc, RGB(17,17,17)); TextOutW(dc, 10, 44, popup->message.c_str(), static_cast<int>(popup->message.size()));
    RECT bar{10, 82, rect.right - 10, 101}; HBRUSH border = CreateSolidBrush(RGB(170, 180, 195)); FrameRect(dc, &bar, border); DeleteObject(border);
    RECT filled = bar; filled.right = filled.left + (filled.right - filled.left) * g_app->popupProgress / 100; HBRUSH progress = CreateSolidBrush(RGB(48, 118, 204)); FillRect(dc, &filled, progress); DeleteObject(progress);
}

LRESULT CALLBACK PopupProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTCLIENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_LBUTTONDOWN) { ClosePopup(window); return 0; }
    if (message == WM_PAINT) { PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); DrawPopup(window, dc); EndPaint(window, &paint); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}

void StartEvent();
void Attack();

LRESULT CALLBACK ControllerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COMMAND && LOWORD(wParam) == kToggleControl && HIWORD(wParam) == BN_CLICKED) {
        g_app->enabled = IsDlgButtonChecked(window, kToggleControl) == BST_CHECKED;
        if (!g_app->enabled) HideOverlays();
        return 0;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == kTriggerControl && HIWORD(wParam) == BN_CLICKED) {
        if (g_app->enabled) g_app->debugTrigger = true;
        return 0;
    }
    if (message == WM_CLOSE) {
        g_app->running = false;
        HideOverlays();
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}

void RegisterClasses(HINSTANCE instance) {
    WNDCLASSW overlay{}; overlay.hInstance = instance; overlay.lpfnWndProc = OverlayProc; overlay.lpszClassName = kClassName; RegisterClassW(&overlay);
    WNDCLASSW popup{}; popup.hInstance = instance; popup.lpfnWndProc = PopupProc; popup.lpszClassName = L"A90NativePopup"; popup.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); RegisterClassW(&popup);
    WNDCLASSW controller{}; controller.hInstance = instance; controller.lpfnWndProc = ControllerProc; controller.lpszClassName = L"A90NativeController"; controller.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); controller.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&controller);
}

HWND MakeOverlay(HINSTANCE instance, bool background) {
    (void)background;
    HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr); SetOverlayStyle(window, true); return window;
}

void CreatePopup() {
    auto titles = {L"Windows Security Update", L"pls speed i need this ;-;", L"A-90 moment", L"Downloading", L"Security Update", L"IMPORTANT!!", L"sans undertale"}; auto messages = {L"Downloading..", L"Getting the latest updates...", L"Virus detected! downloading antivirus..", L":3"};
    auto title = *std::next(titles.begin(), g_app->random() % titles.size()); auto message = *std::next(messages.begin(), g_app->random() % messages.size()); auto* popup = new Popup{nullptr, title, message};
    int x = 300 + g_app->random() % (std::max)(1, GetSystemMetrics(SM_CXSCREEN) - 600 - kPopupWidth);
    int y = 300 + g_app->random() % (std::max)(1, GetSystemMetrics(SM_CYSCREEN) - 600 - kPopupHeight);
    popup->hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"A90NativePopup", L"", WS_POPUP, x, y, kPopupWidth, kPopupHeight, nullptr, nullptr, g_app->instance, nullptr); SetWindowLongPtrW(popup->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(popup)); g_app->popups.push_back(popup); PlaySoundFile(L"popup" + std::to_wstring(1 + g_app->random() % 5)); ShowWindow(popup->hwnd, SW_SHOWNOACTIVATE); SetWindowPos(popup->hwnd, HWND_TOPMOST, x, y, kPopupWidth, kPopupHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW); UpdateWindow(popup->hwnd);
}

bool AnyKeyHeld() { 
    return 
        (GetAsyncKeyState('W') & 0x8000) || 
        (GetAsyncKeyState('A') & 0x8000) || 
        (GetAsyncKeyState('S') & 0x8000) || 
        (GetAsyncKeyState('D') & 0x8000) ||
        (GetAsyncKeyState(VK_UP) & 0x8000) || 
        (GetAsyncKeyState(VK_LEFT) & 0x8000) || 
        (GetAsyncKeyState(VK_DOWN) & 0x8000) || 
        (GetAsyncKeyState(VK_RIGHT) & 0x8000); 
}

void Pump(DWORD milliseconds) { auto end = GetTickCount64() + milliseconds; MSG message{}; while (GetTickCount64() < end) { while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { if (message.message == WM_QUIT) g_app->running = false; TranslateMessage(&message); DispatchMessageW(&message); } Sleep(5); } }

void PressKey(WORD virtualKey) {
    WORD scanCode = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    SendInput(1, &input, sizeof(input));
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

void FocusGameForInput() {
    if (!g_app->gameWindow || !IsWindow(g_app->gameWindow)) return;
    SetForegroundWindow(g_app->gameWindow);
    Sleep(20);
}

void StartEvent() {
    PlaySoundFile(L"appear");
    Bitmap scaled = ScaleEntity(g_app->mainImage);
    ShowBitmap(g_app->entity, scaled, 400, 300, 255);
    Pump(500);
    if (!g_app->running) return;
    HideOverlays();
    Attack();
}

void Attack() {
    ShowNoise(false, 128); ShowCenteredEntity(g_app->mainImage); PlaySoundFile(L"decide_attack"); bool attack = true; auto end = GetTickCount64() + static_cast<ULONGLONG>(kAttackCheckSeconds * 1000); while (GetTickCount64() < end) { if (!AnyKeyHeld()) attack = false; Pump(10); }
    if (!attack) { PlaySoundFile(L"block"); ShowNoise(false, 128); ShowCenteredEntity(g_app->blockImage); Pump(200); HideOverlays(); return; }
    ShowNoise(true, 128); ShowCenteredEntity(g_app->scareImage, 2.0); Pump(100); HideOverlays(); for (int i = 0; i < 7; ++i) { CreatePopup(); Pump(20); }
    g_app->popupProgress = 0;
    auto loadingStart = GetTickCount64();
    auto loadingEnd = loadingStart + static_cast<ULONGLONG>(g_app->config.loadingSeconds * 1000);
    while (GetTickCount64() < loadingEnd && !g_app->popups.empty()) {
        g_app->popupProgress = static_cast<int>(100.0 * (GetTickCount64() - loadingStart) / (g_app->config.loadingSeconds * 1000));
        for (auto* popup : g_app->popups) InvalidateRect(popup->hwnd, nullptr, FALSE);
        Pump(20);
    }
    bool popupCleared = g_app->popups.empty();
        if (!popupCleared) {
            ClearPopups();
        PlaySoundFile(L"jumpscare"); ShowNoise(true, 255); ShowCenteredEntity(g_app->scareImage, 2.0); Pump(100); FocusGameForInput(); PressKey(VK_ESCAPE); Pump(50); PressKey('R'); Pump(50); PressKey(VK_RETURN); Pump(1800);
    }
    HideOverlays();
}

} // namespace

bool RobloxIsForeground();

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); g_app = new App{}; g_app->instance = instance; LoadConfig(g_app->config); RegisterClasses(instance); g_app->controller = CreateWindowExW(WS_EX_APPWINDOW, L"A90NativeController", L"A-90 Controller", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 100, 100, 280, 170, nullptr, nullptr, instance, nullptr); CreateWindowExW(0, L"BUTTON", L"A-90 enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 20, 220, 28, g_app->controller, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToggleControl)), instance, nullptr); CreateWindowExW(0, L"BUTTON", L"Trigger A-90 now", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 60, 220, 30, g_app->controller, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTriggerControl)), instance, nullptr); CheckDlgButton(g_app->controller, kToggleControl, BST_CHECKED); ShowWindow(g_app->controller, SW_SHOWNOACTIVATE); g_app->background = MakeOverlay(instance, true); g_app->entity = MakeOverlay(instance, false); LoadWebp(RootPath(L"assets\\a90_main.webp"), g_app->mainImage); LoadWebp(RootPath(L"assets\\a90_block.webp"), g_app->blockImage); LoadWebp(RootPath(L"assets\\a90_scare.webp"), g_app->scareImage);
    std::uniform_real_distribution<double> interval(g_app->config.minInterval, g_app->config.maxInterval);
    while (g_app->running) {
        Pump(100);
        bool immediate = g_app->debugTrigger;
        g_app->debugTrigger = false;
        if (RobloxIsForeground()) g_app->gameWindow = GetForegroundWindow();
        if (!g_app->enabled || (!RobloxIsForeground() && !immediate)) continue;
        if (!immediate) Pump(static_cast<DWORD>(interval(g_app->random) * 1000));
        if (!g_app->enabled || (!RobloxIsForeground() && !immediate)) continue;
        StartEvent();
    }
    CoUninitialize(); delete g_app; return 0;
}

bool RobloxIsForeground() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    wchar_t title[256]{};
    GetWindowTextW(foreground, title, ARRAYSIZE(title));
    return std::wstring(title).find(L"Roblox") != std::wstring::npos;
}