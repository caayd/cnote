#include "App.h"

#include <dwmapi.h>

#include "DpiUtil.h"
#include "resource.h"

namespace cnote {

namespace {

constexpr wchar_t kWindowClassName[] = L"CnoteWindow";
constexpr wchar_t kWindowTitle[] = L"cnote";
constexpr DWORD kDwmWindowCornerRound = 2;

}  // namespace

int App::Run(int showCommand, std::optional<std::wstring> startupPath) {
    if (!CreateMainWindow(showCommand)) {
        return 1;
    }

    if (startupPath.has_value() && !startupPath->empty()) {
        if (!OpenDocumentPath(*startupPath)) {
            CreateBlankDocument();
        }
    } else {
        CreateBlankDocument();
    }

    ShowWindow(window_, showCommand);
    UpdateWindow(window_);

    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

bool App::OpenNewWindow() {
    std::wstring executablePath(MAX_PATH, L'\0');
    DWORD pathLength = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    while (pathLength >= executablePath.size()) {
        executablePath.resize(executablePath.size() * 2, L'\0');
        pathLength = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    }

    if (pathLength == 0) {
        MessageBoxW(window_, L"Failed to locate the cnote executable.", L"New window", MB_OK | MB_ICONERROR);
        return false;
    }

    executablePath.resize(pathLength);
    std::wstring commandLine = L"\"" + executablePath + L"\"";

    STARTUPINFOW startupInfo {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo {};
    std::wstring mutableCommandLine = commandLine;
    const BOOL created = CreateProcessW(
        executablePath.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);
    if (created == FALSE) {
        MessageBoxW(window_, L"Failed to open a new window.", L"New window", MB_OK | MB_ICONERROR);
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

bool App::CreateMainWindow(int showCommand) {
    static_cast<void>(showCommand);

    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &App::WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&windowClass) == 0) {
        return false;
    }

    const UINT initialDpi = SystemDpi();
    RECT initialWindowRect { 0, 0, ScaleForDpi(1400, initialDpi), ScaleForDpi(960, initialDpi) };
    AdjustWindowRectForDpi(&initialWindowRect, WS_OVERLAPPEDWINDOW, FALSE, 0, initialDpi);
    window_ = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        initialWindowRect.right - initialWindowRect.left,
        initialWindowRect.bottom - initialWindowRect.top,
        nullptr,
        nullptr,
        instance_,
        this);

    if (window_ == nullptr) {
        return false;
    }

    constexpr DWORD kDwmDarkModeAttribute = 20;
    constexpr DWORD kDwmBorderColorAttribute = 34;
    constexpr DWORD kDwmCaptionColorAttribute = 35;
    constexpr DWORD kDwmTextColorAttribute = 36;
    const BOOL useDarkMode = TRUE;
    const COLORREF chromeColor = RGB(15, 18, 24);
    const COLORREF chromeText = RGB(236, 240, 245);
    const DWORD cornerPreference = kDwmWindowCornerRound;
    const MARGINS customFrameMargins { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(window_, &customFrameMargins);
    DwmSetWindowAttribute(window_, kDwmDarkModeAttribute, &useDarkMode, sizeof(useDarkMode));
    DwmSetWindowAttribute(window_, kDwmBorderColorAttribute, &chromeColor, sizeof(chromeColor));
    DwmSetWindowAttribute(window_, kDwmCaptionColorAttribute, &chromeColor, sizeof(chromeColor));
    DwmSetWindowAttribute(window_, kDwmTextColorAttribute, &chromeText, sizeof(chromeText));
    DwmSetWindowAttribute(window_, 33, &cornerPreference, sizeof(cornerPreference));
    UpdateWindowTitle();
    UpdateAutoSaveTimer();
    InitializePrecisionTouchpadSupport();
    return true;
}

}  // namespace cnote
