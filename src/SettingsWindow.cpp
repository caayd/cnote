#include "SettingsWindow.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include "resource.h"
#include "ShortcutConfig.h"

namespace cnote {

namespace {

constexpr wchar_t kSettingsWindowClassName[] = L"CnoteSettingsWindow";
constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 780;
constexpr COLORREF kWindowBackground = RGB(18, 22, 29);
constexpr COLORREF kPanelFill = RGB(18, 24, 32);
constexpr COLORREF kPanelBorder = RGB(46, 58, 74);
constexpr COLORREF kButtonFill = RGB(28, 34, 43);
constexpr COLORREF kButtonFillActive = RGB(48, 68, 94);
constexpr COLORREF kTextPrimary = RGB(235, 240, 246);
constexpr COLORREF kTextMuted = RGB(141, 152, 167);
constexpr COLORREF kSelectionBorder = RGB(244, 247, 252);
constexpr COLORREF kScrollbarTrackFill = RGB(20, 26, 34);
constexpr COLORREF kScrollbarThumbFill = RGB(70, 88, 111);
constexpr COLORREF kScrollbarThumbActiveFill = RGB(92, 118, 150);

constexpr int kScrollbarGap = 8;
constexpr int kScrollbarWidth = 12;
constexpr int kScrollbarMinThumbHeight = 44;

constexpr int kIdPaletteList = 100;
constexpr int kIdAddColor = 101;
constexpr int kIdRemoveColor = 102;
constexpr int kIdPasteHex = 103;
constexpr int kIdShortcutList = 120;
constexpr int kIdChangeShortcut = 121;
constexpr int kIdClearShortcut = 122;
constexpr int kIdRedSlider = 200;
constexpr int kIdGreenSlider = 201;
constexpr int kIdBlueSlider = 202;
constexpr int kIdStabilizationSlider = 205;
constexpr int kIdSpacingSlider = 206;
constexpr int kIdStrokeSmoothingSlider = 207;
constexpr int kIdAutoSaveSlider = 208;

struct SettingsWindowLayout {
    RECT palettePanel {};
    RECT paletteGridRect {};
    std::vector<RECT> paletteSwatches;
    RECT addColorButton {};
    RECT removeColorButton {};
    RECT pasteHexButton {};
    RECT colorPanel {};
    RECT colorPreviewRect {};
    RECT shortcutPanel {};
    RECT shortcutListRect {};
    RECT changeShortcutButton {};
    RECT clearShortcutButton {};
    RECT shortcutHintRect {};
    RECT drawingPanel {};
    RECT redLabelRect {};
    RECT redSliderRect {};
    RECT greenLabelRect {};
    RECT greenSliderRect {};
    RECT blueLabelRect {};
    RECT blueSliderRect {};
    RECT stabilizationLabelRect {};
    RECT stabilizationSliderRect {};
    RECT spacingLabelRect {};
    RECT spacingSliderRect {};
    RECT strokeSmoothingLabelRect {};
    RECT strokeSmoothingSliderRect {};
    RECT autoSaveLabelRect {};
    RECT autoSaveSliderRect {};
    RECT scrollbarTrackRect {};
    RECT scrollbarThumbRect {};
    bool showScrollbar = false;
    int contentHeight = 0;
};

std::wstring FormatPaletteEntry(std::size_t index, std::uint32_t color) {
    wchar_t buffer[32] = {};
    std::swprintf(buffer, std::size(buffer), L"%zu. #%06X", index + 1U, color & 0xFFFFFFU);
    return buffer;
}

std::wstring FormatSliderLabel(const wchar_t* prefix, int value) {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"%ls: %d", prefix, value);
    return buffer;
}

std::wstring FormatFloatSliderLabel(const wchar_t* prefix, float value, int multiplier) {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"%ls: %d", prefix, static_cast<int>(std::lround(value * multiplier)));
    return buffer;
}

std::wstring FormatSpacingLabel(float spacing) {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"Point Spacing: %.2f", spacing);
    return buffer;
}

std::wstring FormatAutoSaveLabel(unsigned int seconds) {
    if (seconds == 0U) {
        return L"Auto Save: Off";
    }
    if (seconds < 60U) {
        return L"Auto Save: " + std::to_wstring(seconds) + L" sec";
    }
    if (seconds % 60U == 0U) {
        const unsigned int minutes = seconds / 60U;
        return L"Auto Save: " + std::to_wstring(minutes) + (minutes == 1U ? L" min" : L" mins");
    }
    return L"Auto Save: " + std::to_wstring(seconds) + L" sec";
}

void SetControlText(HWND control, const std::wstring& text) {
    if (control != nullptr) {
        SetWindowTextW(control, text.c_str());
    }
}

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect { left, top, right, bottom };
    return rect;
}

int RectHeight(const RECT& rect) {
    return static_cast<int>(rect.bottom - rect.top);
}

void OffsetRectY(RECT& rect, int deltaY) {
    rect.top += deltaY;
    rect.bottom += deltaY;
}

std::unique_ptr<Gdiplus::GraphicsPath> CreateRoundedRectPath(const RECT& rect, float radius) {
    const float left = static_cast<float>(rect.left);
    const float top = static_cast<float>(rect.top);
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    const float diameter = std::min(radius * 2.0f, std::min(width, height));

    auto path = std::make_unique<Gdiplus::GraphicsPath>();
    if (diameter <= 0.0f) {
        path->AddRectangle(Gdiplus::RectF(left, top, width, height));
        return path;
    }

    path->AddArc(left, top, diameter, diameter, 180.0f, 90.0f);
    path->AddArc(left + width - diameter, top, diameter, diameter, 270.0f, 90.0f);
    path->AddArc(left + width - diameter, top + height - diameter, diameter, diameter, 0.0f, 90.0f);
    path->AddArc(left, top + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path->CloseFigure();
    return path;
}

SettingsWindowLayout BuildSettingsWindowLayout(RECT clientRect, std::size_t paletteCount, std::size_t shortcutCount, int scrollOffset) {
    SettingsWindowLayout layout;
    const int margin = 16;
    const int gap = 14;
    const int topOffset = 40;
    const int scrollbarReserve = kScrollbarGap + kScrollbarWidth;
    const int usableWidth = static_cast<int>(clientRect.right) - (margin * 2) - scrollbarReserve;
    const int rightColumnWidth = std::clamp(usableWidth / 3, 190, 240);
    const int leftColumnWidth = std::max(260, usableWidth - gap - rightColumnWidth);
    const int left = margin;
    const int rightColumnLeft = left + leftColumnWidth + gap;

    layout.palettePanel = MakeRect(left, topOffset, left + leftColumnWidth, topOffset + 220);
    layout.colorPanel = MakeRect(left, layout.palettePanel.bottom + gap, left + leftColumnWidth, layout.palettePanel.bottom + gap + 272);
    layout.drawingPanel = MakeRect(left, layout.colorPanel.bottom + gap, left + leftColumnWidth, layout.colorPanel.bottom + gap + 10);

    const int panelPadding = 14;
    const int swatchGap = 12;
    const int columns = 4;
    const int swatchWidth = std::max(52, (leftColumnWidth - (panelPadding * 2) - (swatchGap * (columns - 1))) / columns);
    const int swatchHeight = 52;
    layout.paletteGridRect = MakeRect(
        layout.palettePanel.left + panelPadding,
        layout.palettePanel.top + 34,
        layout.palettePanel.right - panelPadding,
        layout.palettePanel.top + 34 + (swatchHeight * 3) + (swatchGap * 2));
    layout.paletteSwatches.reserve(paletteCount);
    for (std::size_t index = 0; index < paletteCount; ++index) {
        const int column = static_cast<int>(index % columns);
        const int row = static_cast<int>(index / columns);
        const int leftPos = layout.paletteGridRect.left + (column * (swatchWidth + swatchGap));
        const int topPos = layout.paletteGridRect.top + (row * (swatchHeight + swatchGap));
        layout.paletteSwatches.push_back(MakeRect(leftPos, topPos, leftPos + swatchWidth, topPos + swatchHeight));
    }

    const int buttonTop = layout.palettePanel.bottom - 42;
    layout.addColorButton = MakeRect(layout.palettePanel.left + panelPadding, buttonTop, layout.palettePanel.left + panelPadding + 108, buttonTop + 28);
    layout.removeColorButton = MakeRect(layout.addColorButton.right + 10, buttonTop, layout.addColorButton.right + 118, buttonTop + 28);
    layout.pasteHexButton = MakeRect(layout.removeColorButton.right + 10, buttonTop, layout.palettePanel.right - panelPadding, buttonTop + 28);

    layout.colorPreviewRect = MakeRect(layout.colorPanel.left + 14, layout.colorPanel.top + 34, layout.colorPanel.right - 14, layout.colorPanel.top + 100);
    const int sliderLeft = layout.colorPanel.left + 14;
    const int sliderRight = layout.colorPanel.right - 14;
    layout.redLabelRect = MakeRect(sliderLeft, layout.colorPreviewRect.bottom + 12, sliderRight, layout.colorPreviewRect.bottom + 28);
    layout.redSliderRect = MakeRect(sliderLeft, layout.redLabelRect.bottom + 2, sliderRight, layout.redLabelRect.bottom + 28);
    layout.greenLabelRect = MakeRect(sliderLeft, layout.redSliderRect.bottom + 8, sliderRight, layout.redSliderRect.bottom + 24);
    layout.greenSliderRect = MakeRect(sliderLeft, layout.greenLabelRect.bottom + 2, sliderRight, layout.greenLabelRect.bottom + 28);
    layout.blueLabelRect = MakeRect(sliderLeft, layout.greenSliderRect.bottom + 8, sliderRight, layout.greenSliderRect.bottom + 24);
    layout.blueSliderRect = MakeRect(sliderLeft, layout.blueLabelRect.bottom + 2, sliderRight, layout.blueLabelRect.bottom + 28);

    const int drawingSliderLeft = layout.drawingPanel.left + 14;
    const int drawingSliderRight = layout.drawingPanel.right - 14;
    layout.stabilizationLabelRect = MakeRect(drawingSliderLeft, layout.drawingPanel.top + 34, drawingSliderRight, layout.drawingPanel.top + 50);
    layout.stabilizationSliderRect = MakeRect(drawingSliderLeft, layout.stabilizationLabelRect.bottom + 2, drawingSliderRight, layout.stabilizationLabelRect.bottom + 28);
    layout.spacingLabelRect = MakeRect(drawingSliderLeft, layout.stabilizationSliderRect.bottom + 12, drawingSliderRight, layout.stabilizationSliderRect.bottom + 28);
    layout.spacingSliderRect = MakeRect(drawingSliderLeft, layout.spacingLabelRect.bottom + 2, drawingSliderRight, layout.spacingLabelRect.bottom + 28);
    layout.strokeSmoothingLabelRect = MakeRect(drawingSliderLeft, layout.spacingSliderRect.bottom + 12, drawingSliderRight, layout.spacingSliderRect.bottom + 28);
    layout.strokeSmoothingSliderRect = MakeRect(drawingSliderLeft, layout.strokeSmoothingLabelRect.bottom + 2, drawingSliderRight, layout.strokeSmoothingLabelRect.bottom + 28);
    layout.autoSaveLabelRect = MakeRect(drawingSliderLeft, layout.strokeSmoothingSliderRect.bottom + 12, drawingSliderRight, layout.strokeSmoothingSliderRect.bottom + 28);
    layout.autoSaveSliderRect = MakeRect(drawingSliderLeft, layout.autoSaveLabelRect.bottom + 2, drawingSliderRight, layout.autoSaveLabelRect.bottom + 28);
    layout.drawingPanel.bottom = layout.autoSaveSliderRect.bottom + 18;

    const int visibleBottom = static_cast<int>(clientRect.bottom) + scrollOffset - margin;
    const int shortcutPanelBottom = std::max({ visibleBottom, static_cast<int>(layout.drawingPanel.bottom), topOffset + 600 });
    layout.shortcutPanel = MakeRect(rightColumnLeft, topOffset, clientRect.right - margin - scrollbarReserve, shortcutPanelBottom);

    const int shortcutPanelPadding = 12;
    layout.shortcutListRect = MakeRect(
        layout.shortcutPanel.left + shortcutPanelPadding,
        layout.shortcutPanel.top + 34,
        layout.shortcutPanel.right - shortcutPanelPadding,
        layout.shortcutPanel.bottom - 104);
    layout.changeShortcutButton = MakeRect(layout.shortcutPanel.left + shortcutPanelPadding, layout.shortcutPanel.bottom - 86, layout.shortcutPanel.right - shortcutPanelPadding, layout.shortcutPanel.bottom - 58);
    layout.clearShortcutButton = MakeRect(layout.shortcutPanel.left + shortcutPanelPadding, layout.shortcutPanel.bottom - 50, layout.shortcutPanel.right - shortcutPanelPadding, layout.shortcutPanel.bottom - 22);
    layout.shortcutHintRect = MakeRect(layout.shortcutPanel.left + shortcutPanelPadding, layout.shortcutPanel.bottom - 128, layout.shortcutPanel.right - shortcutPanelPadding, layout.shortcutPanel.bottom - 92);

    static_cast<void>(shortcutCount);
    layout.contentHeight = std::max(layout.drawingPanel.bottom, layout.shortcutPanel.bottom) + margin;
    layout.scrollbarTrackRect = MakeRect(clientRect.right - margin - kScrollbarWidth, topOffset, clientRect.right - margin, clientRect.bottom - margin);

    const int viewportHeight = std::max(1, RectHeight(clientRect));
    const int maxScroll = std::max(0, layout.contentHeight - viewportHeight);
    layout.showScrollbar = maxScroll > 0 && RectHeight(layout.scrollbarTrackRect) > 0;
    if (layout.showScrollbar) {
        const int trackHeight = RectHeight(layout.scrollbarTrackRect);
        const int thumbHeight = std::clamp((viewportHeight * trackHeight) / std::max(1, layout.contentHeight), kScrollbarMinThumbHeight, trackHeight);
        const int travel = std::max(0, trackHeight - thumbHeight);
        const int thumbTop = layout.scrollbarTrackRect.top + ((travel * scrollOffset) / maxScroll);
        layout.scrollbarThumbRect = MakeRect(layout.scrollbarTrackRect.left, thumbTop, layout.scrollbarTrackRect.right, thumbTop + thumbHeight);
    }

    const int deltaY = -scrollOffset;
    OffsetRectY(layout.palettePanel, deltaY);
    OffsetRectY(layout.paletteGridRect, deltaY);
    for (RECT& swatchRect : layout.paletteSwatches) {
        OffsetRectY(swatchRect, deltaY);
    }
    OffsetRectY(layout.addColorButton, deltaY);
    OffsetRectY(layout.removeColorButton, deltaY);
    OffsetRectY(layout.pasteHexButton, deltaY);
    OffsetRectY(layout.colorPanel, deltaY);
    OffsetRectY(layout.colorPreviewRect, deltaY);
    OffsetRectY(layout.shortcutPanel, deltaY);
    OffsetRectY(layout.shortcutListRect, deltaY);
    OffsetRectY(layout.changeShortcutButton, deltaY);
    OffsetRectY(layout.clearShortcutButton, deltaY);
    OffsetRectY(layout.shortcutHintRect, deltaY);
    OffsetRectY(layout.drawingPanel, deltaY);
    OffsetRectY(layout.redLabelRect, deltaY);
    OffsetRectY(layout.redSliderRect, deltaY);
    OffsetRectY(layout.greenLabelRect, deltaY);
    OffsetRectY(layout.greenSliderRect, deltaY);
    OffsetRectY(layout.blueLabelRect, deltaY);
    OffsetRectY(layout.blueSliderRect, deltaY);
    OffsetRectY(layout.stabilizationLabelRect, deltaY);
    OffsetRectY(layout.stabilizationSliderRect, deltaY);
    OffsetRectY(layout.spacingLabelRect, deltaY);
    OffsetRectY(layout.spacingSliderRect, deltaY);
    OffsetRectY(layout.strokeSmoothingLabelRect, deltaY);
    OffsetRectY(layout.strokeSmoothingSliderRect, deltaY);
    OffsetRectY(layout.autoSaveLabelRect, deltaY);
    OffsetRectY(layout.autoSaveSliderRect, deltaY);

    return layout;
}

void FillRoundedRect(HDC hdc, RECT rect, COLORREF color) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
    const auto path = CreateRoundedRectPath(rect, 6.0f);
    graphics.FillPath(&brush, path.get());
}

void StrokeRoundedRect(HDC hdc, RECT rect, COLORREF color, int width) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), static_cast<Gdiplus::REAL>(width));
    const auto path = CreateRoundedRectPath(rect, 6.0f);
    graphics.DrawPath(&pen, path.get());
}

RECT SliderTrackRect(const RECT& sliderRect) {
    const int centerY = (sliderRect.top + sliderRect.bottom) / 2;
    return MakeRect(sliderRect.left + 10, centerY - 3, sliderRect.right - 10, centerY + 3);
}

int SliderThumbCenterX(const RECT& trackRect, int value, int minimum, int maximum) {
    const int width = std::max(1, static_cast<int>(trackRect.right - trackRect.left));
    const int range = std::max(1, maximum - minimum);
    const float normalized = static_cast<float>(std::clamp(value, minimum, maximum) - minimum) / static_cast<float>(range);
    return trackRect.left + static_cast<int>(std::lround(normalized * static_cast<float>(width - 1)));
}

void DeferMoveControl(HDWP& deferred, HWND control, const RECT& rect) {
    if (deferred == nullptr || control == nullptr) {
        return;
    }

    deferred = DeferWindowPos(
        deferred,
        control,
        nullptr,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

SettingsWindow::SettingsWindow(App& app) : app_(app) {
    INITCOMMONCONTROLSEX initControls {};
    initControls.dwSize = sizeof(initControls);
    initControls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&initControls);
}

SettingsWindow::~SettingsWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (titleFont_ != nullptr) {
        DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }
    if (backgroundBrush_ != nullptr) {
        DeleteObject(backgroundBrush_);
        backgroundBrush_ = nullptr;
    }
    if (panelBrush_ != nullptr) {
        DeleteObject(panelBrush_);
        panelBrush_ = nullptr;
    }
}

void SettingsWindow::Toggle(HWND owner) {
    if (IsOpen()) {
        DestroyWindow(window_);
        return;
    }
    Show(owner);
}

void SettingsWindow::Show(HWND owner) {
    CreateIfNeeded(owner);
    if (window_ == nullptr) {
        return;
    }

    ShowWindow(window_, SW_SHOWNORMAL);
    BringWindowToTop(window_);
    SetForegroundWindow(window_);
    Refresh();
}

bool SettingsWindow::IsOpen() const {
    return window_ != nullptr;
}

void SettingsWindow::Refresh() {
    if (window_ == nullptr) {
        return;
    }

    RefreshPaletteList();
    RefreshSliderValues();
    RefreshShortcutList();
    RefreshShortcutButtons();
    InvalidateRect(window_, nullptr, FALSE);
}

void SettingsWindow::CreateIfNeeded(HWND owner) {
    if (window_ != nullptr) {
        return;
    }

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &SettingsWindow::WindowProc;
        windowClass.hInstance = app_.instance_;
        windowClass.lpszClassName = kSettingsWindowClassName;
        windowClass.hIcon = LoadIconW(app_.instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        windowClass.hIconSm = static_cast<HICON>(LoadImageW(app_.instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }

    window_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kSettingsWindowClassName,
        L"Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kWindowWidth,
        kWindowHeight,
        owner,
        nullptr,
        app_.instance_,
        this);

    if (window_ == nullptr) {
        return;
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

    CreateControls();
    ApplyFonts();
}

void SettingsWindow::CreateControls() {
    const auto createLabel = [&](const wchar_t* text, int x, int y, int width, int height) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, width, height, window_, nullptr, app_.instance_, nullptr);
    };
    const auto createButton = [&](const wchar_t* text, int id, int x, int y, int width, int height) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            x, y, width, height, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app_.instance_, nullptr);
    };
    const auto createSlider = [&](int id, int x, int y, int width, int height) {
        return CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ | TBS_AUTOTICKS,
            x, y, width, height, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app_.instance_, nullptr);
    };

    addColorButton_ = createButton(L"Add Color", kIdAddColor, 0, 0, 120, 28);
    removeColorButton_ = createButton(L"Remove Color", kIdRemoveColor, 0, 0, 120, 28);
    pasteHexButton_ = createButton(L"Paste Hex", kIdPasteHex, 0, 0, 120, 28);

    redLabel_ = createLabel(L"Red", 0, 0, 220, 20);
    redSlider_ = createSlider(kIdRedSlider, 0, 0, 320, 30);
    greenLabel_ = createLabel(L"Green", 0, 0, 220, 20);
    greenSlider_ = createSlider(kIdGreenSlider, 0, 0, 320, 30);
    blueLabel_ = createLabel(L"Blue", 0, 0, 220, 20);
    blueSlider_ = createSlider(kIdBlueSlider, 0, 0, 320, 30);
    stabilizationLabel_ = createLabel(L"Stabilization", 0, 0, 260, 20);
    stabilizationSlider_ = createSlider(kIdStabilizationSlider, 0, 0, 320, 30);
    spacingLabel_ = createLabel(L"Point Spacing", 0, 0, 260, 20);
    spacingSlider_ = createSlider(kIdSpacingSlider, 0, 0, 320, 30);
    strokeSmoothingLabel_ = createLabel(L"Stroke Smoothing", 0, 0, 260, 20);
    strokeSmoothingSlider_ = createSlider(kIdStrokeSmoothingSlider, 0, 0, 320, 30);
    autoSaveLabel_ = createLabel(L"Auto Save", 0, 0, 260, 20);
    autoSaveSlider_ = createSlider(kIdAutoSaveSlider, 0, 0, 320, 30);

    shortcutList_ = CreateWindowExW(
        0,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        0,
        0,
        148,
        500,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdShortcutList)),
        app_.instance_,
        nullptr);
    changeShortcutButton_ = createButton(L"Change Shortcut", kIdChangeShortcut, 0, 0, 148, 28);
    clearShortcutButton_ = createButton(L"Clear Shortcut", kIdClearShortcut, 0, 0, 148, 28);
    shortcutHintLabel_ = createLabel(L"Select a command to edit its binding.", 0, 0, 148, 40);

    SendMessageW(redSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendMessageW(greenSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendMessageW(blueSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendMessageW(stabilizationSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(5, 95));
    SendMessageW(spacingSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(5, 405));
    SendMessageW(strokeSmoothingSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(autoSaveSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 300));
    SendMessageW(autoSaveSlider_, TBM_SETTICFREQ, 15, 0);
    SendMessageW(shortcutList_, LB_SETITEMHEIGHT, 0, 26);
    LayoutControls();
}

void SettingsWindow::ApplyFonts() {
    font_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    titleFont_ = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    backgroundBrush_ = CreateSolidBrush(kWindowBackground);
    panelBrush_ = CreateSolidBrush(kPanelFill);
    const std::array<HWND, 19> controls {
        addColorButton_,
        removeColorButton_,
        pasteHexButton_,
        redLabel_,
        redSlider_,
        greenLabel_,
        greenSlider_,
        blueLabel_,
        blueSlider_,
        stabilizationLabel_,
        stabilizationSlider_,
        spacingLabel_,
        spacingSlider_,
        strokeSmoothingLabel_,
        strokeSmoothingSlider_,
        autoSaveLabel_,
        autoSaveSlider_,
        shortcutList_,
        changeShortcutButton_,
    };

    for (HWND control : controls) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }
    if (clearShortcutButton_ != nullptr) {
        SendMessageW(clearShortcutButton_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    if (shortcutHintLabel_ != nullptr) {
        SendMessageW(shortcutHintLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

void SettingsWindow::LayoutControls() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, app_.palette_.size(), ShortcutDefinitions().size(), scrollOffset_);
    contentHeight_ = layout.contentHeight;
    const int previousScrollOffset = scrollOffset_;
    UpdateScrollInfo();
    if (scrollOffset_ != previousScrollOffset) {
        layout = BuildSettingsWindowLayout(clientRect, app_.palette_.size(), ShortcutDefinitions().size(), scrollOffset_);
        contentHeight_ = layout.contentHeight;
    }

    HDWP deferred = BeginDeferWindowPos(21);
    DeferMoveControl(deferred, addColorButton_, layout.addColorButton);
    DeferMoveControl(deferred, removeColorButton_, layout.removeColorButton);
    DeferMoveControl(deferred, pasteHexButton_, layout.pasteHexButton);
    DeferMoveControl(deferred, redLabel_, layout.redLabelRect);
    DeferMoveControl(deferred, redSlider_, layout.redSliderRect);
    DeferMoveControl(deferred, greenLabel_, layout.greenLabelRect);
    DeferMoveControl(deferred, greenSlider_, layout.greenSliderRect);
    DeferMoveControl(deferred, blueLabel_, layout.blueLabelRect);
    DeferMoveControl(deferred, blueSlider_, layout.blueSliderRect);
    DeferMoveControl(deferred, stabilizationLabel_, layout.stabilizationLabelRect);
    DeferMoveControl(deferred, stabilizationSlider_, layout.stabilizationSliderRect);
    DeferMoveControl(deferred, spacingLabel_, layout.spacingLabelRect);
    DeferMoveControl(deferred, spacingSlider_, layout.spacingSliderRect);
    DeferMoveControl(deferred, strokeSmoothingLabel_, layout.strokeSmoothingLabelRect);
    DeferMoveControl(deferred, strokeSmoothingSlider_, layout.strokeSmoothingSliderRect);
    DeferMoveControl(deferred, autoSaveLabel_, layout.autoSaveLabelRect);
    DeferMoveControl(deferred, autoSaveSlider_, layout.autoSaveSliderRect);
    DeferMoveControl(deferred, shortcutList_, layout.shortcutListRect);
    DeferMoveControl(deferred, changeShortcutButton_, layout.changeShortcutButton);
    DeferMoveControl(deferred, clearShortcutButton_, layout.clearShortcutButton);
    DeferMoveControl(deferred, shortcutHintLabel_, layout.shortcutHintRect);
    if (deferred != nullptr) {
        EndDeferWindowPos(deferred);
    }
}

void SettingsWindow::UpdateScrollInfo() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const int viewportHeight = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top));
    const int maxScroll = std::max(0, contentHeight_ - viewportHeight);
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);
}

void SettingsWindow::ScrollTo(int scrollOffset) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const int viewportHeight = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top));
    const int maxScroll = std::max(0, contentHeight_ - viewportHeight);
    const int clamped = std::clamp(scrollOffset, 0, maxScroll);
    if (clamped == scrollOffset_) {
        return;
    }

    scrollOffset_ = clamped;
    LayoutControls();
    InvalidateRect(window_, nullptr, FALSE);
}

void SettingsWindow::RefreshPaletteList() {
    InvalidateRect(window_, nullptr, FALSE);
}

void SettingsWindow::RefreshSliderValues() {
    const std::uint32_t color = app_.palette_[app_.settings_.paletteIndex];
    const int red = app_.GetColorChannel(color, 16);
    const int green = app_.GetColorChannel(color, 8);
    const int blue = app_.GetColorChannel(color, 0);
    SendMessageW(redSlider_, TBM_SETPOS, TRUE, red);
    SendMessageW(greenSlider_, TBM_SETPOS, TRUE, green);
    SendMessageW(blueSlider_, TBM_SETPOS, TRUE, blue);
    SendMessageW(stabilizationSlider_, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(app_.settings_.inputSmoothing * 100.0f)));
    SendMessageW(spacingSlider_, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(app_.settings_.inputSpacing * 100.0f)));
    SendMessageW(strokeSmoothingSlider_, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(app_.settings_.strokeSmoothing * 100.0f)));
    SendMessageW(autoSaveSlider_, TBM_SETPOS, TRUE, static_cast<LPARAM>(app_.settings_.autoSaveIntervalSeconds));

    SetControlText(redLabel_, FormatSliderLabel(L"Red", red));
    SetControlText(greenLabel_, FormatSliderLabel(L"Green", green));
    SetControlText(blueLabel_, FormatSliderLabel(L"Blue", blue));
    SetControlText(stabilizationLabel_, FormatFloatSliderLabel(L"Stabilization", app_.settings_.inputSmoothing, 100));
    SetControlText(spacingLabel_, FormatSpacingLabel(app_.settings_.inputSpacing));
    SetControlText(strokeSmoothingLabel_, FormatFloatSliderLabel(L"Stroke Smoothing", app_.settings_.strokeSmoothing, 100));
    SetControlText(autoSaveLabel_, FormatAutoSaveLabel(app_.settings_.autoSaveIntervalSeconds));
}

void SettingsWindow::RefreshShortcutList() {
    SendMessageW(shortcutList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(shortcutList_, LB_RESETCONTENT, 0, 0);
    int selection = static_cast<int>(SendMessageW(shortcutList_, LB_GETCURSEL, 0, 0));
    int index = 0;
    for (const ShortcutDefinition& definition : ShortcutDefinitions()) {
        const auto bindingIt = std::find_if(app_.shortcuts_.bindings.begin(), app_.shortcuts_.bindings.end(), [&](const auto& binding) {
            return binding.first == definition.action;
        });
        const std::wstring bindingLabel = bindingIt != app_.shortcuts_.bindings.end()
            ? app_.FormatKeyBinding(bindingIt->second)
            : L"Unbound";
        std::wstring entry = definition.label;
        entry += L"  -  ";
        entry += bindingLabel;
        SendMessageW(shortcutList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.c_str()));
        ++index;
    }
    if (selection < 0) {
        selection = 0;
    }
    if (index > 0) {
        selection = std::clamp(selection, 0, index - 1);
        SendMessageW(shortcutList_, LB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    }
    SendMessageW(shortcutList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(shortcutList_, nullptr, FALSE);
}

void SettingsWindow::RefreshShortcutButtons() {
    const int selection = static_cast<int>(SendMessageW(shortcutList_, LB_GETCURSEL, 0, 0));
    EnableWindow(changeShortcutButton_, selection >= 0);
    EnableWindow(clearShortcutButton_, selection >= 0);
    if (capturingShortcutAction_.has_value()) {
        SetWindowTextW(changeShortcutButton_, L"Press Key...");
        SetWindowTextW(shortcutHintLabel_, L"Press the new shortcut. Esc cancels.");
    } else {
        SetWindowTextW(changeShortcutButton_, L"Change Shortcut");
        SetWindowTextW(shortcutHintLabel_, L"Select a command to edit its binding.");
    }
}

void SettingsWindow::HandleCommand(WPARAM wParam, LPARAM lParam) {
    const int controlId = LOWORD(wParam);
    const int notification = HIWORD(wParam);
    switch (controlId) {
    case kIdAddColor:
        if (notification == BN_CLICKED) {
            app_.AddPaletteColor();
            Refresh();
        }
        break;
    case kIdRemoveColor:
        if (notification == BN_CLICKED) {
            app_.RemovePaletteColor();
            Refresh();
        }
        break;
    case kIdPasteHex:
        if (notification == BN_CLICKED) {
            static_cast<void>(app_.ApplyHexColorFromClipboard());
            Refresh();
        }
        break;
    case kIdShortcutList:
        if (notification == LBN_SELCHANGE) {
            HandleShortcutSelectionChanged();
        } else if (notification == LBN_DBLCLK) {
            BeginShortcutCapture();
        }
        break;
    case kIdChangeShortcut:
        if (notification == BN_CLICKED) {
            BeginShortcutCapture();
        }
        break;
    case kIdClearShortcut:
        if (notification == BN_CLICKED) {
            ClearSelectedShortcut();
        }
        break;
    default:
        static_cast<void>(lParam);
        break;
    }
}

void SettingsWindow::HandleHScroll(LPARAM lParam) {
    if (reinterpret_cast<HWND>(lParam) == redSlider_
        || reinterpret_cast<HWND>(lParam) == greenSlider_
        || reinterpret_cast<HWND>(lParam) == blueSlider_) {
        UpdatePaletteFromRgbSliders();
        RefreshPaletteList();
        RefreshSliderValues();
        InvalidateRect(app_.window_, nullptr, FALSE);
        return;
    }

    if (reinterpret_cast<HWND>(lParam) == stabilizationSlider_) {
        app_.settings_.inputSmoothing = static_cast<float>(SendMessageW(stabilizationSlider_, TBM_GETPOS, 0, 0)) / 100.0f;
    } else if (reinterpret_cast<HWND>(lParam) == spacingSlider_) {
        app_.settings_.inputSpacing = static_cast<float>(SendMessageW(spacingSlider_, TBM_GETPOS, 0, 0)) / 100.0f;
    } else if (reinterpret_cast<HWND>(lParam) == strokeSmoothingSlider_) {
        app_.settings_.strokeSmoothing = static_cast<float>(SendMessageW(strokeSmoothingSlider_, TBM_GETPOS, 0, 0)) / 100.0f;
    } else if (reinterpret_cast<HWND>(lParam) == autoSaveSlider_) {
        const int value = static_cast<int>(SendMessageW(autoSaveSlider_, TBM_GETPOS, 0, 0));
        app_.settings_.autoSaveIntervalSeconds = static_cast<unsigned int>(std::clamp(std::lround(static_cast<float>(value) / 15.0f) * 15, 0L, 300L));
        SendMessageW(autoSaveSlider_, TBM_SETPOS, TRUE, static_cast<LPARAM>(app_.settings_.autoSaveIntervalSeconds));
    } else {
        return;
    }

    app_.SaveAppSettings();
    app_.UpdateAutoSaveTimer();
    RefreshSliderValues();
    InvalidateRect(window_, nullptr, FALSE);
}

HBRUSH SettingsWindow::HandleControlColor(HDC hdc, HWND control) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, control == shortcutHintLabel_ ? kTextMuted : kTextPrimary);
    if (control == shortcutList_) {
        SetTextColor(hdc, kTextPrimary);
        return panelBrush_;
    }
    return backgroundBrush_;
}

RECT SettingsWindow::ColorPreviewRect() const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    return BuildSettingsWindowLayout(clientRect, app_.palette_.size(), ShortcutDefinitions().size(), scrollOffset_).colorPreviewRect;
}

void SettingsWindow::PaintWindow() {
    PAINTSTRUCT paint {};
    HDC windowDc = BeginPaint(window_, &paint);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    HDC memoryDc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, clientRect.right, clientRect.bottom);
    HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    FillRect(memoryDc, &clientRect, backgroundBrush_);
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, app_.palette_.size(), ShortcutDefinitions().size(), scrollOffset_);

    SetBkMode(memoryDc, TRANSPARENT);
    HGDIOBJ previousFont = SelectObject(memoryDc, titleFont_ != nullptr ? titleFont_ : font_);
    SetTextColor(memoryDc, kTextPrimary);
    TextOutW(memoryDc, 16, 12, L"Settings", 8);

    FillRoundedRect(memoryDc, layout.palettePanel, kPanelFill);
    FillRoundedRect(memoryDc, layout.shortcutPanel, kPanelFill);
    FillRoundedRect(memoryDc, layout.colorPanel, kPanelFill);
    FillRoundedRect(memoryDc, layout.drawingPanel, kPanelFill);
    StrokeRoundedRect(memoryDc, layout.palettePanel, kPanelBorder, 1);
    StrokeRoundedRect(memoryDc, layout.shortcutPanel, kPanelBorder, 1);
    StrokeRoundedRect(memoryDc, layout.colorPanel, kPanelBorder, 1);
    StrokeRoundedRect(memoryDc, layout.drawingPanel, kPanelBorder, 1);

    SelectObject(memoryDc, font_);
    SetTextColor(memoryDc, kTextMuted);
    TextOutW(memoryDc, layout.palettePanel.left + 14, layout.palettePanel.top + 12, L"Palette", 7);
    TextOutW(memoryDc, layout.colorPanel.left + 14, layout.colorPanel.top + 12, L"Selected Color", 14);
    TextOutW(memoryDc, layout.drawingPanel.left + 14, layout.drawingPanel.top + 12, L"Drawing", 7);
    TextOutW(memoryDc, layout.shortcutPanel.left + 12, layout.shortcutPanel.top + 12, L"Shortcuts", 9);

    for (std::size_t index = 0; index < layout.paletteSwatches.size(); ++index) {
        const RECT swatchRect = layout.paletteSwatches[index];
        FillRoundedRect(memoryDc, swatchRect, app_.ToColorRef(app_.palette_[index]));
        StrokeRoundedRect(memoryDc, swatchRect, index == app_.settings_.paletteIndex ? kSelectionBorder : kPanelBorder, index == app_.settings_.paletteIndex ? 3 : 1);
        RECT textRect = swatchRect;
        textRect.top = swatchRect.bottom - 18;
        SetTextColor(memoryDc, (app_.GetColorChannel(app_.palette_[index], 16)
                + app_.GetColorChannel(app_.palette_[index], 8)
                + app_.GetColorChannel(app_.palette_[index], 0)) > 384
            ? RGB(22, 28, 36)
            : RGB(248, 250, 252));
        wchar_t number[8] = {};
        std::swprintf(number, std::size(number), L"%zu", index + 1U);
        DrawTextW(memoryDc, number, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT previewRect = layout.colorPreviewRect;
    FillRoundedRect(memoryDc, previewRect, app_.ToColorRef(app_.palette_[app_.settings_.paletteIndex]));
    StrokeRoundedRect(memoryDc, previewRect, kSelectionBorder, 2);

    RECT previewLabel = previewRect;
    previewLabel.top += 10;
    SetTextColor(memoryDc, (app_.GetColorChannel(app_.palette_[app_.settings_.paletteIndex], 16)
            + app_.GetColorChannel(app_.palette_[app_.settings_.paletteIndex], 8)
            + app_.GetColorChannel(app_.palette_[app_.settings_.paletteIndex], 0)) > 384
        ? RGB(22, 28, 36)
        : RGB(248, 250, 252));
    const std::wstring previewText = FormatPaletteEntry(app_.settings_.paletteIndex, app_.palette_[app_.settings_.paletteIndex]);
    DrawTextW(memoryDc, previewText.c_str(), -1, &previewLabel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const auto paintSlider = [&](SliderKind kind, RECT sliderRect, int minimum, int maximum) {
        const RECT trackRect = SliderTrackRect(sliderRect);
        FillRoundedRect(memoryDc, trackRect, RGB(41, 51, 66));
        const int thumbCenterX = SliderThumbCenterX(trackRect, SliderValue(kind), minimum, maximum);
        RECT fillRect = trackRect;
        fillRect.right = static_cast<LONG>(std::max<int>(fillRect.left + 1, thumbCenterX + 1));
        FillRoundedRect(memoryDc, fillRect, RGB(92, 137, 187));

        const int centerY = (sliderRect.top + sliderRect.bottom) / 2;
        RECT thumbRect = MakeRect(thumbCenterX - 6, centerY - 6, thumbCenterX + 7, centerY + 7);
        FillRoundedRect(memoryDc, thumbRect, RGB(244, 247, 252));
        StrokeRoundedRect(memoryDc, thumbRect, draggingSlider_ == kind ? RGB(255, 255, 255) : RGB(120, 138, 162), 1);
    };

    paintSlider(SliderKind::Red, layout.redSliderRect, 0, 255);
    paintSlider(SliderKind::Green, layout.greenSliderRect, 0, 255);
    paintSlider(SliderKind::Blue, layout.blueSliderRect, 0, 255);
    paintSlider(SliderKind::Stabilization, layout.stabilizationSliderRect, 5, 95);
    paintSlider(SliderKind::Spacing, layout.spacingSliderRect, 5, 405);
    paintSlider(SliderKind::StrokeSmoothing, layout.strokeSmoothingSliderRect, 0, 100);
    paintSlider(SliderKind::AutoSave, layout.autoSaveSliderRect, 0, 300);

    if (layout.showScrollbar) {
        FillRoundedRect(memoryDc, layout.scrollbarTrackRect, kScrollbarTrackFill);
        StrokeRoundedRect(memoryDc, layout.scrollbarTrackRect, kPanelBorder, 1);
        FillRoundedRect(memoryDc, layout.scrollbarThumbRect, draggingScrollbar_ ? kScrollbarThumbActiveFill : kScrollbarThumbFill);
    }

    BitBlt(windowDc, 0, 0, clientRect.right, clientRect.bottom, memoryDc, 0, 0, SRCCOPY);

    SelectObject(memoryDc, previousFont);
    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    EndPaint(window_, &paint);
}

void SettingsWindow::DrawButton(const DRAWITEMSTRUCT& drawItem) {
    const bool selected = (drawItem.itemState & ODS_SELECTED) != 0;
    const bool focused = (drawItem.itemState & ODS_FOCUS) != 0;
    const bool disabled = (drawItem.itemState & ODS_DISABLED) != 0;
    COLORREF fill = selected ? kButtonFillActive : kButtonFill;
    COLORREF border = selected ? RGB(92, 137, 187) : kPanelBorder;
    COLORREF text = disabled ? RGB(102, 111, 124) : kTextPrimary;

    FillRoundedRect(drawItem.hDC, drawItem.rcItem, fill);
    StrokeRoundedRect(drawItem.hDC, drawItem.rcItem, focused ? kSelectionBorder : border, focused ? 2 : 1);

    wchar_t label[128] = {};
    GetWindowTextW(drawItem.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(drawItem.hDC, TRANSPARENT);
    SetTextColor(drawItem.hDC, text);
    SelectObject(drawItem.hDC, font_);
    RECT labelRect = drawItem.rcItem;
    DrawTextW(drawItem.hDC, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void SettingsWindow::DrawPaletteItem(const DRAWITEMSTRUCT& drawItem) {
    static_cast<void>(drawItem);
}

void SettingsWindow::DrawShortcutItem(const DRAWITEMSTRUCT& drawItem) {
    if (drawItem.itemID == static_cast<UINT>(-1)) {
        return;
    }

    wchar_t buffer[256] = {};
    SendMessageW(shortcutList_, LB_GETTEXT, drawItem.itemID, reinterpret_cast<LPARAM>(buffer));
    HBRUSH itemBrush = CreateSolidBrush((drawItem.itemState & ODS_SELECTED) != 0 ? RGB(32, 40, 52) : kPanelFill);
    FillRect(drawItem.hDC, &drawItem.rcItem, itemBrush);
    DeleteObject(itemBrush);

    RECT textRect = drawItem.rcItem;
    textRect.left += 10;
    SetBkMode(drawItem.hDC, TRANSPARENT);
    SetTextColor(drawItem.hDC, kTextPrimary);
    SelectObject(drawItem.hDC, font_);
    DrawTextW(drawItem.hDC, buffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void SettingsWindow::HandleShortcutSelectionChanged() {
    RefreshShortcutButtons();
}

void SettingsWindow::BeginShortcutCapture() {
    const int selection = static_cast<int>(SendMessageW(shortcutList_, LB_GETCURSEL, 0, 0));
    const auto definitions = ShortcutDefinitions();
    if (selection < 0 || selection >= static_cast<int>(definitions.size())) {
        return;
    }

    capturingShortcutAction_ = definitions[static_cast<std::size_t>(selection)].action;
    RefreshShortcutButtons();
    SetFocus(window_);
}

void SettingsWindow::ClearSelectedShortcut() {
    const int selection = static_cast<int>(SendMessageW(shortcutList_, LB_GETCURSEL, 0, 0));
    const auto definitions = ShortcutDefinitions();
    if (selection < 0 || selection >= static_cast<int>(definitions.size())) {
        return;
    }

    const App::ShortcutAction action = definitions[static_cast<std::size_t>(selection)].action;
    const auto bindingIt = std::find_if(app_.shortcuts_.bindings.begin(), app_.shortcuts_.bindings.end(), [&](auto& binding) {
        return binding.first == action;
    });
    if (bindingIt != app_.shortcuts_.bindings.end()) {
        bindingIt->second = {};
        app_.SaveAppSettings();
        RefreshShortcutList();
        RefreshShortcutButtons();
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void SettingsWindow::HandleShortcutCapture(UINT key) {
    if (!capturingShortcutAction_.has_value()) {
        return;
    }

    switch (key) {
    case VK_CONTROL:
    case VK_SHIFT:
    case VK_MENU:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LMENU:
    case VK_RMENU:
        return;
    case VK_ESCAPE:
        capturingShortcutAction_.reset();
        RefreshShortcutButtons();
        return;
    default:
        break;
    }

    const App::KeyBinding newBinding {
        key,
        (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0,
    };

    for (auto& shortcut : app_.shortcuts_.bindings) {
        if (shortcut.first != *capturingShortcutAction_
            && shortcut.second.key == newBinding.key
            && shortcut.second.ctrl == newBinding.ctrl
            && shortcut.second.shift == newBinding.shift
            && shortcut.second.alt == newBinding.alt) {
            shortcut.second = {};
        }
    }

    const auto bindingIt = std::find_if(app_.shortcuts_.bindings.begin(), app_.shortcuts_.bindings.end(), [&](auto& binding) {
        return binding.first == *capturingShortcutAction_;
    });
    if (bindingIt != app_.shortcuts_.bindings.end()) {
        bindingIt->second = newBinding;
    }

    capturingShortcutAction_.reset();
    app_.SaveAppSettings();
    RefreshShortcutList();
    RefreshShortcutButtons();
    InvalidateRect(window_, nullptr, FALSE);
}

void SettingsWindow::UpdatePaletteFromRgbSliders() {
    const auto red = static_cast<unsigned char>(SendMessageW(redSlider_, TBM_GETPOS, 0, 0));
    const auto green = static_cast<unsigned char>(SendMessageW(greenSlider_, TBM_GETPOS, 0, 0));
    const auto blue = static_cast<unsigned char>(SendMessageW(blueSlider_, TBM_GETPOS, 0, 0));
    app_.palette_[app_.settings_.paletteIndex] = app_.ComposeColor(red, green, blue);
    app_.SaveAppSettings();
}

HWND SettingsWindow::SliderHandle(SliderKind kind) const {
    switch (kind) {
    case SliderKind::Red:
        return redSlider_;
    case SliderKind::Green:
        return greenSlider_;
    case SliderKind::Blue:
        return blueSlider_;
    case SliderKind::Stabilization:
        return stabilizationSlider_;
    case SliderKind::Spacing:
        return spacingSlider_;
    case SliderKind::StrokeSmoothing:
        return strokeSmoothingSlider_;
    case SliderKind::AutoSave:
        return autoSaveSlider_;
    default:
        return nullptr;
    }
}

int SettingsWindow::SliderValue(SliderKind kind) const {
    if (HWND slider = SliderHandle(kind); slider != nullptr) {
        return static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    }
    return 0;
}

void SettingsWindow::UpdateSliderValue(SliderKind kind, int value) {
    const auto clampValue = [&](int minimum, int maximum) {
        return std::clamp(value, minimum, maximum);
    };

    if (HWND slider = SliderHandle(kind); slider != nullptr) {
        switch (kind) {
        case SliderKind::Red:
        case SliderKind::Green:
        case SliderKind::Blue:
            SendMessageW(slider, TBM_SETPOS, TRUE, clampValue(0, 255));
            UpdatePaletteFromRgbSliders();
            RefreshPaletteList();
            RefreshSliderValues();
            InvalidateRect(app_.window_, nullptr, FALSE);
            InvalidateRect(window_, nullptr, FALSE);
            return;
        case SliderKind::Stabilization:
            SendMessageW(slider, TBM_SETPOS, TRUE, clampValue(5, 95));
            app_.settings_.inputSmoothing = static_cast<float>(SliderValue(kind)) / 100.0f;
            break;
        case SliderKind::Spacing:
            SendMessageW(slider, TBM_SETPOS, TRUE, clampValue(5, 405));
            app_.settings_.inputSpacing = static_cast<float>(SliderValue(kind)) / 100.0f;
            break;
        case SliderKind::StrokeSmoothing:
            SendMessageW(slider, TBM_SETPOS, TRUE, clampValue(0, 100));
            app_.settings_.strokeSmoothing = static_cast<float>(SliderValue(kind)) / 100.0f;
            break;
        case SliderKind::AutoSave:
            SendMessageW(slider, TBM_SETPOS, TRUE, clampValue(0, 300));
            app_.settings_.autoSaveIntervalSeconds = static_cast<unsigned int>(std::clamp(std::lround(static_cast<float>(SliderValue(kind)) / 15.0f) * 15, 0L, 300L));
            SendMessageW(slider, TBM_SETPOS, TRUE, static_cast<LPARAM>(app_.settings_.autoSaveIntervalSeconds));
            break;
        default:
            break;
        }
    }

    app_.SaveAppSettings();
    app_.UpdateAutoSaveTimer();
    RefreshSliderValues();
    InvalidateRect(window_, nullptr, FALSE);
}

void SettingsWindow::UpdateSliderFromPoint(SliderKind kind, POINT cursor) {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, app_.palette_.size(), ShortcutDefinitions().size(), scrollOffset_);
    RECT sliderRect {};
    int minimum = 0;
    int maximum = 100;
    switch (kind) {
    case SliderKind::Red:
        sliderRect = layout.redSliderRect;
        minimum = 0;
        maximum = 255;
        break;
    case SliderKind::Green:
        sliderRect = layout.greenSliderRect;
        minimum = 0;
        maximum = 255;
        break;
    case SliderKind::Blue:
        sliderRect = layout.blueSliderRect;
        minimum = 0;
        maximum = 255;
        break;
    case SliderKind::Stabilization:
        sliderRect = layout.stabilizationSliderRect;
        minimum = 5;
        maximum = 95;
        break;
    case SliderKind::Spacing:
        sliderRect = layout.spacingSliderRect;
        minimum = 5;
        maximum = 405;
        break;
    case SliderKind::StrokeSmoothing:
        sliderRect = layout.strokeSmoothingSliderRect;
        minimum = 0;
        maximum = 100;
        break;
    case SliderKind::AutoSave:
        sliderRect = layout.autoSaveSliderRect;
        minimum = 0;
        maximum = 300;
        break;
    default:
        return;
    }

    const RECT trackRect = SliderTrackRect(sliderRect);
    const int clampedX = std::clamp(static_cast<int>(cursor.x), static_cast<int>(trackRect.left), static_cast<int>(trackRect.right) - 1);
    const int width = std::max(1, static_cast<int>(trackRect.right - trackRect.left - 1));
    const float normalized = static_cast<float>(clampedX - trackRect.left) / static_cast<float>(width);
    const int value = minimum + static_cast<int>(std::lround(normalized * static_cast<float>(maximum - minimum)));
    UpdateSliderValue(kind, value);
}

std::optional<SettingsWindow::SliderKind> SettingsWindow::HitTestSlider(POINT cursor) const {
    if (window_ == nullptr) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, app_.palette_.size(), ShortcutDefinitions().size(), scrollOffset_);
    const std::array<std::pair<SliderKind, RECT>, 7> sliders {{
        { SliderKind::Red, layout.redSliderRect },
        { SliderKind::Green, layout.greenSliderRect },
        { SliderKind::Blue, layout.blueSliderRect },
        { SliderKind::Stabilization, layout.stabilizationSliderRect },
        { SliderKind::Spacing, layout.spacingSliderRect },
        { SliderKind::StrokeSmoothing, layout.strokeSmoothingSliderRect },
        { SliderKind::AutoSave, layout.autoSaveSliderRect },
    }};

    for (const auto& [kind, rect] : sliders) {
        RECT hitRect = rect;
        hitRect.top -= 4;
        hitRect.bottom += 4;
        if (PtInRect(&hitRect, cursor) != 0) {
            return kind;
        }
    }

    return std::nullopt;
}

LRESULT CALLBACK SettingsWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    SettingsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT SettingsWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_GETMINMAXINFO:
    {
        auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
        minMax->ptMinTrackSize.x = 640;
        minMax->ptMinTrackSize.y = 780;
        return 0;
    }
    case WM_SIZE:
        LayoutControls();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        ScrollTo(scrollOffset_ - ((GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA) * 48));
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, app_.palette_.size(), ShortcutDefinitions().size(), scrollOffset_);
        if (layout.showScrollbar && PtInRect(&layout.scrollbarThumbRect, cursor) != 0) {
            draggingScrollbar_ = true;
            scrollbarDragOffset_ = cursor.y - layout.scrollbarThumbRect.top;
            SetCapture(window_);
            InvalidateRect(window_, &layout.scrollbarTrackRect, FALSE);
            return 0;
        }
        if (layout.showScrollbar && PtInRect(&layout.scrollbarTrackRect, cursor) != 0) {
            const int pageStep = std::max(48, RectHeight(clientRect) - 96);
            ScrollTo(scrollOffset_ + (cursor.y < layout.scrollbarThumbRect.top ? -pageStep : pageStep));
            return 0;
        }
        for (std::size_t index = 0; index < layout.paletteSwatches.size(); ++index) {
            if (PtInRect(&layout.paletteSwatches[index], cursor) != 0) {
                app_.settings_.paletteIndex = index;
                app_.SaveAppSettings();
                RefreshSliderValues();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
        }
        if (const auto slider = HitTestSlider(cursor); slider.has_value()) {
            draggingSlider_ = slider;
            SetCapture(window_);
            UpdateSliderFromPoint(*slider, cursor);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (draggingScrollbar_) {
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, app_.palette_.size(), ShortcutDefinitions().size(), scrollOffset_);
            if (!layout.showScrollbar) {
                return 0;
            }

            const int thumbHeight = RectHeight(layout.scrollbarThumbRect);
            const int maxScroll = std::max(0, contentHeight_ - RectHeight(clientRect));
            const int travel = std::max(1, RectHeight(layout.scrollbarTrackRect) - thumbHeight);
            const int thumbTop = std::clamp(
                GET_Y_LPARAM(lParam) - scrollbarDragOffset_,
                static_cast<int>(layout.scrollbarTrackRect.top),
                static_cast<int>(layout.scrollbarTrackRect.bottom) - thumbHeight);
            const int newOffset = maxScroll > 0 ? ((thumbTop - static_cast<int>(layout.scrollbarTrackRect.top)) * maxScroll) / travel : 0;
            ScrollTo(newOffset);
            return 0;
        }
        if (draggingSlider_.has_value()) {
            UpdateSliderFromPoint(*draggingSlider_, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (draggingScrollbar_) {
            draggingScrollbar_ = false;
            ReleaseCapture();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (draggingSlider_.has_value()) {
            draggingSlider_.reset();
            ReleaseCapture();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (draggingScrollbar_) {
            draggingScrollbar_ = false;
            InvalidateRect(window_, nullptr, FALSE);
        }
        if (draggingSlider_.has_value()) {
            draggingSlider_.reset();
            InvalidateRect(window_, nullptr, FALSE);
        }
        break;
    case WM_PAINT:
        PaintWindow();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(HandleControlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
    case WM_DRAWITEM:
    {
        const auto* drawItem = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (drawItem == nullptr) {
            return FALSE;
        }
        if (drawItem->CtlType == ODT_BUTTON) {
            DrawButton(*drawItem);
            return TRUE;
        }
        if (drawItem->CtlType == ODT_LISTBOX) {
            if (drawItem->CtlID == kIdPaletteList) {
                DrawPaletteItem(*drawItem);
                return TRUE;
            }
            if (drawItem->CtlID == kIdShortcutList) {
                DrawShortcutItem(*drawItem);
                return TRUE;
            }
        }
        break;
    }
    case WM_COMMAND:
        HandleCommand(wParam, lParam);
        return 0;
    case WM_HSCROLL:
        HandleHScroll(lParam);
        return 0;
    case WM_KEYDOWN:
        if (capturingShortcutAction_.has_value()) {
            HandleShortcutCapture(static_cast<UINT>(wParam));
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        capturingShortcutAction_.reset();
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace cnote
