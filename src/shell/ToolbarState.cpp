#include "App.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "DpiUtil.h"
#include "ui/Layout.h"

namespace cnote {

namespace appui = cnote::appui;

namespace {

constexpr float kMinBrushSize = 0.5f;
constexpr float kMaxBrushSize = 20.0f;
constexpr float kBrushSizeStep = 0.25f;

float SnapBrushSize(float size) {
    const float clamped = std::clamp(size, kMinBrushSize, kMaxBrushSize);
    const float steps = std::round((clamped - kMinBrushSize) / kBrushSizeStep);
    return kMinBrushSize + (steps * kBrushSizeStep);
}

std::wstring FormatBrushSizeValue(float size) {
    wchar_t buffer[16] = {};
    std::swprintf(buffer, std::size(buffer), L"%.2f", SnapBrushSize(size));
    std::wstring value = buffer;
    while (!value.empty() && value.back() == L'0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == L'.') {
        value.pop_back();
    }
    return value;
}

std::wstring FormatZoomPercentValue(float zoom) {
    wchar_t buffer[16] = {};
    std::swprintf(buffer, std::size(buffer), L"%.0f", std::round(std::max(0.0f, zoom) * 100.0f));
    return buffer;
}

bool IsToolbarBrushTool(App::Tool tool) {
    return tool == App::Tool::Draw || tool == App::Tool::Erase;
}

}  // namespace

void App::ClosePaletteOverlays() {
    wheel_.paletteMenuOpen = false;
    wheel_.paletteWheelOpen = false;
}

void App::CancelQuickSizePopup(bool cancelInput) {
    RECT popupRect {};
    bool invalidatePopup = false;
    if (window_ != nullptr && quickSizePopupTool_.has_value()) {
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        const RECT canvasRect = CanvasRect(clientRect);
        const UINT dpi = WindowDpi(window_);
        popupRect = appui::QuickSizePopupBounds(appui::BuildQuickSizePopupLayout(canvasRect, quickSizePopupAnchor_, dpi), dpi);
        invalidatePopup = true;
    }

    quickSizePopupTool_.reset();
    if (cancelInput) {
        CancelToolbarBrushSizeInput();
    }
    EndToolbarBrushSliderDrag();
    if (invalidatePopup) {
        InvalidateRect(window_, &popupRect, FALSE);
    }
}

void App::ToggleToolInWheel(Tool tool) {
    const auto existing = std::find(wheel_.tools.begin(), wheel_.tools.end(), tool);
    if (existing != wheel_.tools.end()) {
        wheel_.tools.erase(existing);
    } else {
        wheel_.tools.push_back(tool);
    }
    SaveAppSettings();
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::IsToolInWheel(Tool tool) const {
    return std::find(wheel_.tools.begin(), wheel_.tools.end(), tool) != wheel_.tools.end();
}

void App::TogglePaletteWheel() {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);

    POINT cursor {};
    GetCursorPos(&cursor);
    ScreenToClient(window_, &cursor);

    if (cursor.x < 0 || cursor.y < 0 || cursor.x >= clientRect.right || cursor.y >= clientRect.bottom) {
        const bool maximized = IsZoomed(window_);
        const appui::ToolbarLayout toolbarLayout = appui::BuildToolbarLayout(clientRect, palette_.size(), maximized, activeTool_, WindowDpi(window_));
        cursor.x = (toolbarLayout.currentColorRect.left + toolbarLayout.currentColorRect.right) / 2;
        cursor.y = toolbarLayout.currentColorRect.bottom + 72;
    }

    wheel_.center = cursor;
    wheel_.paletteWheelOpen = !wheel_.paletteWheelOpen;
    wheel_.paletteMenuOpen = false;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::ToggleQuickSizePopup() {
    if (textEditWindow_ != nullptr && editingTextPageIndex_.has_value() && editingTextIndex_.has_value()) {
        ToggleTextFormatPopup();
        return;
    }

    if (!IsToolbarBrushTool(activeTool_) || window_ == nullptr) {
        return;
    }

    if (quickSizePopupTool_.has_value() && *quickSizePopupTool_ == activeTool_) {
        CancelQuickSizePopup();
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    POINT cursor {};
    GetCursorPos(&cursor);
    ScreenToClient(window_, &cursor);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    RECT canvasRect = CanvasRect(clientRect);
    if (cursor.x < canvasRect.left || cursor.x >= canvasRect.right || cursor.y < canvasRect.top || cursor.y >= canvasRect.bottom) {
        cursor.x = (canvasRect.left + canvasRect.right) / 2;
        cursor.y = canvasRect.top + 120;
    }

    quickSizePopupAnchor_ = cursor;
    quickSizePopupTool_ = activeTool_;
    CancelToolbarZoomInput();
    CancelToolbarBrushSizeInput();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::CancelPageJumpInput() {
    pageJumpEditing_ = false;
    pageJumpReplaceOnType_ = false;
    pageJumpText_.clear();
}

void App::BeginPageJumpInput() {
    CancelToolbarBrushSizeInput();
    CancelToolbarZoomInput();
    pageJumpEditing_ = true;
    pageJumpText_ = std::to_wstring(document_.CurrentPageIndex() + 1U);
    pageJumpReplaceOnType_ = true;
}

void App::ApplyPageJumpInput() {
    if (pageJumpText_.empty()) {
        return;
    }

    wchar_t* end = nullptr;
    const unsigned long pageValue = std::wcstoul(pageJumpText_.c_str(), &end, 10);
    if (end == pageJumpText_.c_str() || *end != L'\0' || pageValue == 0UL) {
        return;
    }

    const std::size_t pageIndex = static_cast<std::size_t>(pageValue - 1UL);
    if (pageIndex < document_.PageCount()) {
        FocusPage(pageIndex);
    }
}

std::optional<App::Tool> App::ToolbarBrushTool() const {
    if (quickSizePopupTool_.has_value()) {
        return quickSizePopupTool_;
    }
    if (IsToolbarBrushTool(activeTool_)) {
        return activeTool_;
    }
    return std::nullopt;
}

std::wstring App::ToolbarBrushValueLabel() const {
    if (toolbarBrushSizeEditing_) {
        return toolbarBrushSizeText_.empty() ? L"" : toolbarBrushSizeText_;
    }

    if (const auto tool = ToolbarBrushTool(); tool.has_value()) {
        return FormatBrushSizeValue(BrushSizeForTool(*tool));
    }

    return L"";
}

void App::CancelToolbarBrushSizeInput() {
    toolbarBrushSizeEditing_ = false;
    toolbarBrushSizeReplaceOnType_ = false;
    toolbarBrushSizeEditTool_.reset();
    toolbarBrushSizeText_.clear();
}

void App::BeginToolbarBrushSizeInput() {
    const auto tool = ToolbarBrushTool();
    if (!tool.has_value()) {
        return;
    }

    pageJumpEditing_ = false;
    pageJumpReplaceOnType_ = false;
    pageJumpText_.clear();
    CancelToolbarZoomInput();
    toolbarBrushSizeEditing_ = true;
    toolbarBrushSizeReplaceOnType_ = true;
    toolbarBrushSizeEditTool_ = tool;
    toolbarBrushSizeText_ = FormatBrushSizeValue(BrushSizeForTool(*tool));
}

void App::ApplyToolbarBrushSizeInput() {
    if (!toolbarBrushSizeEditTool_.has_value() || toolbarBrushSizeText_.empty()) {
        return;
    }

    wchar_t* end = nullptr;
    const float parsed = std::wcstof(toolbarBrushSizeText_.c_str(), &end);
    if (end == toolbarBrushSizeText_.c_str() || *end != L'\0' || parsed <= 0.0f) {
        return;
    }

    SetBrushSizeForTool(*toolbarBrushSizeEditTool_, parsed);
    toolbarBrushSizeText_ = FormatBrushSizeValue(BrushSizeForTool(*toolbarBrushSizeEditTool_));
}

void App::CancelToolbarZoomInput() {
    toolbarZoomEditing_ = false;
    toolbarZoomReplaceOnType_ = false;
    toolbarZoomText_.clear();
}

void App::BeginToolbarZoomInput() {
    CancelPageJumpInput();
    CancelToolbarBrushSizeInput();
    toolbarZoomEditing_ = true;
    toolbarZoomReplaceOnType_ = true;
    toolbarZoomText_ = FormatZoomPercentValue(zoom_);
}

void App::ApplyToolbarZoomInput() {
    if (toolbarZoomText_.empty()) {
        return;
    }

    wchar_t* end = nullptr;
    const float parsedPercent = std::wcstof(toolbarZoomText_.c_str(), &end);
    if (end == toolbarZoomText_.c_str() || *end != L'\0' || parsedPercent <= 0.0f) {
        return;
    }

    SetZoom(parsedPercent / 100.0f);
    toolbarZoomText_ = FormatZoomPercentValue(zoom_);
}

void App::BeginToolbarBrushSliderDrag(POINT cursor) {
    if (!quickSizePopupTool_.has_value()) {
        return;
    }

    CancelToolbarBrushSizeInput();
    toolbarBrushSliderDragging_ = true;
    UpdateCaptureState();
    UpdateToolbarBrushSliderDrag(cursor);
}

void App::UpdateToolbarBrushSliderDrag(POINT cursor) {
    const auto tool = ToolbarBrushTool();
    if (!tool.has_value() || !quickSizePopupTool_.has_value()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT canvasRect = CanvasRect(clientRect);
    const appui::QuickSizePopupLayout popupLayout = appui::BuildQuickSizePopupLayout(canvasRect, quickSizePopupAnchor_, WindowDpi(window_));
    const RECT trackRect = appui::ToolbarBrushTrackRect(popupLayout.sliderRect);
    if (trackRect.right <= trackRect.left) {
        return;
    }

    const int clampedX = std::clamp(cursor.x, trackRect.left, trackRect.right - 1);
    const int width = std::max(1, static_cast<int>(trackRect.right - trackRect.left - 1));
    const float normalized = static_cast<float>(clampedX - trackRect.left) / static_cast<float>(width);
    const float size = 0.5f + (normalized * 19.5f);
    SetBrushSizeForTool(*tool, size);
}

void App::EndToolbarBrushSliderDrag() {
    if (!toolbarBrushSliderDragging_) {
        return;
    }

    toolbarBrushSliderDragging_ = false;
    UpdateCaptureState();
}

void App::ChangeBrushSize(float delta) {
    const Tool targetTool = activeTool_ == Tool::Erase ? Tool::Erase : Tool::Draw;
    SetBrushSizeForTool(targetTool, BrushSizeForTool(targetTool) + delta);
}

float App::BrushSizeForTool(Tool tool) const {
    switch (tool) {
    case Tool::Erase:
        return settings_.eraseBrushSize;
    case Tool::Draw:
    default:
        return settings_.drawBrushSize;
    }
}

void App::SetBrushSizeForTool(Tool tool, float size, bool refreshUi) {
    const float clamped = SnapBrushSize(std::clamp(size, kMinBrushSize, kMaxBrushSize));
    switch (tool) {
    case Tool::Erase:
        settings_.eraseBrushSize = clamped;
        break;
    case Tool::Draw:
    default:
        settings_.drawBrushSize = clamped;
        break;
    }

    SyncBrushWidthFromActiveTool();
    SaveAppSettings();
    if (refreshUi) {
        RefreshSettingsWindow();
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::SyncBrushWidthFromActiveTool() {
    if (activeTool_ == Tool::Erase) {
        brushWidth_ = settings_.eraseBrushSize;
    } else {
        brushWidth_ = settings_.drawBrushSize;
    }
}

void App::CycleColor() {
    SetColorIndex((colorIndex_ + 1U) % palette_.size());
}

void App::SetColorIndex(std::size_t index) {
    if (index >= palette_.size()) {
        return;
    }

    colorIndex_ = index;
    SaveAppSettings();
    ClosePaletteOverlays();
    bool recoloredSelection = false;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (pageIndex >= selectedStrokeIndices_.size() || selectedStrokeIndices_[pageIndex].empty()) {
            continue;
        }
        Page& page = document_.PageAt(pageIndex);
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
            if (strokeIndex < page.strokes.size()) {
                page.strokes[strokeIndex].style.color = palette_[index];
                recoloredSelection = true;
            }
        }
        if (recoloredSelection) {
            InvalidatePageSurfaceCache(pageIndex);
        }
    }

    if (recoloredSelection) {
        InvalidateCanvas();
        return;
    }

    InvalidateRect(window_, nullptr, FALSE);
}

}  // namespace cnote
