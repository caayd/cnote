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
        static_cast<void>(OpenDocumentPath(*startupPath));
    }

    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

bool App::CreateMainWindow(int showCommand) {
    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &App::WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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

    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

}  // namespace cnote
