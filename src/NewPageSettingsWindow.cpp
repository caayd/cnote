#include "NewPageSettingsWindow.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <dwmapi.h>
#include <gdiplus.h>
#include <uxtheme.h>
#include <windowsx.h>

#include "resource.h"

namespace cnote {

namespace {

constexpr wchar_t kWindowClassName[] = L"CnoteNewPageSettingsWindow";
constexpr int kWindowWidth = 780;
constexpr int kWindowHeight = 600;
constexpr COLORREF kWindowBackground = RGB(18, 22, 29);
constexpr COLORREF kPanelFill = RGB(18, 24, 32);
constexpr COLORREF kPanelBorder = RGB(46, 58, 74);
constexpr COLORREF kButtonFill = RGB(28, 34, 43);
constexpr COLORREF kButtonFillActive = RGB(48, 68, 94);
constexpr COLORREF kInputFill = RGB(23, 30, 39);
constexpr COLORREF kTextPrimary = RGB(235, 240, 246);
constexpr COLORREF kTextMuted = RGB(141, 152, 167);
constexpr COLORREF kSelectionBorder = RGB(244, 247, 252);
constexpr COLORREF kAccent = RGB(92, 137, 187);
constexpr COLORREF kScrollbarTrackFill = RGB(20, 26, 34);
constexpr COLORREF kScrollbarThumbFill = RGB(70, 88, 111);
constexpr COLORREF kScrollbarThumbActiveFill = RGB(92, 118, 150);

constexpr int kScrollbarGap = 8;
constexpr int kScrollbarWidth = 12;
constexpr int kScrollbarMinThumbHeight = 44;

constexpr int kIdWidthEdit = 100;
constexpr int kIdHeightEdit = 101;
constexpr int kIdAppendPages = 102;
constexpr int kIdResetDefaults = 103;
constexpr int kIdApplyCurrent = 104;
constexpr int kIdPasteHex = 105;
constexpr int kIdApplyFuture = 106;
constexpr int kIdDimensionPresetCombo = 107;
constexpr int kIdApplyAll = 108;

constexpr std::array<std::uint32_t, 10> kDefaultPageColors {
    0xFFFFFFFFU,
    0xFFFFFBF5U,
    0xFFF4F0E6U,
    0xFFE3E7EFU,
    0xFFBFC7D4U,
    0xFF1C2430U,
    0xFF161C26U,
    0xFF12171FU,
    0xFF10161FU,
    0xFF090D12U,
};

struct DimensionPreset {
    const wchar_t* label = L"";
    float width = 612.0f;
    float height = 792.0f;
};

constexpr std::array<DimensionPreset, 6> kDimensionPresets {
    DimensionPreset { L"Letter Portrait (612 x 792)", 612.0f, 792.0f },
    DimensionPreset { L"Letter Landscape (792 x 612)", 792.0f, 612.0f },
    DimensionPreset { L"Legal Portrait (612 x 1008)", 612.0f, 1008.0f },
    DimensionPreset { L"A4 Portrait (595 x 842)", 595.0f, 842.0f },
    DimensionPreset { L"A4 Landscape (842 x 595)", 842.0f, 595.0f },
    DimensionPreset { L"Tabloid Landscape (1224 x 792)", 1224.0f, 792.0f },
};

struct NewPageSettingsLayout {
    RECT actionBarRect {};
    RECT scrollViewportRect {};
    RECT defaultsPanel {};
    RECT importPanel {};
    RECT dimensionsLabelRect {};
    RECT dimensionsComboRect {};
    RECT colorPreviewRect {};
    RECT defaultColorsLabelRect {};
    std::vector<RECT> defaultColorSwatches {};
    RECT previousColorsLabelRect {};
    std::vector<RECT> previousColorSwatches {};
    RECT previousColorsEmptyRect {};
    RECT pasteHexButtonRect {};
    RECT redLabelRect {};
    RECT redSliderRect {};
    RECT greenLabelRect {};
    RECT greenSliderRect {};
    RECT blueLabelRect {};
    RECT blueSliderRect {};
    RECT widthLabelRect {};
    RECT widthEditRect {};
    RECT heightLabelRect {};
    RECT heightEditRect {};
    RECT resetButtonRect {};
    RECT applyCurrentButtonRect {};
    RECT applyAllButtonRect {};
    RECT applyFutureButtonRect {};
    RECT appendButtonRect {};
    RECT importHintRect {};
    RECT scrollbarTrackRect {};
    RECT scrollbarThumbRect {};
    bool showScrollbar = false;
    int contentHeight = 0;
};

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect { left, top, right, bottom };
    return rect;
}

std::wstring FormatDimensionValue(float value) {
    wchar_t buffer[32] = {};
    if (std::abs(value - std::round(value)) < 0.01f) {
        std::swprintf(buffer, std::size(buffer), L"%.0f", value);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%.2f", value);
    }
    return buffer;
}

std::wstring FormatHexColor(std::uint32_t color) {
    wchar_t buffer[16] = {};
    std::swprintf(buffer, std::size(buffer), L"#%02X%02X%02X",
        static_cast<unsigned int>((color >> 16U) & 0xFFU),
        static_cast<unsigned int>((color >> 8U) & 0xFFU),
        static_cast<unsigned int>(color & 0xFFU));
    return buffer;
}

bool NearlyEqual(float left, float right, float epsilon = 0.5f) {
    return std::abs(left - right) <= epsilon;
}

std::optional<float> ParsePositiveFloat(std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }), value.end());
    if (value.empty()) {
        return std::nullopt;
    }

    try {
        std::size_t consumed = 0;
        const float parsed = std::stof(value, &consumed);
        if (consumed != value.size() || !std::isfinite(parsed) || parsed <= 0.0f) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint32_t> ParseHexColor(std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }), value.end());

    if (!value.empty() && value.front() == L'#') {
        value.erase(value.begin());
    }
    if (value.rfind(L"0x", 0) == 0 || value.rfind(L"0X", 0) == 0) {
        value.erase(0, 2);
    }

    if (value.size() != 6U && value.size() != 8U) {
        return std::nullopt;
    }
    if (!std::all_of(value.begin(), value.end(), [](wchar_t ch) { return std::iswxdigit(ch) != 0; })) {
        return std::nullopt;
    }

    try {
        const std::uint32_t parsed = static_cast<std::uint32_t>(std::stoul(value, nullptr, 16));
        return value.size() == 6U ? (0xFF000000U | parsed) : parsed;
    } catch (...) {
        return std::nullopt;
    }
}

void SetControlText(HWND control, const std::wstring& text) {
    if (control != nullptr) {
        SetWindowTextW(control, text.c_str());
    }
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

bool RectFullyInside(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left
        && inner.top >= outer.top
        && inner.right <= outer.right
        && inner.bottom <= outer.bottom;
}

NewPageSettingsLayout BuildLayout(RECT clientRect, std::size_t recentColorCount, int scrollOffset) {
    NewPageSettingsLayout layout {};
    const int margin = 16;
    const int gap = 16;
    const int top = 48;
    const int actionBarHeight = 60;
    const int actionBarTop = std::max(top + 120, static_cast<int>(clientRect.bottom) - margin - actionBarHeight);
    const int actionBarBottom = std::max(actionBarTop + actionBarHeight, static_cast<int>(clientRect.bottom) - margin);
    const int scrollViewportBottom = actionBarTop - 12;
    const int scrollbarReserve = kScrollbarGap + kScrollbarWidth;
    const int rightPanelWidth = std::clamp((static_cast<int>(clientRect.right) - (margin * 2)) / 3, 240, 280);
    const int leftPanelWidth = std::max(420, static_cast<int>(clientRect.right) - (margin * 2) - gap - rightPanelWidth - scrollbarReserve);
    const int rightLeft = margin + leftPanelWidth + gap;
    const int panelPadding = 16;
    layout.actionBarRect = MakeRect(margin, actionBarTop, static_cast<int>(clientRect.right) - margin, actionBarBottom);
    layout.scrollViewportRect = MakeRect(margin, top, static_cast<int>(clientRect.right) - margin, scrollViewportBottom);

    const int defaultsLeft = margin;
    const int defaultsRight = margin + leftPanelWidth;
    const int importRight = static_cast<int>(clientRect.right) - margin;
    const int contentLeft = defaultsLeft + panelPadding;
    const int contentRight = defaultsRight - panelPadding;

    layout.colorPreviewRect = MakeRect(contentLeft, top + 34, contentRight, top + 110);

    layout.defaultColorsLabelRect = MakeRect(contentLeft, layout.colorPreviewRect.bottom + 12, contentRight, layout.colorPreviewRect.bottom + 30);
    const int defaultColumns = std::max(1, std::min(5, static_cast<int>(kDefaultPageColors.size())));
    const int defaultGap = 10;
    const int defaultSwatchSize = std::clamp(
        (contentRight - contentLeft - (defaultGap * (defaultColumns - 1))) / defaultColumns,
        40,
        62);
    const int defaultSwatchHeight = 38;
    const int defaultSwatchTop = layout.defaultColorsLabelRect.bottom + 8;
    for (std::size_t index = 0; index < kDefaultPageColors.size(); ++index) {
        const int column = static_cast<int>(index % static_cast<std::size_t>(defaultColumns));
        const int row = static_cast<int>(index / static_cast<std::size_t>(defaultColumns));
        const int left = contentLeft + (column * (defaultSwatchSize + defaultGap));
        const int topPos = defaultSwatchTop + (row * (defaultSwatchHeight + defaultGap));
        layout.defaultColorSwatches.push_back(MakeRect(left, topPos, left + defaultSwatchSize, topPos + defaultSwatchHeight));
    }
    const int defaultRows = std::max(1, static_cast<int>(
        (kDefaultPageColors.size() + static_cast<std::size_t>(defaultColumns) - 1U) / static_cast<std::size_t>(defaultColumns)));
    const int defaultGridBottom = defaultSwatchTop
        + (defaultRows * defaultSwatchHeight)
        + (std::max(0, defaultRows - 1) * defaultGap);

    layout.previousColorsLabelRect = MakeRect(contentLeft, defaultGridBottom + 12, contentRight - 132, defaultGridBottom + 30);
    layout.pasteHexButtonRect = MakeRect(contentRight - 120, defaultGridBottom + 4, contentRight, defaultGridBottom + 32);

    const int recentColumns = 6;
    const int recentGap = 8;
    const int recentSwatchSize = std::clamp((contentRight - contentLeft - (recentGap * (recentColumns - 1))) / recentColumns, 34, 44);
    const int recentTop = layout.previousColorsLabelRect.bottom + 8;
    const std::size_t swatchCount = std::min<std::size_t>(recentColorCount, 8U);
    for (std::size_t index = 0; index < swatchCount; ++index) {
        const int column = static_cast<int>(index % static_cast<std::size_t>(recentColumns));
        const int row = static_cast<int>(index / static_cast<std::size_t>(recentColumns));
        const int left = contentLeft + (column * (recentSwatchSize + recentGap));
        const int topPos = recentTop + (row * (recentSwatchSize + recentGap));
        layout.previousColorSwatches.push_back(MakeRect(left, topPos, left + recentSwatchSize, topPos + recentSwatchSize));
    }
    const int recentRows = std::max(1, static_cast<int>((swatchCount + static_cast<std::size_t>(recentColumns) - 1U) / static_cast<std::size_t>(recentColumns)));
    layout.previousColorsEmptyRect = MakeRect(contentLeft, recentTop, contentRight, recentTop + (recentSwatchSize * recentRows));

    const int slidersTop = recentTop + (recentSwatchSize * recentRows) + (recentGap * std::max(0, recentRows - 1)) + 16;
    layout.redLabelRect = MakeRect(contentLeft, slidersTop, contentRight, slidersTop + 18);
    layout.redSliderRect = MakeRect(contentLeft, layout.redLabelRect.bottom + 2, contentRight, layout.redLabelRect.bottom + 26);
    layout.greenLabelRect = MakeRect(contentLeft, layout.redSliderRect.bottom + 6, contentRight, layout.redSliderRect.bottom + 24);
    layout.greenSliderRect = MakeRect(contentLeft, layout.greenLabelRect.bottom + 2, contentRight, layout.greenLabelRect.bottom + 26);
    layout.blueLabelRect = MakeRect(contentLeft, layout.greenSliderRect.bottom + 6, contentRight, layout.greenSliderRect.bottom + 24);
    layout.blueSliderRect = MakeRect(contentLeft, layout.blueLabelRect.bottom + 2, contentRight, layout.blueLabelRect.bottom + 26);

    const int fieldGap = 18;
    const int fieldWidth = std::clamp((contentRight - contentLeft - fieldGap) / 2, 132, 164);
    const int fieldHeight = 28;
    layout.widthLabelRect = MakeRect(contentLeft, layout.blueSliderRect.bottom + 14, contentLeft + fieldWidth, layout.blueSliderRect.bottom + 32);
    layout.widthEditRect = MakeRect(contentLeft, layout.widthLabelRect.bottom + 4, contentLeft + fieldWidth, layout.widthLabelRect.bottom + 4 + fieldHeight);
    layout.heightLabelRect = MakeRect(contentLeft + fieldWidth + fieldGap, layout.blueSliderRect.bottom + 14, contentLeft + (fieldWidth * 2) + fieldGap, layout.blueSliderRect.bottom + 32);
    layout.heightEditRect = MakeRect(contentLeft + fieldWidth + fieldGap, layout.heightLabelRect.bottom + 4, contentLeft + (fieldWidth * 2) + fieldGap, layout.heightLabelRect.bottom + 4 + fieldHeight);
    layout.dimensionsLabelRect = MakeRect(contentLeft, layout.heightEditRect.bottom + 12, contentRight, layout.heightEditRect.bottom + 30);
    layout.dimensionsComboRect = MakeRect(contentLeft, layout.dimensionsLabelRect.bottom + 4, contentRight, layout.dimensionsLabelRect.bottom + 32);

    const int importContentLeft = rightLeft + panelPadding;
    const int importContentRight = importRight - panelPadding;
    layout.appendButtonRect = MakeRect(
        importContentLeft,
        top + 44,
        importContentRight,
        top + 76);
    layout.importHintRect = MakeRect(
        importContentLeft,
        layout.appendButtonRect.bottom + 18,
        importContentRight,
        layout.appendButtonRect.bottom + 180);

    const int leftContentBottom = layout.dimensionsComboRect.bottom + panelPadding;
    const int rightContentBottom = layout.importHintRect.bottom + panelPadding;
    layout.defaultsPanel = MakeRect(defaultsLeft, top, defaultsRight, std::max(scrollViewportBottom, leftContentBottom));
    layout.importPanel = MakeRect(rightLeft, top, importRight, std::max(scrollViewportBottom, rightContentBottom));

    constexpr int buttonGap = 12;
    constexpr int resetButtonWidth = 136;
    constexpr int applyCurrentWidth = 164;
    constexpr int applyFutureWidth = 170;
    constexpr int applyAllWidth = 118;
    const int actionButtonTop = layout.actionBarRect.top + ((actionBarHeight - 28) / 2);
    int actionButtonLeft = layout.actionBarRect.left + 16;
    layout.resetButtonRect = MakeRect(actionButtonLeft, actionButtonTop, actionButtonLeft + resetButtonWidth, actionButtonTop + 28);
    actionButtonLeft = layout.resetButtonRect.right + buttonGap;
    layout.applyCurrentButtonRect = MakeRect(actionButtonLeft, actionButtonTop, actionButtonLeft + applyCurrentWidth, actionButtonTop + 28);
    actionButtonLeft = layout.applyCurrentButtonRect.right + buttonGap;
    layout.applyFutureButtonRect = MakeRect(actionButtonLeft, actionButtonTop, actionButtonLeft + applyFutureWidth, actionButtonTop + 28);
    actionButtonLeft = layout.applyFutureButtonRect.right + buttonGap;
    layout.applyAllButtonRect = MakeRect(actionButtonLeft, actionButtonTop, actionButtonLeft + applyAllWidth, actionButtonTop + 28);

    layout.contentHeight = std::max(layout.defaultsPanel.bottom, layout.importPanel.bottom) - top + margin;
    layout.scrollbarTrackRect = MakeRect(clientRect.right - margin - kScrollbarWidth, top, clientRect.right - margin, scrollViewportBottom);
    const int viewportHeight = std::max(1, scrollViewportBottom - top);
    const int maxScroll = std::max(0, layout.contentHeight - viewportHeight);
    layout.showScrollbar = maxScroll > 0 && (layout.scrollbarTrackRect.bottom - layout.scrollbarTrackRect.top) > 0;
    if (layout.showScrollbar) {
        const int trackHeight = layout.scrollbarTrackRect.bottom - layout.scrollbarTrackRect.top;
        const int thumbHeight = std::clamp((viewportHeight * trackHeight) / std::max(1, layout.contentHeight), kScrollbarMinThumbHeight, trackHeight);
        const int travel = std::max(0, trackHeight - thumbHeight);
        const int thumbTop = layout.scrollbarTrackRect.top + ((travel * scrollOffset) / std::max(1, maxScroll));
        layout.scrollbarThumbRect = MakeRect(layout.scrollbarTrackRect.left, thumbTop, layout.scrollbarTrackRect.right, thumbTop + thumbHeight);
    }

    const int deltaY = -scrollOffset;
    auto offsetY = [deltaY](RECT& rect) {
        rect.top += deltaY;
        rect.bottom += deltaY;
    };
    offsetY(layout.defaultsPanel);
    offsetY(layout.importPanel);
    offsetY(layout.colorPreviewRect);
    offsetY(layout.defaultColorsLabelRect);
    for (RECT& rect : layout.defaultColorSwatches) {
        offsetY(rect);
    }
    offsetY(layout.previousColorsLabelRect);
    for (RECT& rect : layout.previousColorSwatches) {
        offsetY(rect);
    }
    offsetY(layout.previousColorsEmptyRect);
    offsetY(layout.pasteHexButtonRect);
    offsetY(layout.redLabelRect);
    offsetY(layout.redSliderRect);
    offsetY(layout.greenLabelRect);
    offsetY(layout.greenSliderRect);
    offsetY(layout.blueLabelRect);
    offsetY(layout.blueSliderRect);
    offsetY(layout.widthLabelRect);
    offsetY(layout.widthEditRect);
    offsetY(layout.heightLabelRect);
    offsetY(layout.heightEditRect);
    offsetY(layout.dimensionsLabelRect);
    offsetY(layout.dimensionsComboRect);
    offsetY(layout.appendButtonRect);
    offsetY(layout.importHintRect);

    return layout;
}

}  // namespace

NewPageSettingsWindow::NewPageSettingsWindow(App& app) : app_(app) {
}

NewPageSettingsWindow::~NewPageSettingsWindow() {
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
    if (inputBrush_ != nullptr) {
        DeleteObject(inputBrush_);
        inputBrush_ = nullptr;
    }
    if (toolbarBrush_ != nullptr) {
        DeleteObject(toolbarBrush_);
        toolbarBrush_ = nullptr;
    }
}

void NewPageSettingsWindow::Toggle(HWND owner) {
    if (IsOpen()) {
        DestroyWindow(window_);
        return;
    }
    Show(owner);
}

void NewPageSettingsWindow::Show(HWND owner) {
    CreateIfNeeded(owner);
    if (window_ == nullptr) {
        return;
    }

    SyncDraftFromApp();
    ShowWindow(window_, SW_SHOWNORMAL);
    BringWindowToTop(window_);
    SetForegroundWindow(window_);
    Refresh();
}

bool NewPageSettingsWindow::IsOpen() const {
    return window_ != nullptr;
}

void NewPageSettingsWindow::Refresh() {
    if (window_ == nullptr) {
        return;
    }

    SyncDraftFromApp();
    UpdateEditValues();
    InvalidateRect(window_, nullptr, FALSE);
}

void NewPageSettingsWindow::CreateIfNeeded(HWND owner) {
    if (window_ != nullptr) {
        return;
    }

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &NewPageSettingsWindow::WindowProc;
        windowClass.hInstance = app_.instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hIcon = LoadIconW(app_.instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        windowClass.hIconSm = static_cast<HICON>(LoadImageW(app_.instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }

    window_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kWindowClassName,
        L"New Page Settings",
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

void NewPageSettingsWindow::CreateControls() {
    const auto createButton = [&](const wchar_t* text, int id, int width) {
        return CreateWindowExW(
            0,
            L"BUTTON",
            text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0,
            0,
            width,
            28,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            app_.instance_,
            nullptr);
    };
    const auto createEdit = [&](int id) {
        return CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0,
            0,
            148,
            28,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            app_.instance_,
            nullptr);
    };
    const auto createComboBox = [&](int id) {
        return CreateWindowExW(
            0,
            WC_COMBOBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            0,
            0,
            220,
            240,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            app_.instance_,
            nullptr);
    };

    widthEdit_ = createEdit(kIdWidthEdit);
    heightEdit_ = createEdit(kIdHeightEdit);
    dimensionPresetCombo_ = createComboBox(kIdDimensionPresetCombo);
    pasteHexButton_ = createButton(L"Paste Hex", kIdPasteHex, 120);
    appendButton_ = createButton(L"Append Pages...", kIdAppendPages, 164);
    resetButton_ = createButton(L"Reset Defaults", kIdResetDefaults, 164);
    applyCurrentButton_ = createButton(L"Apply to Current Page", kIdApplyCurrent, 180);
    applyAllButton_ = createButton(L"Apply to All", kIdApplyAll, 118);
    applyFutureButton_ = createButton(L"Apply to Future Pages", kIdApplyFuture, 180);

    SendMessageW(dimensionPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom"));
    for (const DimensionPreset& preset : kDimensionPresets) {
        SendMessageW(dimensionPresetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.label));
    }
    SendMessageW(dimensionPresetCombo_, CB_SETCURSEL, 0, 0);

    SyncDraftFromApp();
    LayoutControls();
    UpdateEditValues();
}

void NewPageSettingsWindow::ApplyFonts() {
    font_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    titleFont_ = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    backgroundBrush_ = CreateSolidBrush(kWindowBackground);
    panelBrush_ = CreateSolidBrush(kPanelFill);
    inputBrush_ = CreateSolidBrush(kInputFill);
    toolbarBrush_ = CreateSolidBrush(RGB(15, 20, 27));

    const HWND controls[] {
        widthEdit_,
        heightEdit_,
        dimensionPresetCombo_,
        pasteHexButton_,
        appendButton_,
        resetButton_,
        applyCurrentButton_,
        applyAllButton_,
        applyFutureButton_,
    };
    for (HWND control : controls) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }

    if (dimensionPresetCombo_ != nullptr) {
        SetWindowTheme(dimensionPresetCombo_, L"DarkMode_CFD", nullptr);
        COMBOBOXINFO comboInfo {};
        comboInfo.cbSize = sizeof(comboInfo);
        if (GetComboBoxInfo(dimensionPresetCombo_, &comboInfo) != FALSE) {
            if (comboInfo.hwndList != nullptr) {
                SetWindowTheme(comboInfo.hwndList, L"DarkMode_Explorer", nullptr);
            }
            if (comboInfo.hwndItem != nullptr) {
                SetWindowTheme(comboInfo.hwndItem, L"DarkMode_CFD", nullptr);
            }
        }
    }
}

void NewPageSettingsWindow::LayoutControls() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_);
    contentHeight_ = layout.contentHeight;
    const int previousScrollOffset = scrollOffset_;
    UpdateScrollInfo();
    if (scrollOffset_ != previousScrollOffset) {
        layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_);
        contentHeight_ = layout.contentHeight;
    }

    HDWP deferred = BeginDeferWindowPos(9);
    DeferMoveControl(deferred, widthEdit_, layout.widthEditRect);
    DeferMoveControl(deferred, heightEdit_, layout.heightEditRect);
    DeferMoveControl(
        deferred,
        dimensionPresetCombo_,
        MakeRect(
            layout.dimensionsComboRect.left,
            layout.dimensionsComboRect.top,
            layout.dimensionsComboRect.right,
            layout.dimensionsComboRect.top + 240));
    DeferMoveControl(deferred, pasteHexButton_, layout.pasteHexButtonRect);
    DeferMoveControl(deferred, appendButton_, layout.appendButtonRect);
    DeferMoveControl(deferred, resetButton_, layout.resetButtonRect);
    DeferMoveControl(deferred, applyCurrentButton_, layout.applyCurrentButtonRect);
    DeferMoveControl(deferred, applyAllButton_, layout.applyAllButtonRect);
    DeferMoveControl(deferred, applyFutureButton_, layout.applyFutureButtonRect);
    if (deferred != nullptr) {
        EndDeferWindowPos(deferred);
    }

    const auto syncScrollableControl = [&](HWND control, RECT rect, int dropdownHeight = 0) {
        if (control == nullptr) {
            return;
        }

        RECT visibleRect = rect;
        if (dropdownHeight > 0) {
            visibleRect.bottom = visibleRect.top + 28;
        }
        const bool fullyVisible = RectFullyInside(layout.scrollViewportRect, visibleRect);
        ShowWindow(control, fullyVisible ? SW_SHOWNA : SW_HIDE);
    };

    syncScrollableControl(widthEdit_, layout.widthEditRect);
    syncScrollableControl(heightEdit_, layout.heightEditRect);
    syncScrollableControl(dimensionPresetCombo_, layout.dimensionsComboRect, 240);
    syncScrollableControl(pasteHexButton_, layout.pasteHexButtonRect);
    syncScrollableControl(appendButton_, layout.appendButtonRect);

    ShowWindow(resetButton_, SW_SHOWNA);
    ShowWindow(applyCurrentButton_, SW_SHOWNA);
    ShowWindow(applyFutureButton_, SW_SHOWNA);
    ShowWindow(applyAllButton_, SW_SHOWNA);
    SetWindowPos(resetButton_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(applyCurrentButton_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(applyFutureButton_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(applyAllButton_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void NewPageSettingsWindow::UpdateScrollInfo() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_);
    const int viewportHeight = std::max(1, static_cast<int>(layout.scrollViewportRect.bottom - layout.scrollViewportRect.top));
    const int maxScroll = std::max(0, contentHeight_ - viewportHeight);
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);
}

void NewPageSettingsWindow::ScrollTo(int scrollOffset) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_);
    const int viewportHeight = std::max(1, static_cast<int>(layout.scrollViewportRect.bottom - layout.scrollViewportRect.top));
    const int maxScroll = std::max(0, contentHeight_ - viewportHeight);
    const int clamped = std::clamp(scrollOffset, 0, maxScroll);
    if (clamped == scrollOffset_) {
        return;
    }

    scrollOffset_ = clamped;
    LayoutControls();
    InvalidateRect(window_, nullptr, FALSE);
}

void NewPageSettingsWindow::SyncDraftFromApp() {
    draftWidth_ = app_.newPageSettings_.width;
    draftHeight_ = app_.newPageSettings_.height;
    draftBackgroundColor_ = app_.newPageSettings_.backgroundColor;
}

void NewPageSettingsWindow::UpdateEditValues() {
    if (suppressEditRefresh_) {
        return;
    }

    suppressEditRefresh_ = true;
    SetControlText(widthEdit_, FormatDimensionValue(draftWidth_));
    SetControlText(heightEdit_, FormatDimensionValue(draftHeight_));
    SyncDimensionPresetSelection();
    suppressEditRefresh_ = false;
}

void NewPageSettingsWindow::HandleCommand(WPARAM wParam, LPARAM lParam) {
    const int controlId = LOWORD(wParam);
    const int notification = HIWORD(wParam);
    switch (controlId) {
    case kIdPasteHex:
        if (notification == BN_CLICKED) {
            static_cast<void>(PasteHexColorFromClipboard());
        }
        break;
    case kIdAppendPages:
        if (notification == BN_CLICKED) {
            static_cast<void>(app_.AppendPagesFromFile());
        }
        break;
    case kIdResetDefaults:
        if (notification == BN_CLICKED) {
            ResetDraftToDefaults();
        }
        break;
    case kIdApplyCurrent:
        if (notification == BN_CLICKED) {
            ApplyDraftToCurrentPage();
        }
        break;
    case kIdApplyAll:
        if (notification == BN_CLICKED) {
            ApplyDraftToAllPages();
        }
        break;
    case kIdApplyFuture:
        if (notification == BN_CLICKED) {
            ApplyDraftToFuturePages();
        }
        break;
    case kIdDimensionPresetCombo:
        if (notification == CBN_SELCHANGE && !suppressPresetSelection_) {
            ApplySelectedDimensionPreset();
        }
        break;
    case kIdWidthEdit:
        if (notification == EN_CHANGE) {
            HandleDimensionEdit(widthEdit_, draftWidth_, false);
        } else if (notification == EN_KILLFOCUS) {
            HandleDimensionEdit(widthEdit_, draftWidth_, true);
        }
        break;
    case kIdHeightEdit:
        if (notification == EN_CHANGE) {
            HandleDimensionEdit(heightEdit_, draftHeight_, false);
        } else if (notification == EN_KILLFOCUS) {
            HandleDimensionEdit(heightEdit_, draftHeight_, true);
        }
        break;
    default:
        static_cast<void>(lParam);
        break;
    }
}

HBRUSH NewPageSettingsWindow::HandleControlColor(HDC hdc, HWND control) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kTextPrimary);
    if (control == widthEdit_ || control == heightEdit_ || control == dimensionPresetCombo_) {
        return inputBrush_;
    }
    if (control == applyCurrentButton_ || control == applyAllButton_ || control == applyFutureButton_ || control == resetButton_) {
        return toolbarBrush_;
    }
    return backgroundBrush_;
}

void NewPageSettingsWindow::PaintWindow() {
    PAINTSTRUCT paint {};
    HDC windowDc = BeginPaint(window_, &paint);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_);

    HDC memoryDc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, clientRect.right, clientRect.bottom);
    HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    FillRect(memoryDc, &clientRect, backgroundBrush_);

    SetBkMode(memoryDc, TRANSPARENT);
    HGDIOBJ previousFont = SelectObject(memoryDc, titleFont_ != nullptr ? titleFont_ : font_);
    SetTextColor(memoryDc, kTextPrimary);
    TextOutW(memoryDc, 16, 12, L"New Page Settings", 17);

    SaveDC(memoryDc);
    IntersectClipRect(
        memoryDc,
        layout.scrollViewportRect.left,
        layout.scrollViewportRect.top,
        layout.scrollViewportRect.right,
        layout.scrollViewportRect.bottom);

    FillRoundedRect(memoryDc, layout.defaultsPanel, kPanelFill);
    FillRoundedRect(memoryDc, layout.importPanel, kPanelFill);
    StrokeRoundedRect(memoryDc, layout.defaultsPanel, kPanelBorder, 1);
    StrokeRoundedRect(memoryDc, layout.importPanel, kPanelBorder, 1);

    SelectObject(memoryDc, font_);
    SetTextColor(memoryDc, kTextMuted);
    TextOutW(memoryDc, layout.defaultsPanel.left + 16, layout.defaultsPanel.top + 12, L"Defaults", 8);
    TextOutW(memoryDc, layout.importPanel.left + 16, layout.importPanel.top + 12, L"Import", 6);

    FillRoundedRect(memoryDc, layout.colorPreviewRect, app_.ToColorRef(draftBackgroundColor_));
    StrokeRoundedRect(memoryDc, layout.colorPreviewRect, kSelectionBorder, 2);
    RECT previewTextRect = layout.colorPreviewRect;
    previewTextRect.top += 10;
    previewTextRect.bottom -= 10;
    const COLORREF previewText = (app_.GetColorChannel(draftBackgroundColor_, 16)
            + app_.GetColorChannel(draftBackgroundColor_, 8)
            + app_.GetColorChannel(draftBackgroundColor_, 0)) > 384
        ? RGB(22, 28, 36)
        : RGB(248, 250, 252);
    SetTextColor(memoryDc, previewText);
    const std::wstring previewLabel = L"Background Preview\n" + FormatHexColor(draftBackgroundColor_);
    DrawTextW(memoryDc, previewLabel.c_str(), -1, &previewTextRect, DT_CENTER | DT_VCENTER | DT_WORDBREAK);

    SetTextColor(memoryDc, kTextMuted);
    RECT defaultColorsLabelRect = layout.defaultColorsLabelRect;
    DrawTextW(memoryDc, L"Default Colors", -1, &defaultColorsLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (std::size_t index = 0; index < layout.defaultColorSwatches.size(); ++index) {
        const RECT swatchRect = layout.defaultColorSwatches[index];
        const std::uint32_t color = kDefaultPageColors[index];
        FillRoundedRect(memoryDc, swatchRect, app_.ToColorRef(color));
        StrokeRoundedRect(memoryDc, swatchRect, color == draftBackgroundColor_ ? kSelectionBorder : kPanelBorder, color == draftBackgroundColor_ ? 2 : 1);
    }

    RECT previousColorsLabelRect = layout.previousColorsLabelRect;
    DrawTextW(memoryDc, L"Previous Colors", -1, &previousColorsLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (layout.previousColorSwatches.empty()) {
        RECT emptyRect = layout.previousColorsEmptyRect;
        SetTextColor(memoryDc, RGB(108, 119, 134));
        DrawTextW(memoryDc, L"No previous colors yet.", -1, &emptyRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        for (std::size_t index = 0; index < layout.previousColorSwatches.size(); ++index) {
            const RECT swatchRect = layout.previousColorSwatches[index];
            const std::uint32_t color = app_.newPageSettings_.recentColors[index];
            FillRoundedRect(memoryDc, swatchRect, app_.ToColorRef(color));
            StrokeRoundedRect(memoryDc, swatchRect, color == draftBackgroundColor_ ? kSelectionBorder : kPanelBorder, color == draftBackgroundColor_ ? 2 : 1);
        }
    }

    const auto paintSlider = [&](SliderKind kind, const RECT& labelRect, const RECT& sliderRect, const wchar_t* label) {
        wchar_t buffer[32] = {};
        std::swprintf(buffer, std::size(buffer), L"%ls: %d", label, SliderValue(kind));
        RECT textRect = labelRect;
        SetTextColor(memoryDc, kTextPrimary);
        DrawTextW(memoryDc, buffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        const RECT trackRect = SliderTrackRect(sliderRect);
        FillRoundedRect(memoryDc, trackRect, RGB(41, 51, 66));
        const int thumbCenterX = SliderThumbCenterX(trackRect, SliderValue(kind), 0, 255);
        RECT fillRect = trackRect;
        fillRect.right = static_cast<LONG>(std::max<int>(fillRect.left + 1, thumbCenterX + 1));
        FillRoundedRect(memoryDc, fillRect, kAccent);

        const int centerY = (sliderRect.top + sliderRect.bottom) / 2;
        RECT thumbRect = MakeRect(thumbCenterX - 6, centerY - 6, thumbCenterX + 7, centerY + 7);
        FillRoundedRect(memoryDc, thumbRect, RGB(244, 247, 252));
        StrokeRoundedRect(memoryDc, thumbRect, draggingSlider_ == kind ? RGB(255, 255, 255) : RGB(120, 138, 162), 1);
    };

    paintSlider(SliderKind::Red, layout.redLabelRect, layout.redSliderRect, L"Red");
    paintSlider(SliderKind::Green, layout.greenLabelRect, layout.greenSliderRect, L"Green");
    paintSlider(SliderKind::Blue, layout.blueLabelRect, layout.blueSliderRect, L"Blue");

    RECT widthLabelRect = layout.widthLabelRect;
    RECT heightLabelRect = layout.heightLabelRect;
    RECT dimensionsLabelRect = layout.dimensionsLabelRect;
    SetTextColor(memoryDc, kTextPrimary);
    DrawTextW(memoryDc, L"Width (pt)", -1, &widthLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(memoryDc, L"Height (pt)", -1, &heightLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(memoryDc, L"Common Sizes", -1, &dimensionsLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(memoryDc, kTextMuted);
    RECT importHintRect = layout.importHintRect;
    DrawTextW(
        memoryDc,
        L"Append pages from a .pdf or .cnote file.\n\nImported pages are appended after the current last page and flattened into the current document so they work with this session's note model.",
        -1,
        &importHintRect,
        DT_WORDBREAK);

    if (layout.showScrollbar) {
        FillRoundedRect(memoryDc, layout.scrollbarTrackRect, kScrollbarTrackFill);
        StrokeRoundedRect(memoryDc, layout.scrollbarTrackRect, kPanelBorder, 1);
        FillRoundedRect(memoryDc, layout.scrollbarThumbRect, draggingScrollbar_ ? kScrollbarThumbActiveFill : kScrollbarThumbFill);
    }

    RestoreDC(memoryDc, -1);

    FillRoundedRect(memoryDc, layout.actionBarRect, RGB(15, 20, 27));
    StrokeRoundedRect(memoryDc, layout.actionBarRect, kPanelBorder, 1);

    BitBlt(windowDc, 0, 0, clientRect.right, clientRect.bottom, memoryDc, 0, 0, SRCCOPY);

    SelectObject(memoryDc, previousFont);
    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    EndPaint(window_, &paint);
}

void NewPageSettingsWindow::DrawButton(const DRAWITEMSTRUCT& drawItem) {
    const bool selected = (drawItem.itemState & ODS_SELECTED) != 0;
    const bool focused = (drawItem.itemState & ODS_FOCUS) != 0;
    const bool disabled = (drawItem.itemState & ODS_DISABLED) != 0;
    const int controlId = GetDlgCtrlID(drawItem.hwndItem);
    const bool emphasized = controlId == kIdApplyCurrent || controlId == kIdApplyAll || controlId == kIdApplyFuture || controlId == kIdAppendPages;
    const COLORREF fill = selected
        ? (emphasized ? RGB(49, 82, 125) : kButtonFillActive)
        : (emphasized ? RGB(32, 63, 102) : kButtonFill);
    const COLORREF border = selected ? kAccent : kPanelBorder;
    const COLORREF text = disabled ? RGB(102, 111, 124) : kTextPrimary;

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

void NewPageSettingsWindow::ApplyDraftToCurrentPage() {
    HandleDimensionEdit(widthEdit_, draftWidth_, true);
    HandleDimensionEdit(heightEdit_, draftHeight_, true);
    app_.ApplyConfiguredSettingsToCurrentPage(draftWidth_, draftHeight_, draftBackgroundColor_);
}

void NewPageSettingsWindow::ApplyDraftToAllPages() {
    HandleDimensionEdit(widthEdit_, draftWidth_, true);
    HandleDimensionEdit(heightEdit_, draftHeight_, true);
    app_.ApplyConfiguredSettingsToAllPages(draftWidth_, draftHeight_, draftBackgroundColor_);
}

void NewPageSettingsWindow::ApplyDraftToFuturePages() {
    HandleDimensionEdit(widthEdit_, draftWidth_, true);
    HandleDimensionEdit(heightEdit_, draftHeight_, true);

    const std::uint32_t previousColor = app_.newPageSettings_.backgroundColor;
    app_.newPageSettings_.width = std::max(72.0f, draftWidth_);
    app_.newPageSettings_.height = std::max(72.0f, draftHeight_);
    app_.newPageSettings_.backgroundColor = draftBackgroundColor_;

    if (previousColor != draftBackgroundColor_) {
        auto& recentColors = app_.newPageSettings_.recentColors;
        recentColors.erase(std::remove(recentColors.begin(), recentColors.end(), previousColor), recentColors.end());
        recentColors.erase(std::remove(recentColors.begin(), recentColors.end(), draftBackgroundColor_), recentColors.end());
        recentColors.insert(recentColors.begin(), previousColor);
        if (recentColors.size() > 8U) {
            recentColors.resize(8U);
        }
    }

    LayoutControls();
    InvalidateRect(window_, nullptr, FALSE);
}

void NewPageSettingsWindow::ResetDraftToDefaults() {
    draftWidth_ = 612.0f;
    draftHeight_ = 792.0f;
    draftBackgroundColor_ = 0xFFFFFFFFU;
    UpdateEditValues();
    InvalidateRect(window_, nullptr, FALSE);
}

void NewPageSettingsWindow::SyncDimensionPresetSelection() {
    if (dimensionPresetCombo_ == nullptr) {
        return;
    }

    suppressPresetSelection_ = true;
    int selectedIndex = 0;
    for (std::size_t index = 0; index < kDimensionPresets.size(); ++index) {
        const DimensionPreset& preset = kDimensionPresets[index];
        if (NearlyEqual(draftWidth_, preset.width) && NearlyEqual(draftHeight_, preset.height)) {
            selectedIndex = static_cast<int>(index + 1U);
            break;
        }
    }
    SendMessageW(dimensionPresetCombo_, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
    suppressPresetSelection_ = false;
}

void NewPageSettingsWindow::ApplySelectedDimensionPreset() {
    if (dimensionPresetCombo_ == nullptr) {
        return;
    }

    const LRESULT selected = SendMessageW(dimensionPresetCombo_, CB_GETCURSEL, 0, 0);
    if (selected <= 0 || static_cast<std::size_t>(selected - 1) >= kDimensionPresets.size()) {
        return;
    }

    const DimensionPreset& preset = kDimensionPresets[static_cast<std::size_t>(selected - 1)];
    draftWidth_ = preset.width;
    draftHeight_ = preset.height;
    UpdateEditValues();
    InvalidateRect(window_, nullptr, FALSE);
}

bool NewPageSettingsWindow::PasteHexColorFromClipboard() {
    if (!OpenClipboard(window_)) {
        return false;
    }

    bool applied = false;
    HANDLE clipboardData = GetClipboardData(CF_UNICODETEXT);
    if (clipboardData != nullptr) {
        const auto* rawText = static_cast<const wchar_t*>(GlobalLock(clipboardData));
        if (rawText != nullptr) {
            if (const auto parsed = ParseHexColor(rawText); parsed.has_value()) {
                draftBackgroundColor_ = *parsed;
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

void NewPageSettingsWindow::UpdateSliderFromPoint(SliderKind kind, POINT cursor) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_);

    RECT sliderRect {};
    switch (kind) {
    case SliderKind::Red:
        sliderRect = layout.redSliderRect;
        break;
    case SliderKind::Green:
        sliderRect = layout.greenSliderRect;
        break;
    case SliderKind::Blue:
        sliderRect = layout.blueSliderRect;
        break;
    }

    const RECT trackRect = SliderTrackRect(sliderRect);
    const int clampedX = std::clamp(static_cast<int>(cursor.x), static_cast<int>(trackRect.left), static_cast<int>(trackRect.right - 1));
    const int width = std::max(1, static_cast<int>(trackRect.right - trackRect.left));
    const int value = static_cast<int>(std::lround(static_cast<float>(clampedX - trackRect.left) * 255.0f / static_cast<float>(width - 1)));
    SetSliderValue(kind, value);
}

std::optional<NewPageSettingsWindow::SliderKind> NewPageSettingsWindow::HitTestSlider(POINT cursor) const {
    if (window_ == nullptr) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_);
    if (PtInRect(&layout.redSliderRect, cursor) != 0) {
        return SliderKind::Red;
    }
    if (PtInRect(&layout.greenSliderRect, cursor) != 0) {
        return SliderKind::Green;
    }
    if (PtInRect(&layout.blueSliderRect, cursor) != 0) {
        return SliderKind::Blue;
    }
    return std::nullopt;
}

int NewPageSettingsWindow::SliderValue(SliderKind kind) const {
    switch (kind) {
    case SliderKind::Red:
        return app_.GetColorChannel(draftBackgroundColor_, 16);
    case SliderKind::Green:
        return app_.GetColorChannel(draftBackgroundColor_, 8);
    case SliderKind::Blue:
        return app_.GetColorChannel(draftBackgroundColor_, 0);
    default:
        return 0;
    }
}

void NewPageSettingsWindow::SetSliderValue(SliderKind kind, int value) {
    const int clamped = std::clamp(value, 0, 255);
    unsigned char red = app_.GetColorChannel(draftBackgroundColor_, 16);
    unsigned char green = app_.GetColorChannel(draftBackgroundColor_, 8);
    unsigned char blue = app_.GetColorChannel(draftBackgroundColor_, 0);
    switch (kind) {
    case SliderKind::Red:
        red = static_cast<unsigned char>(clamped);
        break;
    case SliderKind::Green:
        green = static_cast<unsigned char>(clamped);
        break;
    case SliderKind::Blue:
        blue = static_cast<unsigned char>(clamped);
        break;
    }
    draftBackgroundColor_ = app_.ComposeColor(red, green, blue);
    InvalidateRect(window_, nullptr, FALSE);
}

void NewPageSettingsWindow::HandleDimensionEdit(HWND edit, float& value, bool normalizeText) {
    if (edit == nullptr || suppressEditRefresh_) {
        return;
    }

    wchar_t buffer[64] = {};
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    if (const auto parsed = ParsePositiveFloat(buffer); parsed.has_value()) {
        value = std::max(72.0f, *parsed);
    }

    if (normalizeText) {
        UpdateEditValues();
    }
}

LRESULT CALLBACK NewPageSettingsWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    NewPageSettingsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<NewPageSettingsWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    } else {
        self = reinterpret_cast<NewPageSettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT NewPageSettingsWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_GETMINMAXINFO:
    {
        auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
        minMax->ptMinTrackSize.x = 700;
        minMax->ptMinTrackSize.y = 540;
        return 0;
    }
    case WM_SIZE:
        LayoutControls();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        wheelRemainder_ += static_cast<int>(std::lround(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) * 72.0f));
        {
            const int scrollPixels = wheelRemainder_ / WHEEL_DELTA;
            wheelRemainder_ -= scrollPixels * WHEEL_DELTA;
            ScrollTo(scrollOffset_ - scrollPixels);
        }
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_);

        if (layout.showScrollbar && PtInRect(&layout.scrollbarThumbRect, cursor) != 0) {
            draggingScrollbar_ = true;
            scrollbarDragOffset_ = cursor.y - layout.scrollbarThumbRect.top;
            SetCapture(window_);
            InvalidateRect(window_, &layout.scrollbarTrackRect, FALSE);
            return 0;
        }
        if (layout.showScrollbar && PtInRect(&layout.scrollbarTrackRect, cursor) != 0) {
            const int pageStep = std::max(48, static_cast<int>(layout.scrollViewportRect.bottom - layout.scrollViewportRect.top) - 96);
            ScrollTo(scrollOffset_ + (cursor.y < layout.scrollbarThumbRect.top ? -pageStep : pageStep));
            return 0;
        }

        for (std::size_t index = 0; index < layout.defaultColorSwatches.size(); ++index) {
            if (PtInRect(&layout.defaultColorSwatches[index], cursor) != 0) {
                draftBackgroundColor_ = kDefaultPageColors[index];
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
        }

        for (std::size_t index = 0; index < layout.previousColorSwatches.size(); ++index) {
            if (PtInRect(&layout.previousColorSwatches[index], cursor) != 0 && index < app_.newPageSettings_.recentColors.size()) {
                draftBackgroundColor_ = app_.newPageSettings_.recentColors[index];
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
            const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_);
            if (!layout.showScrollbar) {
                return 0;
            }

            const int thumbHeight = static_cast<int>(layout.scrollbarThumbRect.bottom - layout.scrollbarThumbRect.top);
            const int maxScroll = std::max(0, contentHeight_ - static_cast<int>(layout.scrollViewportRect.bottom - layout.scrollViewportRect.top));
            const int travel = std::max(1, static_cast<int>(layout.scrollbarTrackRect.bottom - layout.scrollbarTrackRect.top) - thumbHeight);
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
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
        return reinterpret_cast<LRESULT>(HandleControlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
    case WM_DRAWITEM:
    {
        const auto* drawItem = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (drawItem != nullptr && drawItem->CtlType == ODT_BUTTON) {
            DrawButton(*drawItem);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        HandleCommand(wParam, lParam);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        draggingSlider_.reset();
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace cnote
