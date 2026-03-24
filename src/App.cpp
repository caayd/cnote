#include "App.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <utility>

#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

namespace ctrainer {

namespace {

constexpr wchar_t kWindowClassName[] = L"CTrainerPrototypeWindow";
constexpr wchar_t kWindowTitle[] = L"ctrainer - PDF Drawing Prototype";
constexpr int kToolbarPadding = 18;
constexpr int kToolButtonWidth = 92;
constexpr int kToolButtonHeight = 34;
constexpr int kColorSwatchSize = 26;
constexpr int kColorSwatchGap = 10;
constexpr unsigned int kCommandFileNew = 1001;
constexpr unsigned int kCommandFileOpen = 1002;
constexpr unsigned int kCommandFileExit = 1003;
constexpr unsigned int kCommandEditUndo = 1101;
constexpr unsigned int kCommandEditRedo = 1102;
constexpr unsigned int kCommandEditBrushSettings = 1103;
constexpr unsigned int kCommandViewReset = 1201;
constexpr unsigned int kCommandViewPreviousPage = 1202;
constexpr unsigned int kCommandViewNextPage = 1203;

struct MenuLayout {
    RECT fileRect {};
    RECT editRect {};
    RECT viewRect {};
};

struct ToolbarLayout {
    MenuLayout menu {};
    RECT previousPageRect {};
    RECT nextPageRect {};
    RECT pageLabelRect {};
    RECT undoRect {};
    RECT redoRect {};
    RECT drawToolRect {};
    RECT panToolRect {};
    RECT eraseToolRect {};
    std::vector<RECT> colorRects;
};

struct SettingsLayout {
    RECT panelRect {};
    std::vector<RECT> paletteRects;
    RECT pasteHexRect {};
    RECT addColorRect {};
    RECT removeColorRect {};
    RECT redTrack {};
    RECT greenTrack {};
    RECT blueTrack {};
    RECT brushSizeTrack {};
    RECT stabilizationTrack {};
    RECT spacingTrack {};
    RECT smoothingTrack {};
};

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect { left, top, right, bottom };
    return rect;
}

std::wstring BaseName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }

    return path.substr(slash + 1);
}

ToolbarLayout BuildToolbarLayout(RECT clientRect, std::size_t colorCount) {
    ToolbarLayout layout;

    const int rightEdge = static_cast<int>(clientRect.right) - kToolbarPadding;
    const int toolbarTop = 10;
    const int swatchBandWidth = static_cast<int>(colorCount) * kColorSwatchSize
        + static_cast<int>(colorCount > 0 ? colorCount - 1U : 0U) * kColorSwatchGap;
    const int colorsLeft = rightEdge - swatchBandWidth;

    layout.menu.fileRect = MakeRect(14, toolbarTop + 4, 58, toolbarTop + 28);
    layout.menu.editRect = MakeRect(layout.menu.fileRect.right + 8, toolbarTop + 4, layout.menu.fileRect.right + 52, toolbarTop + 28);
    layout.menu.viewRect = MakeRect(layout.menu.editRect.right + 8, toolbarTop + 4, layout.menu.editRect.right + 54, toolbarTop + 28);
    layout.previousPageRect = MakeRect(layout.menu.viewRect.right + 20, toolbarTop, layout.menu.viewRect.right + 52, toolbarTop + kToolButtonHeight);
    layout.pageLabelRect = MakeRect(layout.previousPageRect.right + 8, toolbarTop + 2, layout.previousPageRect.right + 92, toolbarTop + kToolButtonHeight - 2);
    layout.nextPageRect = MakeRect(layout.pageLabelRect.right + 8, toolbarTop, layout.pageLabelRect.right + 40, toolbarTop + kToolButtonHeight);

    layout.colorRects.reserve(colorCount);
    for (std::size_t index = 0; index < colorCount; ++index) {
        const int left = colorsLeft + static_cast<int>(index) * (kColorSwatchSize + kColorSwatchGap);
        layout.colorRects.push_back(MakeRect(left, toolbarTop + 4, left + kColorSwatchSize, toolbarTop + 4 + kColorSwatchSize));
    }

    const int toolBandRight = colorsLeft - 24;
    layout.panToolRect = MakeRect(
        toolBandRight - kToolButtonWidth,
        toolbarTop,
        toolBandRight,
        toolbarTop + kToolButtonHeight);
    layout.drawToolRect = MakeRect(
        layout.panToolRect.left - 10 - kToolButtonWidth,
        toolbarTop,
        layout.panToolRect.left - 10,
        toolbarTop + kToolButtonHeight);
    layout.eraseToolRect = MakeRect(
        layout.drawToolRect.left - 10 - kToolButtonWidth,
        toolbarTop,
        layout.drawToolRect.left - 10,
        toolbarTop + kToolButtonHeight);
    layout.redoRect = MakeRect(
        layout.eraseToolRect.left - 22 - 72,
        toolbarTop,
        layout.eraseToolRect.left - 22,
        toolbarTop + kToolButtonHeight);
    layout.undoRect = MakeRect(
        layout.redoRect.left - 10 - 72,
        toolbarTop,
        layout.redoRect.left - 10,
        toolbarTop + kToolButtonHeight);

    return layout;
}

SettingsLayout BuildSettingsLayout(RECT clientRect, std::size_t colorCount) {
    SettingsLayout layout;
    const int panelWidth = 320;
    const int panelLeft = std::max(24, static_cast<int>(clientRect.right) - panelWidth - 24);
    const int panelTop = 54 + 10;
    layout.panelRect = MakeRect(panelLeft, panelTop, panelLeft + panelWidth, panelTop + 418);

    const int contentLeft = panelLeft + 18;
    const int sliderLeft = contentLeft;
    const int sliderRight = layout.panelRect.right - 18;
    const int sliderHeight = 10;

    layout.paletteRects.reserve(colorCount);
    const int paletteTop = panelTop + 48;
    for (std::size_t index = 0; index < colorCount; ++index) {
        const int column = static_cast<int>(index % 6U);
        const int row = static_cast<int>(index / 6U);
        const int left = contentLeft + (column * 44);
        const int top = paletteTop + (row * 34);
        layout.paletteRects.push_back(MakeRect(left, top, left + 36, top + 26));
    }

    const int paletteRows = static_cast<int>((colorCount + 5U) / 6U);
    const int actionsTop = paletteTop + (std::max(1, paletteRows) * 34) + 4;
    layout.pasteHexRect = MakeRect(contentLeft, actionsTop, contentLeft + 118, actionsTop + 28);
    layout.addColorRect = MakeRect(layout.pasteHexRect.right + 10, actionsTop, layout.pasteHexRect.right + 54, actionsTop + 28);
    layout.removeColorRect = MakeRect(layout.addColorRect.right + 10, actionsTop, layout.addColorRect.right + 54, actionsTop + 28);

    const int sliderStart = actionsTop + 54;
    layout.redTrack = MakeRect(sliderLeft, sliderStart, sliderRight, sliderStart + sliderHeight);
    layout.greenTrack = MakeRect(sliderLeft, sliderStart + 36, sliderRight, sliderStart + 36 + sliderHeight);
    layout.blueTrack = MakeRect(sliderLeft, sliderStart + 72, sliderRight, sliderStart + 72 + sliderHeight);
    layout.brushSizeTrack = MakeRect(sliderLeft, sliderStart + 118, sliderRight, sliderStart + 118 + sliderHeight);
    layout.stabilizationTrack = MakeRect(sliderLeft, sliderStart + 164, sliderRight, sliderStart + 164 + sliderHeight);
    layout.spacingTrack = MakeRect(sliderLeft, sliderStart + 200, sliderRight, sliderStart + 200 + sliderHeight);
    layout.smoothingTrack = MakeRect(sliderLeft, sliderStart + 236, sliderRight, sliderStart + 236 + sliderHeight);

    layout.panelRect.bottom = layout.smoothingTrack.bottom + 24;

    const int maxBottom = static_cast<int>(clientRect.bottom) - 12;
    if (layout.panelRect.bottom > maxBottom) {
        const int minTop = 62;
        const int availableShift = std::max(0, static_cast<int>(layout.panelRect.top) - minTop);
        const int overflow = static_cast<int>(layout.panelRect.bottom) - maxBottom;
        const int shift = std::min(overflow, availableShift);
        OffsetRect(&layout.panelRect, 0, -shift);
        OffsetRect(&layout.pasteHexRect, 0, -shift);
        OffsetRect(&layout.addColorRect, 0, -shift);
        OffsetRect(&layout.removeColorRect, 0, -shift);
        OffsetRect(&layout.redTrack, 0, -shift);
        OffsetRect(&layout.greenTrack, 0, -shift);
        OffsetRect(&layout.blueTrack, 0, -shift);
        OffsetRect(&layout.brushSizeTrack, 0, -shift);
        OffsetRect(&layout.stabilizationTrack, 0, -shift);
        OffsetRect(&layout.spacingTrack, 0, -shift);
        OffsetRect(&layout.smoothingTrack, 0, -shift);
        for (RECT& rect : layout.paletteRects) {
            OffsetRect(&rect, 0, -shift);
        }
    }

    return layout;
}

void FillRoundedRect(HDC hdc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
}

void StrokeRoundedRect(HDC hdc, RECT rect, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

float DistanceSquared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

Vec2 Lerp(Vec2 from, Vec2 to, float amount) {
    return {
        from.x + ((to.x - from.x) * amount),
        from.y + ((to.y - from.y) * amount),
    };
}

std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength) {
    if (points.size() <= 2U || strength <= 0.001f) {
        return points;
    }

    std::vector<Vec2> smoothed;
    smoothed.reserve(points.size());
    smoothed.push_back(points.front());

    const float sideWeight = 0.25f * std::clamp(strength, 0.0f, 1.0f);
    const float centerWeight = 1.0f - (sideWeight * 2.0f);

    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const Vec2 previous = points[index - 1];
        const Vec2 current = points[index];
        const Vec2 next = points[index + 1];
        const Vec2 filtered {
            (previous.x * sideWeight) + (current.x * centerWeight) + (next.x * sideWeight),
            (previous.y * sideWeight) + (current.y * centerWeight) + (next.y * sideWeight),
        };

        if (DistanceSquared(filtered, smoothed.back()) >= 0.05f) {
            smoothed.push_back(filtered);
        }
    }

    if (DistanceSquared(points.back(), smoothed.back()) >= 0.01f) {
        smoothed.push_back(points.back());
    }

    return smoothed;
}

std::wstring Trimmed(std::wstring value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](wchar_t ch) { return !iswspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](wchar_t ch) { return !iswspace(ch); }).base(), value.end());
    return value;
}

std::optional<std::uint32_t> ParseHexColor(std::wstring value) {
    value = Trimmed(std::move(value));
    if (value.empty()) {
        return std::nullopt;
    }

    if (value.starts_with(L"#")) {
        value.erase(0, 1);
    }
    if (value.starts_with(L"0x") || value.starts_with(L"0X")) {
        value.erase(0, 2);
    }

    if (value.size() != 6U && value.size() != 8U) {
        return std::nullopt;
    }

    for (wchar_t ch : value) {
        if (!iswxdigit(ch)) {
            return std::nullopt;
        }
    }

    const unsigned long parsed = std::wcstoul(value.c_str(), nullptr, 16);
    if (value.size() == 6U) {
        return 0xFF000000U | static_cast<std::uint32_t>(parsed);
    }

    return static_cast<std::uint32_t>(parsed);
}

}  // namespace

App::App(HINSTANCE instance) : instance_(instance) {
    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) == Gdiplus::Ok) {
        gdiplusToken_ = token;
    }
}

App::~App() {
    if (gdiplusToken_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
    }
}

int App::Run(int showCommand) {
    if (!CreateMainWindow(showCommand)) {
        return 1;
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
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&windowClass) == 0) {
        return false;
    }

    window_ = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1400,
        960,
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
    DwmSetWindowAttribute(window_, kDwmDarkModeAttribute, &useDarkMode, sizeof(useDarkMode));
    DwmSetWindowAttribute(window_, kDwmBorderColorAttribute, &chromeColor, sizeof(chromeColor));
    DwmSetWindowAttribute(window_, kDwmCaptionColorAttribute, &chromeColor, sizeof(chromeColor));
    DwmSetWindowAttribute(window_, kDwmTextColorAttribute, &chromeText, sizeof(chromeText));
    UpdateWindowTitle();

    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

void App::CreateBlankDocument() {
    document_.ResetToBlank();
    pageSurfaceCaches_.assign(document_.PageCount(), {});
    ClearHistory();
    brushWidth_ = 3.0f;
    ResetView();
    UpdateWindowTitle();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::ClearHistory() {
    undoActions_.clear();
    redoActions_.clear();
}

void App::InvalidateCanvas() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    clientRect.top = kToolbarHeight;
    InvalidateRect(window_, &clientRect, FALSE);
}

void App::InvalidatePageSurfaceCache(std::size_t pageIndex) {
    if (pageIndex >= pageSurfaceCaches_.size()) {
        return;
    }

    PageSurfaceCache& cache = pageSurfaceCaches_[pageIndex];
    cache.valid = false;
    cache.pixels.clear();
    cache.width = 0;
    cache.height = 0;
    cache.stride = 0;
    cache.rasterWidth = 0;
    cache.rasterHeight = 0;
    cache.rasterScale = 0.0f;
    cache.pageScale = 0.0f;
    cache.strokeCount = 0;
}

void App::InvalidateAllPageSurfaceCaches() {
    for (std::size_t pageIndex = 0; pageIndex < pageSurfaceCaches_.size(); ++pageIndex) {
        InvalidatePageSurfaceCache(pageIndex);
    }
}

void App::InvalidateStrokeRegion(const Stroke& stroke, std::size_t pageIndex, bool smoothLiveStroke) {
    if (window_ == nullptr || stroke.points.empty()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    if (!pageView.valid) {
        return;
    }

    const std::vector<Vec2> renderPoints = smoothLiveStroke
        ? BuildSmoothedPoints(stroke.points, strokeSmoothing_)
        : stroke.points;
    if (renderPoints.empty()) {
        return;
    }

    float minX = renderPoints.front().x;
    float minY = renderPoints.front().y;
    float maxX = renderPoints.front().x;
    float maxY = renderPoints.front().y;
    for (const Vec2& point : renderPoints) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }

    const float penWidth = std::max(1.25f, stroke.style.width * pageView.scale / 2.15f);
    const int padding = static_cast<int>(std::ceil(penWidth)) + 12;
    RECT dirtyRect = MakeRect(
        pageView.rect.left + static_cast<int>(std::floor(minX * pageView.scale)) - padding,
        pageView.rect.top + static_cast<int>(std::floor(minY * pageView.scale)) - padding,
        pageView.rect.left + static_cast<int>(std::ceil(maxX * pageView.scale)) + padding + 1,
        pageView.rect.top + static_cast<int>(std::ceil(maxY * pageView.scale)) + padding + 1);
    InvalidateRect(window_, &dirtyRect, FALSE);
}

void App::ResetView() {
    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
}

void App::SetActiveTool(Tool tool) {
    activeTool_ = tool;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::PreviousPage() {
    if (CanGoToPreviousPage()) {
        FocusPage(document_.CurrentPageIndex() - 1U);
    }
}

void App::NextPage() {
    if (CanGoToNextPage()) {
        FocusPage(document_.CurrentPageIndex() + 1U);
    }
}

void App::SetCurrentPageIndex(std::size_t index) {
    if (document_.SetCurrentPageIndex(index)) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::FocusPage(std::size_t index) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, index);
    if (!pageView.valid) {
        return;
    }

    document_.SetCurrentPageIndex(index);
    const int canvasTop = kToolbarHeight;
    const int canvasHeight = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top) - kToolbarHeight);
    const int pageCenterY = (pageView.rect.top + pageView.rect.bottom) / 2;
    const int desiredCenterY = canvasTop + (canvasHeight / 2);
    panY_ += static_cast<float>(desiredCenterY - pageCenterY);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::ToggleBrushSettings() {
    settingsOpen_ = !settingsOpen_;
    activeSlider_ = SliderTarget::None;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::Undo() {
    if (isDrawing_ || isPanning_ || !CanUndo()) {
        return;
    }

    HistoryAction action = undoActions_.back();
    undoActions_.pop_back();
    auto& strokes = document_.PageAt(action.pageIndex).strokes;
    document_.SetCurrentPageIndex(action.pageIndex);

    if (action.type == HistoryActionType::AddStroke) {
        if (action.index < strokes.size()) {
            strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(action.index));
        }
    } else if (action.index <= strokes.size()) {
        strokes.insert(strokes.begin() + static_cast<std::ptrdiff_t>(action.index), action.stroke);
    }

    redoActions_.push_back(std::move(action));
    InvalidatePageSurfaceCache(redoActions_.back().pageIndex);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::Redo() {
    if (isDrawing_ || isPanning_ || !CanRedo()) {
        return;
    }

    HistoryAction action = redoActions_.back();
    redoActions_.pop_back();
    auto& strokes = document_.PageAt(action.pageIndex).strokes;
    document_.SetCurrentPageIndex(action.pageIndex);

    if (action.type == HistoryActionType::AddStroke) {
        if (action.index <= strokes.size()) {
            strokes.insert(strokes.begin() + static_cast<std::ptrdiff_t>(action.index), action.stroke);
        }
    } else if (action.index < strokes.size()) {
        strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(action.index));
    }

    undoActions_.push_back(std::move(action));
    InvalidatePageSurfaceCache(undoActions_.back().pageIndex);
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::CanUndo() const {
    return !undoActions_.empty();
}

bool App::CanRedo() const {
    return !redoActions_.empty();
}

bool App::CanGoToPreviousPage() const {
    return document_.CurrentPageIndex() > 0U;
}

bool App::CanGoToNextPage() const {
    return document_.CurrentPageIndex() + 1U < document_.PageCount();
}

void App::AddPaletteColor() {
    if (palette_.size() >= 12U) {
        return;
    }

    const std::uint32_t sourceColor = palette_.at(settingsPaletteIndex_);
    palette_.push_back(sourceColor);
    settingsPaletteIndex_ = palette_.size() - 1U;
    colorIndex_ = std::min(colorIndex_, palette_.size() - 1U);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::RemovePaletteColor() {
    if (palette_.size() <= 1U || settingsPaletteIndex_ >= palette_.size()) {
        return;
    }

    palette_.erase(palette_.begin() + static_cast<std::ptrdiff_t>(settingsPaletteIndex_));
    if (settingsPaletteIndex_ >= palette_.size()) {
        settingsPaletteIndex_ = palette_.size() - 1U;
    }
    if (colorIndex_ >= palette_.size()) {
        colorIndex_ = palette_.size() - 1U;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::ApplyHexColorFromClipboard() {
    if (!OpenClipboard(window_)) {
        return false;
    }

    bool applied = false;
    HANDLE clipboardData = GetClipboardData(CF_UNICODETEXT);
    if (clipboardData != nullptr) {
        const auto* rawText = static_cast<const wchar_t*>(GlobalLock(clipboardData));
        if (rawText != nullptr) {
            if (const auto parsed = ParseHexColor(rawText); parsed.has_value() && settingsPaletteIndex_ < palette_.size()) {
                palette_[settingsPaletteIndex_] = *parsed;
                applied = true;
            }
            GlobalUnlock(clipboardData);
        }
    }

    CloseClipboard();
    if (applied) {
        InvalidateRect(window_, nullptr, FALSE);
    }
    return applied;
}

std::wstring App::CurrentWindowTitle() const {
    if (!document_.SourcePath().empty()) {
        return BaseName(document_.SourcePath());
    }

    return L"Untitled";
}

std::wstring App::PageIndicatorLabel() const {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"%zu / %zu", document_.CurrentPageIndex() + 1U, document_.PageCount());
    return buffer;
}

void App::UpdateWindowTitle() {
    if (window_ != nullptr) {
        const std::wstring title = CurrentWindowTitle();
        SetWindowTextW(window_, title.c_str());
    }
}

void App::BeginStroke(POINT cursor) {
    if (isPanning_ || isErasing_) {
        return;
    }

    std::size_t pageIndex = 0;
    const Vec2 pagePoint = ScreenToPage(cursor, &pageIndex);
    if (!HitTestPage(cursor).has_value()) {
        return;
    }

    document_.SetCurrentPageIndex(pageIndex);
    activeStroke_ = Stroke {};
    activeStrokePageIndex_ = pageIndex;
    activeStroke_->style.color = palette_.at(colorIndex_);
    activeStroke_->style.width = brushWidth_;
    activeStroke_->points.push_back(pagePoint);
    isDrawing_ = true;
    UpdateCaptureState();
    InvalidateStrokeRegion(*activeStroke_, pageIndex, true);
}

void App::ExtendStroke(POINT cursor) {
    if (!isDrawing_ || isPanning_ || !activeStroke_.has_value()) {
        return;
    }

    if (!activeStrokePageIndex_.has_value()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *activeStrokePageIndex_);
    const Page& page = document_.PageAt(*activeStrokePageIndex_);
    if (!pageView.valid || pageView.scale <= 0.0f) {
        return;
    }

    const float pageX = (static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale;
    const float pageY = (static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale;
    const Vec2 rawPoint {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
    Vec2 filteredPoint = rawPoint;

    if (!activeStroke_->points.empty()) {
        filteredPoint = Lerp(activeStroke_->points.back(), rawPoint, inputSmoothing_);
        if (DistanceSquared(filteredPoint, activeStroke_->points.back()) < inputSpacing_) {
            return;
        }
    }

    activeStroke_->points.push_back(filteredPoint);
    InvalidateStrokeRegion(*activeStroke_, *activeStrokePageIndex_, true);
}

void App::EndStroke() {
    if (isDrawing_ && activeStroke_.has_value() && activeStrokePageIndex_.has_value() && !activeStroke_->points.empty()) {
        Stroke finishedStroke = std::move(*activeStroke_);
        finishedStroke.points = BuildSmoothedPoints(finishedStroke.points, strokeSmoothing_);
        auto& strokes = document_.PageAt(*activeStrokePageIndex_).strokes;
        const std::size_t insertIndex = strokes.size();
        strokes.push_back(finishedStroke);
        undoActions_.push_back({ HistoryActionType::AddStroke, std::move(finishedStroke), insertIndex, *activeStrokePageIndex_ });
        redoActions_.clear();
        InvalidatePageSurfaceCache(*activeStrokePageIndex_);
    }

    activeStroke_.reset();
    activeStrokePageIndex_.reset();
    isDrawing_ = false;
    UpdateCaptureState();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::BeginErase(POINT cursor) {
    if (isPanning_) {
        return;
    }

    isErasing_ = true;
    UpdateCaptureState();
    static_cast<void>(EraseAt(cursor));
}

void App::ExtendErase(POINT cursor) {
    if (!isErasing_) {
        return;
    }

    static_cast<void>(EraseAt(cursor));
}

bool App::EraseAt(POINT cursor) {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        return false;
    }

    const PageView pageView = *hitPage;
    if (!pageView.valid || pageView.scale <= 0.0f) {
        return false;
    }

    const Vec2 center = ScreenToPage(cursor, nullptr);
    const float eraseRadiusPage = std::max(10.0f, brushWidth_ * 4.0f) / pageView.scale;
    const float eraseRadiusSquared = eraseRadiusPage * eraseRadiusPage;
    auto& strokes = document_.PageAt(pageView.pageIndex).strokes;
    document_.SetCurrentPageIndex(pageView.pageIndex);

    for (std::size_t strokeIndex = 0; strokeIndex < strokes.size(); ++strokeIndex) {
        const Stroke& stroke = strokes[strokeIndex];
        if (stroke.points.empty()) {
            continue;
        }

        bool hit = DistanceSquared(stroke.points.front(), center) <= eraseRadiusSquared;
        for (std::size_t index = 1; !hit && index < stroke.points.size(); ++index) {
            const Vec2 a = stroke.points[index - 1];
            const Vec2 b = stroke.points[index];
            const float segmentDx = b.x - a.x;
            const float segmentDy = b.y - a.y;
            const float lengthSquared = (segmentDx * segmentDx) + (segmentDy * segmentDy);

            float t = 0.0f;
            if (lengthSquared > 0.0001f) {
                t = ((center.x - a.x) * segmentDx + (center.y - a.y) * segmentDy) / lengthSquared;
                t = std::clamp(t, 0.0f, 1.0f);
            }

            const Vec2 closest {
                a.x + (segmentDx * t),
                a.y + (segmentDy * t),
            };
            hit = DistanceSquared(closest, center) <= eraseRadiusSquared;
        }

        if (hit) {
            Stroke removed = strokes[strokeIndex];
            strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(strokeIndex));
            undoActions_.push_back({ HistoryActionType::RemoveStroke, std::move(removed), strokeIndex, pageView.pageIndex });
            redoActions_.clear();
            InvalidatePageSurfaceCache(pageView.pageIndex);
            InvalidateRect(window_, nullptr, FALSE);
            return true;
        }
    }

    return false;
}

void App::BeginPan(POINT cursor, bool fromMiddleButton) {
    if (isDrawing_) {
        EndStroke();
    }

    isPanning_ = true;
    panFromMiddleButton_ = fromMiddleButton;
    lastPanCursor_ = cursor;
    UpdateCaptureState();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::UpdatePan(POINT cursor) {
    if (!isPanning_) {
        return;
    }

    const float dx = static_cast<float>(cursor.x - lastPanCursor_.x);
    const float dy = static_cast<float>(cursor.y - lastPanCursor_.y);
    lastPanCursor_ = cursor;
    PanBy(dx, dy);
}

void App::EndPan() {
    isPanning_ = false;
    panFromMiddleButton_ = false;
    UpdateCaptureState();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::UpdateCaptureState() {
    if (isDrawing_ || isPanning_) {
        SetCapture(window_);
        return;
    }

    if (isErasing_) {
        SetCapture(window_);
        return;
    }

    if (GetCapture() == window_) {
        ReleaseCapture();
    }
}

void App::ExecuteCommand(unsigned int commandId) {
    switch (commandId) {
    case kCommandFileNew:
        CreateBlankDocument();
        break;
    case kCommandFileOpen:
        OpenPdf();
        break;
    case kCommandFileExit:
        PostMessageW(window_, WM_CLOSE, 0, 0);
        break;
    case kCommandEditUndo:
        Undo();
        break;
    case kCommandEditRedo:
        Redo();
        break;
    case kCommandEditBrushSettings:
        ToggleBrushSettings();
        break;
    case kCommandViewReset:
        ResetView();
        EnsureVisiblePageRasterQuality();
        InvalidateRect(window_, nullptr, FALSE);
        break;
    case kCommandViewPreviousPage:
        PreviousPage();
        break;
    case kCommandViewNextPage:
        NextPage();
        break;
    default:
        break;
    }
}

bool App::HandleToolbarClick(POINT cursor) {
    if (cursor.y >= kToolbarHeight) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const ToolbarLayout layout = BuildToolbarLayout(clientRect, palette_.size());

    const auto openMenu = [&](RECT rect, unsigned int kind) {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) {
            return;
        }

        switch (kind) {
        case 0:
            AppendMenuW(menu, MF_STRING, kCommandFileNew, L"New");
            AppendMenuW(menu, MF_STRING, kCommandFileOpen, L"Open PDF...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCommandFileExit, L"Exit");
            break;
        case 1:
            AppendMenuW(menu, MF_STRING | (CanUndo() ? 0 : MF_GRAYED), kCommandEditUndo, L"Undo");
            AppendMenuW(menu, MF_STRING | (CanRedo() ? 0 : MF_GRAYED), kCommandEditRedo, L"Redo");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCommandEditBrushSettings, settingsOpen_ ? L"Hide Brush Settings" : L"Brush Settings");
            break;
        case 2:
            AppendMenuW(menu, MF_STRING, kCommandViewReset, L"Reset View");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING | (CanGoToPreviousPage() ? 0 : MF_GRAYED), kCommandViewPreviousPage, L"Previous Page");
            AppendMenuW(menu, MF_STRING | (CanGoToNextPage() ? 0 : MF_GRAYED), kCommandViewNextPage, L"Next Page");
            break;
        default:
            DestroyMenu(menu);
            return;
        }

        POINT anchor { rect.left, rect.bottom + 2 };
        ClientToScreen(window_, &anchor);
        const unsigned int selected = TrackPopupMenu(
            menu,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
            anchor.x,
            anchor.y,
            0,
            window_,
            nullptr);
        DestroyMenu(menu);

        if (selected != 0U) {
            ExecuteCommand(selected);
        }
    };

    if (PtInRect(&layout.menu.fileRect, cursor) != 0) {
        openMenu(layout.menu.fileRect, 0);
        return true;
    }

    if (PtInRect(&layout.menu.editRect, cursor) != 0) {
        openMenu(layout.menu.editRect, 1);
        return true;
    }

    if (PtInRect(&layout.menu.viewRect, cursor) != 0) {
        openMenu(layout.menu.viewRect, 2);
        return true;
    }

    if (PtInRect(&layout.previousPageRect, cursor) != 0) {
        PreviousPage();
        return true;
    }

    if (PtInRect(&layout.nextPageRect, cursor) != 0) {
        NextPage();
        return true;
    }

    if (PtInRect(&layout.drawToolRect, cursor) != 0) {
        SetActiveTool(Tool::Draw);
        return true;
    }

    if (PtInRect(&layout.panToolRect, cursor) != 0) {
        SetActiveTool(Tool::Pan);
        return true;
    }

    if (PtInRect(&layout.eraseToolRect, cursor) != 0) {
        SetActiveTool(Tool::Erase);
        return true;
    }

    if (PtInRect(&layout.undoRect, cursor) != 0) {
        Undo();
        return true;
    }

    if (PtInRect(&layout.redoRect, cursor) != 0) {
        Redo();
        return true;
    }

    for (std::size_t index = 0; index < layout.colorRects.size(); ++index) {
        if (PtInRect(&layout.colorRects[index], cursor) != 0) {
            SetColorIndex(index);
            return true;
        }
    }

    return true;
}

bool App::HandleSettingsClick(POINT cursor) {
    if (!settingsOpen_) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsLayout layout = BuildSettingsLayout(clientRect, palette_.size());
    if (PtInRect(&layout.panelRect, cursor) == 0) {
        return false;
    }

    for (std::size_t index = 0; index < layout.paletteRects.size(); ++index) {
        if (PtInRect(&layout.paletteRects[index], cursor) != 0) {
            settingsPaletteIndex_ = index;
            InvalidateRect(window_, nullptr, FALSE);
            return true;
        }
    }

    if (PtInRect(&layout.pasteHexRect, cursor) != 0) {
        static_cast<void>(ApplyHexColorFromClipboard());
        return true;
    }

    if (PtInRect(&layout.addColorRect, cursor) != 0) {
        AddPaletteColor();
        return true;
    }

    if (PtInRect(&layout.removeColorRect, cursor) != 0) {
        RemovePaletteColor();
        return true;
    }

    const auto beginSlider = [&](SliderTarget target, const RECT& track) -> bool {
        RECT expanded = track;
        expanded.top -= 8;
        expanded.bottom += 8;
        if (PtInRect(&expanded, cursor) != 0) {
            activeSlider_ = target;
            UpdateActiveSlider(cursor);
            return true;
        }
        return false;
    };

    if (beginSlider(SliderTarget::Red, layout.redTrack)
        || beginSlider(SliderTarget::Green, layout.greenTrack)
        || beginSlider(SliderTarget::Blue, layout.blueTrack)
        || beginSlider(SliderTarget::BrushSize, layout.brushSizeTrack)
        || beginSlider(SliderTarget::Stabilization, layout.stabilizationTrack)
        || beginSlider(SliderTarget::Spacing, layout.spacingTrack)
        || beginSlider(SliderTarget::Smoothing, layout.smoothingTrack)) {
        return true;
    }

    return true;
}

void App::UpdateActiveSlider(POINT cursor) {
    if (activeSlider_ == SliderTarget::None) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsLayout layout = BuildSettingsLayout(clientRect, palette_.size());

    const auto sliderValue = [&](const RECT& track) {
        const float width = static_cast<float>(std::max(1L, track.right - track.left));
        return std::clamp((static_cast<float>(cursor.x - track.left) / width), 0.0f, 1.0f);
    };

    std::uint32_t color = palette_.at(settingsPaletteIndex_);
    unsigned char red = GetColorChannel(color, 16);
    unsigned char green = GetColorChannel(color, 8);
    unsigned char blue = GetColorChannel(color, 0);

    switch (activeSlider_) {
    case SliderTarget::Red:
        red = static_cast<unsigned char>(std::lround(sliderValue(layout.redTrack) * 255.0f));
        palette_[settingsPaletteIndex_] = ComposeColor(red, green, blue);
        break;
    case SliderTarget::Green:
        green = static_cast<unsigned char>(std::lround(sliderValue(layout.greenTrack) * 255.0f));
        palette_[settingsPaletteIndex_] = ComposeColor(red, green, blue);
        break;
    case SliderTarget::Blue:
        blue = static_cast<unsigned char>(std::lround(sliderValue(layout.blueTrack) * 255.0f));
        palette_[settingsPaletteIndex_] = ComposeColor(red, green, blue);
        break;
    case SliderTarget::BrushSize:
        brushWidth_ = 1.0f + (sliderValue(layout.brushSizeTrack) * 23.0f);
        break;
    case SliderTarget::Stabilization:
        inputSmoothing_ = std::clamp(sliderValue(layout.stabilizationTrack), 0.05f, 0.95f);
        break;
    case SliderTarget::Spacing:
        inputSpacing_ = 0.05f + (sliderValue(layout.spacingTrack) * 4.0f);
        break;
    case SliderTarget::Smoothing:
        strokeSmoothing_ = sliderValue(layout.smoothingTrack);
        break;
    case SliderTarget::None:
        break;
    }

    InvalidateRect(window_, nullptr, FALSE);
}

bool App::IsCursorInsideSettingsPanel(POINT cursor) const {
    if (!settingsOpen_) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsLayout layout = BuildSettingsLayout(clientRect, palette_.size());
    return PtInRect(&layout.panelRect, cursor) != 0;
}

void App::PanBy(float dx, float dy) {
    panX_ += dx;
    panY_ += dy;
    InvalidateCanvas();
}

void App::ZoomAt(int delta, POINT cursor) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const auto hitPage = HitTestPage(cursor);
    const float factor = delta > 0 ? 1.1f : 0.9f;
    const float newZoom = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);

    if (std::abs(newZoom - zoom_) < 0.0001f) {
        return;
    }

    std::size_t anchorPageIndex = document_.CurrentPageIndex();
    Vec2 anchorPage {};
    const bool cursorOnPage = hitPage.has_value();
    if (cursorOnPage) {
        anchorPageIndex = hitPage->pageIndex;
        anchorPage = ScreenToPage(cursor, nullptr);
    }

    zoom_ = newZoom;

    if (cursorOnPage) {
        const PageView after = CalculatePageView(clientRect, anchorPageIndex);
        if (after.valid) {
            const POINT anchoredScreen = PageToScreen(anchorPageIndex, anchorPage);
            panX_ += static_cast<float>(cursor.x - anchoredScreen.x);
            panY_ += static_cast<float>(cursor.y - anchoredScreen.y);
        }
    }

    EnsureVisiblePageRasterQuality();
    InvalidateCanvas();
}

void App::ChangeBrushSize(float delta) {
    brushWidth_ = std::clamp(brushWidth_ + delta, 1.0f, 24.0f);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::CycleColor() {
    SetColorIndex((colorIndex_ + 1U) % palette_.size());
}

void App::SetColorIndex(std::size_t index) {
    if (index >= palette_.size()) {
        return;
    }

    colorIndex_ = index;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::OpenPdf() {
    wchar_t fileBuffer[MAX_PATH] = {};
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    dialog.lpstrFile = fileBuffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"pdf";

    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    const auto opened = pdfBackend_.Open(fileBuffer);
    if (!opened.has_value()) {
        MessageBoxW(window_, L"Failed to open or render the PDF.", L"Open PDF", MB_OK | MB_ICONERROR);
        return;
    }

    document_ = *opened;
    pageSurfaceCaches_.assign(document_.PageCount(), {});
    ClearHistory();
    ResetView();
    EnsureVisiblePageRasterQuality();
    UpdateWindowTitle();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::EnsureVisiblePageRasterQuality() {
    if (!document_.IsPdfDocument() || document_.SourcePath().empty()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const int visibleTop = kToolbarHeight - 200;
    const int visibleBottom = static_cast<int>(clientRect.bottom) + 200;
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom < visibleTop || pageView.rect.top > visibleBottom) {
            continue;
        }
        EnsurePageRasterQuality(pageView);
    }
}

void App::EnsurePageRasterQuality(const PageView& pageView) {
    if (!document_.IsPdfDocument() || document_.SourcePath().empty() || !pageView.valid) {
        return;
    }

    Page& page = document_.PageAt(pageView.pageIndex);
    const float desiredScale = DesiredRenderScale(pageView);
    if (desiredScale <= 0.0f) {
        return;
    }

    const float currentScale = page.raster.scale;
    const bool missingRaster = page.raster.pixels.empty() || page.raster.width <= 0 || page.raster.height <= 0;
    const bool needsUpgrade = missingRaster || currentScale <= 0.0f || desiredScale > (currentScale * 1.2f);
    if (!needsUpgrade) {
        return;
    }

    const auto rerendered = pdfBackend_.RenderPage(document_.SourcePath(), pageView.pageIndex, desiredScale);
    if (rerendered.has_value()) {
        page.raster = std::move(*rerendered);
        InvalidatePageSurfaceCache(pageView.pageIndex);
    }
}

bool App::IsCursorInsidePage(POINT cursor) const {
    return HitTestPage(cursor).has_value();
}

std::optional<App::PageView> App::HitTestPage(POINT cursor) const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (pageView.valid && PtInRect(&pageView.rect, cursor) != 0) {
            return pageView;
        }
    }
    return std::nullopt;
}

Vec2 App::ScreenToPage(POINT cursor, std::size_t* pageIndex) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value() || hitPage->scale <= 0.0f) {
        if (pageIndex != nullptr) {
            *pageIndex = document_.CurrentPageIndex();
        }
        return {};
    }

    if (pageIndex != nullptr) {
        *pageIndex = hitPage->pageIndex;
    }

    const Page& page = document_.PageAt(hitPage->pageIndex);
    const float pageX = (static_cast<float>(cursor.x - hitPage->rect.left)) / hitPage->scale;
    const float pageY = (static_cast<float>(cursor.y - hitPage->rect.top)) / hitPage->scale;
    return {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
}

POINT App::PageToScreen(std::size_t pageIndex, Vec2 point) const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    POINT output {};
    output.x = pageView.rect.left + static_cast<LONG>(std::lround(point.x * pageView.scale));
    output.y = pageView.rect.top + static_cast<LONG>(std::lround(point.y * pageView.scale));
    return output;
}

App::PageView App::CalculatePageView(RECT clientRect, std::size_t pageIndex) const {
    const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
    if (pageIndex >= pageViews.size()) {
        return {};
    }

    return pageViews[pageIndex];
}

std::vector<App::PageView> App::CalculatePageViews(RECT clientRect) const {
    const int clientWidth = static_cast<int>(clientRect.right - clientRect.left);
    const int canvasWidth = std::max(1, clientWidth);
    float widestPage = 1.0f;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        widestPage = std::max(widestPage, document_.PageAt(pageIndex).width);
    }

    const float fitScale = static_cast<float>(canvasWidth - 120) / widestPage;
    const float scale = std::max(0.1f, fitScale * zoom_);
    const int canvasCenterX = canvasWidth / 2;
    int top = kToolbarHeight + 40 + static_cast<int>(std::lround(panY_));

    std::vector<PageView> pageViews;
    pageViews.reserve(document_.PageCount());
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const Page& page = document_.PageAt(pageIndex);
        const int pageWidth = static_cast<int>(std::lround(page.width * scale));
        const int pageHeight = static_cast<int>(std::lround(page.height * scale));
        const int left = static_cast<int>(std::lround(canvasCenterX - (pageWidth / 2.0f) + panX_));
        pageViews.push_back({
            MakeRect(left, top, left + pageWidth, top + pageHeight),
            pageIndex,
            scale,
            pageWidth > 0 && pageHeight > 0,
        });
        top += pageHeight + kPageGap;
    }

    return pageViews;
}

float App::DesiredRenderScale(const PageView& pageView) const {
    if (!pageView.valid) {
        return 0.0f;
    }

    constexpr float kRenderOverscan = 1.2f;
    constexpr float kMinRenderScale = 1.5f;
    constexpr float kMaxRenderScale = 6.0f;
    return std::clamp(pageView.scale * kRenderOverscan, kMinRenderScale, kMaxRenderScale);
}

COLORREF App::ToColorRef(std::uint32_t argb) const {
    const auto red = static_cast<unsigned char>((argb >> 16U) & 0xFFU);
    const auto green = static_cast<unsigned char>((argb >> 8U) & 0xFFU);
    const auto blue = static_cast<unsigned char>(argb & 0xFFU);
    return RGB(red, green, blue);
}

std::uint32_t App::ComposeColor(unsigned char red, unsigned char green, unsigned char blue) const {
    return 0xFF000000U | (static_cast<std::uint32_t>(red) << 16U) | (static_cast<std::uint32_t>(green) << 8U)
        | static_cast<std::uint32_t>(blue);
}

unsigned char App::GetColorChannel(std::uint32_t color, int shift) const {
    return static_cast<unsigned char>((color >> shift) & 0xFFU);
}

void App::Paint() {
    PAINTSTRUCT paint {};
    HDC windowDc = BeginPaint(window_, &paint);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);

    HDC memoryDc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, clientRect.right, clientRect.bottom);
    HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);

    HBRUSH backgroundBrush = CreateSolidBrush(RGB(18, 22, 29));
    FillRect(memoryDc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    PaintToolbar(memoryDc, clientRect);
    PaintCanvas(memoryDc, clientRect);
    PaintSettingsPanel(memoryDc, clientRect);

    BitBlt(windowDc, 0, 0, clientRect.right, clientRect.bottom, memoryDc, 0, 0, SRCCOPY);

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);

    EndPaint(window_, &paint);
}

void App::PaintToolbar(HDC hdc, RECT clientRect) const {
    RECT toolbarRect = clientRect;
    toolbarRect.bottom = kToolbarHeight;

    const COLORREF toolbarBackground = RGB(15, 18, 24);
    const COLORREF panelBorder = RGB(43, 52, 64);
    const COLORREF primaryText = RGB(236, 240, 245);
    const COLORREF selectedFill = RGB(48, 68, 94);
    const COLORREF idleFill = RGB(28, 34, 43);

    HBRUSH toolbarBrush = CreateSolidBrush(toolbarBackground);
    FillRect(hdc, &toolbarRect, toolbarBrush);
    DeleteObject(toolbarBrush);

    HPEN dividerPen = CreatePen(PS_SOLID, 1, panelBorder);
    HGDIOBJ oldPen = SelectObject(hdc, dividerPen);
    MoveToEx(hdc, 0, kToolbarHeight - 1, nullptr);
    LineTo(hdc, clientRect.right, kToolbarHeight - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(dividerPen);

    SetBkMode(hdc, TRANSPARENT);

    HFONT bodyFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT labelFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    const ToolbarLayout layout = BuildToolbarLayout(clientRect, palette_.size());

    HGDIOBJ previousFont = SelectObject(hdc, labelFont);
    SetTextColor(hdc, primaryText);
    RECT fileRect = layout.menu.fileRect;
    RECT editRect = layout.menu.editRect;
    RECT viewRect = layout.menu.viewRect;
    DrawTextW(hdc, L"File", -1, &fileRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(hdc, L"Edit", -1, &editRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(hdc, L"View", -1, &viewRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const auto paintToolButton = [&](RECT rect, const wchar_t* label, bool selected, bool enabled = true) {
        FillRoundedRect(hdc, rect, selected ? selectedFill : idleFill);
        StrokeRoundedRect(hdc, rect, selected ? RGB(92, 137, 187) : panelBorder, 1);
        SetTextColor(hdc, enabled ? (selected ? RGB(240, 247, 255) : primaryText) : RGB(102, 111, 124));
        SelectObject(hdc, bodyFont);
        RECT labelRect = rect;
        DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    paintToolButton(layout.undoRect, L"Undo", false, CanUndo());
    paintToolButton(layout.redoRect, L"Redo", false, CanRedo());
    paintToolButton(layout.previousPageRect, L"<", false, CanGoToPreviousPage());
    paintToolButton(layout.nextPageRect, L">", false, CanGoToNextPage());
    paintToolButton(layout.eraseToolRect, L"Erase", activeTool_ == Tool::Erase);
    paintToolButton(layout.drawToolRect, L"Draw", activeTool_ == Tool::Draw && !isPanning_);
    paintToolButton(layout.panToolRect, L"Pan", activeTool_ == Tool::Pan || isPanning_);

    FillRoundedRect(hdc, layout.pageLabelRect, RGB(22, 28, 36));
    StrokeRoundedRect(hdc, layout.pageLabelRect, panelBorder, 1);
    SetTextColor(hdc, primaryText);
    SelectObject(hdc, labelFont);
    RECT pageLabelRect = layout.pageLabelRect;
    const std::wstring pageLabel = PageIndicatorLabel();
    DrawTextW(hdc, pageLabel.c_str(), -1, &pageLabelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    for (std::size_t index = 0; index < layout.colorRects.size(); ++index) {
        RECT swatchRect = layout.colorRects[index];
        const bool selected = index == colorIndex_;
        FillRoundedRect(hdc, swatchRect, ToColorRef(palette_[index]));
        StrokeRoundedRect(hdc, swatchRect, selected ? RGB(244, 247, 252) : panelBorder, selected ? 2 : 1);
    }

    SelectObject(hdc, previousFont);
    DeleteObject(bodyFont);
    DeleteObject(labelFont);
}

void App::PaintSettingsPanel(HDC hdc, RECT clientRect) const {
    if (!settingsOpen_) {
        return;
    }

    const SettingsLayout layout = BuildSettingsLayout(clientRect, palette_.size());
    const COLORREF panelFill = RGB(18, 24, 32);
    const COLORREF panelBorder = RGB(46, 58, 74);
    const COLORREF textColor = RGB(235, 240, 246);
    const COLORREF mutedText = RGB(141, 152, 167);

    FillRoundedRect(hdc, layout.panelRect, panelFill);
    StrokeRoundedRect(hdc, layout.panelRect, panelBorder, 1);

    SetBkMode(hdc, TRANSPARENT);
    HFONT titleFont = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT bodyFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HGDIOBJ previousFont = SelectObject(hdc, titleFont);
    SetTextColor(hdc, textColor);
    TextOutW(hdc, layout.panelRect.left + 18, layout.panelRect.top + 18, L"Brush Settings", 14);

    SelectObject(hdc, bodyFont);
    SetTextColor(hdc, mutedText);
    TextOutW(hdc, layout.panelRect.left + 18, layout.panelRect.top + 96, L"Palette", 7);

    for (std::size_t index = 0; index < layout.paletteRects.size(); ++index) {
        RECT slotRect = layout.paletteRects[index];
        FillRoundedRect(hdc, slotRect, ToColorRef(palette_[index]));
        StrokeRoundedRect(hdc, slotRect, index == settingsPaletteIndex_ ? RGB(244, 247, 252) : panelBorder, index == settingsPaletteIndex_ ? 2 : 1);
    }

    const auto paintButton = [&](RECT rect, const wchar_t* label, bool enabled = true) {
        FillRoundedRect(hdc, rect, enabled ? RGB(28, 34, 43) : RGB(24, 28, 35));
        StrokeRoundedRect(hdc, rect, panelBorder, 1);
        SetTextColor(hdc, enabled ? textColor : RGB(102, 111, 124));
        RECT labelRect = rect;
        DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    paintButton(layout.pasteHexRect, L"Paste Hex");
    paintButton(layout.addColorRect, L"+", palette_.size() < 12U);
    paintButton(layout.removeColorRect, L"-", palette_.size() > 1U);

    const auto paintSlider = [&](const wchar_t* label, const RECT& track, float normalized, COLORREF accent, int value) {
        RECT labelRect = MakeRect(track.left, track.top - 24, track.right, track.top - 6);
        SetTextColor(hdc, mutedText);
        DrawTextW(hdc, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        wchar_t valueBuffer[32] = {};
        std::swprintf(valueBuffer, std::size(valueBuffer), L"%d", value);
        SetTextColor(hdc, textColor);
        DrawTextW(hdc, valueBuffer, -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        FillRoundedRect(hdc, track, RGB(33, 40, 52));
        RECT fillRect = track;
        fillRect.right = track.left + static_cast<LONG>(std::lround((track.right - track.left) * std::clamp(normalized, 0.0f, 1.0f)));
        FillRoundedRect(hdc, fillRect, accent);

        RECT knob = MakeRect(fillRect.right - 6, track.top - 5, fillRect.right + 6, track.bottom + 5);
        FillRoundedRect(hdc, knob, RGB(245, 247, 250));
    };

    const std::uint32_t activeColor = palette_[settingsPaletteIndex_];
    paintSlider(L"Red", layout.redTrack, GetColorChannel(activeColor, 16) / 255.0f, RGB(220, 76, 70), GetColorChannel(activeColor, 16));
    paintSlider(L"Green", layout.greenTrack, GetColorChannel(activeColor, 8) / 255.0f, RGB(70, 190, 118), GetColorChannel(activeColor, 8));
    paintSlider(L"Blue", layout.blueTrack, GetColorChannel(activeColor, 0) / 255.0f, RGB(74, 132, 225), GetColorChannel(activeColor, 0));
    paintSlider(L"Brush Size", layout.brushSizeTrack, (brushWidth_ - 1.0f) / 23.0f, RGB(56, 189, 248), static_cast<int>(std::lround(brushWidth_)));
    paintSlider(L"Stabilization", layout.stabilizationTrack, inputSmoothing_, RGB(129, 140, 248), static_cast<int>(std::lround(inputSmoothing_ * 100.0f)));
    paintSlider(L"Point Spacing", layout.spacingTrack, (inputSpacing_ - 0.05f) / 4.0f, RGB(244, 114, 182), static_cast<int>(std::lround(inputSpacing_ * 100.0f)));
    paintSlider(L"Stroke Smoothing", layout.smoothingTrack, strokeSmoothing_, RGB(245, 158, 11), static_cast<int>(std::lround(strokeSmoothing_ * 100.0f)));

    SelectObject(hdc, previousFont);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
}

void App::PaintCanvas(HDC hdc, RECT clientRect) {
    RECT canvasRect = clientRect;
    canvasRect.top = kToolbarHeight;

    HBRUSH canvasBrush = CreateSolidBrush(RGB(22, 26, 33));
    FillRect(hdc, &canvasRect, canvasBrush);
    DeleteObject(canvasBrush);

    SaveDC(hdc);
    IntersectClipRect(hdc, canvasRect.left, canvasRect.top, canvasRect.right, canvasRect.bottom);
    EnsureVisiblePageRasterQuality();
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom < canvasRect.top - 20 || pageView.rect.top > canvasRect.bottom + 20) {
            continue;
        }
        PaintPage(hdc, pageView);
    }
    RestoreDC(hdc, -1);
}

void App::EnsurePageSurfaceCache(const PageView& pageView) {
    if (pageView.pageIndex >= pageSurfaceCaches_.size()) {
        pageSurfaceCaches_.resize(document_.PageCount());
    }

    const Page& page = document_.PageAt(pageView.pageIndex);
    PageSurfaceCache& cache = pageSurfaceCaches_[pageView.pageIndex];
    const int width = pageView.rect.right - pageView.rect.left;
    const int height = pageView.rect.bottom - pageView.rect.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    const bool cacheMatches = cache.valid
        && cache.pageIndex == pageView.pageIndex
        && cache.width == width
        && cache.height == height
        && cache.strokeCount == page.strokes.size()
        && cache.rasterWidth == page.raster.width
        && cache.rasterHeight == page.raster.height
        && std::abs(cache.rasterScale - page.raster.scale) < 0.001f
        && std::abs(cache.pageScale - pageView.scale) < 0.001f;
    if (cacheMatches) {
        return;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC cacheDc = CreateCompatibleDC(nullptr);
    if (cacheDc == nullptr) {
        return;
    }

    void* bits = nullptr;
    HBITMAP cacheBitmap = CreateDIBSection(cacheDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (cacheBitmap == nullptr || bits == nullptr) {
        if (cacheBitmap != nullptr) {
            DeleteObject(cacheBitmap);
        }
        DeleteDC(cacheDc);
        return;
    }

    HGDIOBJ previousBitmap = SelectObject(cacheDc, cacheBitmap);

    RECT cacheRect = MakeRect(0, 0, width, height);
    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(cacheDc, &cacheRect, pageBrush);
    DeleteObject(pageBrush);

    if (!page.raster.pixels.empty() && page.raster.width > 0 && page.raster.height > 0) {
        BITMAPINFO pageBitmapInfo {};
        pageBitmapInfo.bmiHeader.biSize = sizeof(pageBitmapInfo.bmiHeader);
        pageBitmapInfo.bmiHeader.biWidth = page.raster.width;
        pageBitmapInfo.bmiHeader.biHeight = -page.raster.height;
        pageBitmapInfo.bmiHeader.biPlanes = 1;
        pageBitmapInfo.bmiHeader.biBitCount = 32;
        pageBitmapInfo.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(cacheDc, HALFTONE);
        StretchDIBits(
            cacheDc,
            0,
            0,
            width,
            height,
            0,
            0,
            page.raster.width,
            page.raster.height,
            page.raster.pixels.data(),
            &pageBitmapInfo,
            DIB_RGB_COLORS,
            SRCCOPY);
    }

    const PageView cacheView {
        MakeRect(0, 0, width, height),
        pageView.pageIndex,
        pageView.scale,
        true,
    };
    for (const Stroke& stroke : page.strokes) {
        PaintStroke(cacheDc, stroke, cacheView, false);
    }

    const int stride = width * 4;
    const std::size_t totalBytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    cache.pixels.resize(totalBytes);
    std::copy_n(static_cast<const std::uint8_t*>(bits), totalBytes, cache.pixels.data());
    cache.width = width;
    cache.height = height;
    cache.stride = stride;
    cache.pageIndex = pageView.pageIndex;
    cache.strokeCount = page.strokes.size();
    cache.rasterWidth = page.raster.width;
    cache.rasterHeight = page.raster.height;
    cache.rasterScale = page.raster.scale;
    cache.pageScale = pageView.scale;
    cache.valid = true;

    SelectObject(cacheDc, previousBitmap);
    DeleteObject(cacheBitmap);
    DeleteDC(cacheDc);
}

void App::PaintPage(HDC hdc, const PageView& pageView) {
    RECT shadow = pageView.rect;
    OffsetRect(&shadow, 8, 10);
    HBRUSH shadowBrush = CreateSolidBrush(RGB(7, 10, 15));
    FillRect(hdc, &shadow, shadowBrush);
    DeleteObject(shadowBrush);

    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &pageView.rect, pageBrush);
    DeleteObject(pageBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(94, 105, 121));
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, pageView.rect.left, pageView.rect.top, pageView.rect.right, pageView.rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    SaveDC(hdc);
    IntersectClipRect(hdc, pageView.rect.left, pageView.rect.top, pageView.rect.right, pageView.rect.bottom);

    EnsurePageSurfaceCache(pageView);
    if (pageView.pageIndex < pageSurfaceCaches_.size()) {
        const PageSurfaceCache& cache = pageSurfaceCaches_[pageView.pageIndex];
        if (cache.valid && !cache.pixels.empty()) {
        BITMAPINFO bitmapInfo {};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = cache.width;
        bitmapInfo.bmiHeader.biHeight = -cache.height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        SetDIBitsToDevice(
            hdc,
            pageView.rect.left,
            pageView.rect.top,
            static_cast<DWORD>(cache.width),
            static_cast<DWORD>(cache.height),
            0,
            0,
            0,
            static_cast<UINT>(cache.height),
            cache.pixels.data(),
            &bitmapInfo,
            DIB_RGB_COLORS);
        }
    }

    if (activeStroke_.has_value() && activeStrokePageIndex_.has_value() && *activeStrokePageIndex_ == pageView.pageIndex) {
        PaintStroke(hdc, *activeStroke_, pageView, true);
    }

    RestoreDC(hdc, -1);
}

void App::PaintStroke(HDC hdc, const Stroke& stroke, const PageView& pageView, bool smoothLiveStroke) const {
    if (stroke.points.empty()) {
        return;
    }

    const std::vector<Vec2> smoothedPoints = smoothLiveStroke
        ? BuildSmoothedPoints(stroke.points, strokeSmoothing_)
        : stroke.points;
    const float penWidth = std::max(1.25f, stroke.style.width * pageView.scale / 2.15f);

    if (gdiplusToken_ != 0) {
        std::vector<Gdiplus::PointF> screenPoints;
        screenPoints.reserve(smoothedPoints.size());
        for (const Vec2& point : smoothedPoints) {
            screenPoints.emplace_back(
                static_cast<Gdiplus::REAL>(pageView.rect.left + (point.x * pageView.scale)),
                static_cast<Gdiplus::REAL>(pageView.rect.top + (point.y * pageView.scale)));
        }

        const auto alpha = static_cast<BYTE>((stroke.style.color >> 24U) & 0xFFU);
        const auto red = static_cast<BYTE>((stroke.style.color >> 16U) & 0xFFU);
        const auto green = static_cast<BYTE>((stroke.style.color >> 8U) & 0xFFU);
        const auto blue = static_cast<BYTE>(stroke.style.color & 0xFFU);

        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

        Gdiplus::Color color(alpha, red, green, blue);
        Gdiplus::Pen pen(color, penWidth);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);

        if (screenPoints.size() == 1U) {
            Gdiplus::SolidBrush brush(color);
            const Gdiplus::REAL radius = penWidth * 0.5f;
            graphics.FillEllipse(
                &brush,
                screenPoints.front().X - radius,
                screenPoints.front().Y - radius,
                penWidth,
                penWidth);
        } else if (screenPoints.size() == 2U) {
            graphics.DrawLines(&pen, screenPoints.data(), static_cast<INT>(screenPoints.size()));
        } else {
            Gdiplus::GraphicsPath path;
            path.AddCurve(screenPoints.data(), static_cast<INT>(screenPoints.size()), 0.1f + (strokeSmoothing_ * 0.8f));
            graphics.DrawPath(&pen, &path);
        }

        return;
    }

    const int fallbackWidth = std::max(1, static_cast<int>(std::lround(penWidth)));
    HPEN pen = CreatePen(PS_SOLID, fallbackWidth, ToColorRef(stroke.style.color));
    HGDIOBJ oldPen = SelectObject(hdc, pen);

    POINT first {
        pageView.rect.left + static_cast<LONG>(std::lround(smoothedPoints.front().x * pageView.scale)),
        pageView.rect.top + static_cast<LONG>(std::lround(smoothedPoints.front().y * pageView.scale)),
    };
    MoveToEx(hdc, first.x, first.y, nullptr);

    if (smoothedPoints.size() == 1U) {
        LineTo(hdc, first.x + 1, first.y + 1);
    } else {
        for (std::size_t index = 1; index < smoothedPoints.size(); ++index) {
            const POINT next {
                pageView.rect.left + static_cast<LONG>(std::lround(smoothedPoints[index].x * pageView.scale)),
                pageView.rect.top + static_cast<LONG>(std::lround(smoothedPoints[index].y * pageView.scale)),
            };
            LineTo(hdc, next.x, next.y);
        }
    }

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (HandleToolbarClick(cursor)) {
            return 0;
        }

        if (HandleSettingsClick(cursor)) {
            return 0;
        }

        if (activeTool_ == Tool::Pan) {
            BeginPan(cursor, false);
        } else if (activeTool_ == Tool::Erase) {
            BeginErase(cursor);
        } else {
            BeginStroke(cursor);
        }
        return 0;
    }
    case WM_MBUTTONDOWN: {
        SetFocus(window_);
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        BeginPan(cursor, true);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (activeSlider_ != SliderTarget::None && (wParam & MK_LBUTTON) != 0) {
            UpdateActiveSlider(cursor);
        } else if (isPanning_ || (wParam & MK_MBUTTON) != 0) {
            UpdatePan(cursor);
        } else if (isErasing_ || ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Erase)) {
            ExtendErase(cursor);
        } else if ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Draw) {
            ExtendStroke(cursor);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (isDrawing_) {
            EndStroke();
        } else if (isErasing_) {
            isErasing_ = false;
            UpdateCaptureState();
        } else if (isPanning_ && !panFromMiddleButton_) {
            EndPan();
        }
        activeSlider_ = SliderTarget::None;
        return 0;
    case WM_MBUTTONUP:
        if (isPanning_ && panFromMiddleButton_) {
            EndPan();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window_, &cursor);
        ZoomAt(GET_WHEEL_DELTA_WPARAM(wParam), cursor);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT cursor {};
            GetCursorPos(&cursor);
            ScreenToClient(window_, &cursor);
            if (cursor.y < kToolbarHeight) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            } else if (activeSlider_ != SliderTarget::None || IsCursorInsideSettingsPanel(cursor)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            } else if (isPanning_ || activeTool_ == Tool::Pan) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            } else if (activeTool_ == Tool::Erase) {
                SetCursor(LoadCursorW(nullptr, IDC_NO));
            } else {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            }
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (wParam == 'Z') {
                if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                    Redo();
                } else {
                    Undo();
                }
                return 0;
            }
            if (wParam == 'Y') {
                Redo();
                return 0;
            }
            if (wParam == 'N') {
                CreateBlankDocument();
                return 0;
            }
            if (wParam == 'O') {
                OpenPdf();
                return 0;
            }
            if (wParam == 'B') {
                ToggleBrushSettings();
                return 0;
            }
        }

        switch (wParam) {
        case VK_PRIOR:
            PreviousPage();
            return 0;
        case VK_NEXT:
            NextPage();
            return 0;
        case VK_LEFT:
            PanBy(20.0f, 0.0f);
            return 0;
        case VK_RIGHT:
            PanBy(-20.0f, 0.0f);
            return 0;
        case VK_UP:
            PanBy(0.0f, 20.0f);
            return 0;
        case VK_DOWN:
            PanBy(0.0f, -20.0f);
            return 0;
        case VK_OEM_4:
        case VK_SUBTRACT:
        case VK_OEM_MINUS:
            ChangeBrushSize(-1.0f);
            return 0;
        case VK_OEM_6:
        case VK_ADD:
        case VK_OEM_PLUS:
            ChangeBrushSize(1.0f);
            return 0;
        case '0':
            ResetView();
            EnsureVisiblePageRasterQuality();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
            SetColorIndex(static_cast<std::size_t>(wParam - '1'));
            return 0;
        case 'C':
            CycleColor();
            return 0;
        default:
            break;
        }
        break;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateAllPageSurfaceCaches();
        EnsureVisiblePageRasterQuality();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->window_ = window;
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (app != nullptr) {
        return app->HandleMessage(message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace ctrainer
