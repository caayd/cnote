#include "App.h"

#include <dwmapi.h>
#include <algorithm>

#include "DpiUtil.h"
#include "resource.h"

namespace cnote {

namespace {

constexpr wchar_t kWindowClassName[] = L"CnoteWindow";
constexpr wchar_t kWindowTitle[] = L"cnote";
constexpr DWORD kDwmWindowCornerRound = 2;

}  // namespace

int App::Run(int showCommand, std::optional<std::filesystem::path> startupPath) {
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
        std::erase_if(childWindows_, [](const auto& view) { return view->window_ == nullptr; });
    }

    childWindows_.clear();
    return static_cast<int>(message.wParam);
}

bool App::OpenNewWindow() { return CreateAdditionalWindow(false); }
bool App::OpenNewView() { return CreateAdditionalWindow(true); }

bool App::CreateAdditionalWindow(bool linked, int showCommand) {
    FinishDocumentInteraction();
    auto view = std::make_unique<App>(instance_);
    view->host_ = host_;
    if (linked) {
        view->AttachSession(session_);
        view->currentPageIndex_ = CurrentPageIndex();
        view->zoom_ = zoom_;
        view->targetZoom_ = zoom_;
        view->panX_ = panX_;
        view->panY_ = panY_;
        view->pdfSidebarOpen_ = pdfSidebarOpen_;
        view->pdfSidebarSlideProgress_ = pdfSidebarOpen_ ? 1.0f : 0.0f;
        view->newPageSettings_.width = session_->document.NewPageDefaults().width;
        view->newPageSettings_.height = session_->document.NewPageDefaults().height;
        view->newPageSettings_.backgroundColor = session_->document.NewPageDefaults().backgroundColor;
        view->newPageSettings_.paletteProfile = session_->document.NewPageDefaults().paletteProfile;
        view->ClearSelection();
    }
    if (!view->CreateMainWindow(showCommand)) {
        MessageBoxW(window_, L"Failed to create a window.", L"New window", MB_OK | MB_ICONERROR);
        return false;
    }
    if (!linked) view->CreateBlankDocument();
    App* created = view.get();
    host_->childWindows_.push_back(std::move(view));
    ShowWindow(created->window_, showCommand);
    UpdateWindow(created->window_);
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

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
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

    openWindows_.push_back(this);

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
