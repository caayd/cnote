#include "NewPageSettingsWindow.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include "DpiUtil.h"
#include "resource.h"
#include "ui/UiUtil.h"

namespace cnote {

namespace ui = cnote::ui;

namespace {

constexpr wchar_t kWindowClassName[] = L"CnoteNewPageSettingsWindow";
constexpr int kWindowWidth = 780;
constexpr int kWindowHeight = 600;
constexpr DWORD kNewPageWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
constexpr DWORD kNewPageWindowExStyle = WS_EX_DLGMODALFRAME;

constexpr int kIdWidthEdit = 100;
constexpr int kIdHeightEdit = 101;
constexpr int kIdAppendPages = 102;
constexpr int kIdResetDefaults = 103;
constexpr int kIdApplyCurrent = 104;
constexpr int kIdHexColorEdit = 105;
constexpr int kIdApplyFuture = 106;
constexpr int kIdApplyAll = 108;
constexpr UINT kDimensionPresetMenuBase = 400;
constexpr UINT kPaletteProfileMenuBase = 500;

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
    RECT hexColorEditRect {};
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
    RECT paletteProfileLabelRect {};
    RECT paletteProfileComboRect {};
    RECT paletteProfileApplyToggleRect {};
    RECT paletteProfileHintRect {};
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

using ui::DeferMoveControl;
using ui::FillHuePreviewTrack;
using ui::FillLightnessPreviewTrack;
using ui::FillRoundedRect;
using ui::FillSaturationPreviewTrack;
using ui::FormatHexColor;
using ui::FormatHueLabel;
using ui::FormatPercentLabel;
using ui::HslToColor;
using ui::HslColor;
using ui::kAccent;
using ui::kButtonFill;
using ui::kButtonFillActive;
using ui::kInputFill;
using ui::kPanelBorder;
using ui::kPanelFill;
using ui::kScrollbarGap;
using ui::kScrollbarMinThumbHeight;
using ui::kScrollbarThumbActiveFill;
using ui::kScrollbarThumbFill;
using ui::kScrollbarTrackFill;
using ui::kScrollbarWidth;
using ui::kSelectionBorder;
using ui::kTextMuted;
using ui::kTextPrimary;
using ui::kWindowBackground;
using ui::MakeRect;
using ui::ParseHexColor;
using ui::RgbToHsl;
using ui::SetControlText;
using ui::SliderThumbCenterX;
using ui::SliderTrackRect;
using ui::StrokeRoundedRect;

std::wstring FormatDimensionValue(float value) {
    wchar_t buffer[32] = {};
    if (std::abs(value - std::round(value)) < 0.01f) {
        std::swprintf(buffer, std::size(buffer), L"%.0f", value);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%.2f", value);
    }
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

int MatchingDimensionPresetSelection(float width, float height) {
    int selectedIndex = 0;
    for (std::size_t index = 0; index < kDimensionPresets.size(); ++index) {
        const DimensionPreset& preset = kDimensionPresets[index];
        if (NearlyEqual(width, preset.width) && NearlyEqual(height, preset.height)) {
            selectedIndex = static_cast<int>(index + 1U);
            break;
        }
    }
    return selectedIndex;
}

std::wstring DimensionPresetLabelForSelection(int selectedIndex) {
    if (selectedIndex <= 0 || static_cast<std::size_t>(selectedIndex - 1) >= kDimensionPresets.size()) {
        return L"Custom";
    }
    return kDimensionPresets[static_cast<std::size_t>(selectedIndex - 1)].label;
}

bool RectFullyInside(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left
        && inner.top >= outer.top
        && inner.right <= outer.right
        && inner.bottom <= outer.bottom;
}

void SyncScrollableControl(HWND control, const RECT& rect, const RECT& viewportRect, bool visible) {
    if (control == nullptr) {
        return;
    }

    const bool fullyVisible = visible && RectFullyInside(viewportRect, rect);
    if (!fullyVisible) {
        SetWindowRgn(control, nullptr, FALSE);
        SetWindowPos(
            control,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_HIDEWINDOW);
        return;
    }

    SetWindowRgn(control, nullptr, FALSE);
    SetWindowPos(
        control,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
}

NewPageSettingsLayout BuildLayout(RECT clientRect, std::size_t recentColorCount, int scrollOffset, UINT dpi) {
    NewPageSettingsLayout layout {};
    const auto px = [dpi](int value) {
        return ScaleForDpi(value, dpi);
    };
    const int margin = px(16);
    const int gap = px(16);
    const int top = px(48);
    const int actionBarHeight = px(45);
    const int actionBarTop = std::max(top + px(90), static_cast<int>(clientRect.bottom) - margin - actionBarHeight);
    const int actionBarBottom = std::max(actionBarTop + actionBarHeight, static_cast<int>(clientRect.bottom) - margin);
    const int scrollViewportBottom = actionBarTop - px(12);
    const int scrollbarReserve = px(kScrollbarGap + kScrollbarWidth);
    const int rightPanelWidth = std::clamp((static_cast<int>(clientRect.right) - (margin * 2)) / 3, px(240), px(280));
    const int leftPanelWidth = std::max(px(420), static_cast<int>(clientRect.right) - (margin * 2) - gap - rightPanelWidth - scrollbarReserve);
    const int rightLeft = margin + leftPanelWidth + gap;
    const int panelPadding = px(16);
    layout.actionBarRect = MakeRect(margin, actionBarTop, static_cast<int>(clientRect.right) - margin, actionBarBottom);
    layout.scrollViewportRect = MakeRect(margin, top, static_cast<int>(clientRect.right) - margin, scrollViewportBottom);

    const int defaultsLeft = margin;
    const int defaultsRight = margin + leftPanelWidth;
    const int importRight = static_cast<int>(clientRect.right) - margin;
    const int contentLeft = defaultsLeft + panelPadding;
    const int contentRight = defaultsRight - panelPadding;

    layout.colorPreviewRect = MakeRect(contentLeft, top + px(34), contentRight, top + px(110));
    const int previewWidth = static_cast<int>(layout.colorPreviewRect.right - layout.colorPreviewRect.left);
    const int hexEditWidth = std::max(px(84), std::min(px(132), previewWidth - px(24)));
    layout.hexColorEditRect = MakeRect(
        layout.colorPreviewRect.left + ((previewWidth - hexEditWidth) / 2),
        layout.colorPreviewRect.top + px(10),
        layout.colorPreviewRect.left + ((previewWidth - hexEditWidth) / 2) + hexEditWidth,
        layout.colorPreviewRect.top + px(34));

    layout.defaultColorsLabelRect = MakeRect(contentLeft, layout.colorPreviewRect.bottom + px(12), contentRight, layout.colorPreviewRect.bottom + px(30));
    const int defaultColumns = std::max(1, std::min(5, static_cast<int>(kDefaultPageColors.size())));
    const int defaultGap = px(10);
    const int defaultSwatchSize = std::clamp(
        (contentRight - contentLeft - (defaultGap * (defaultColumns - 1))) / defaultColumns,
        px(40),
        px(62));
    const int defaultSwatchHeight = px(38);
    const int defaultSwatchTop = layout.defaultColorsLabelRect.bottom + px(8);
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

    layout.previousColorsLabelRect = MakeRect(contentLeft, defaultGridBottom + px(12), contentRight, defaultGridBottom + px(30));

    const int recentColumns = 6;
    const int recentGap = px(8);
    const int recentSwatchSize = std::clamp((contentRight - contentLeft - (recentGap * (recentColumns - 1))) / recentColumns, px(34), px(44));
    const int recentTop = layout.previousColorsLabelRect.bottom + px(8);
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

    const int slidersTop = recentTop + (recentSwatchSize * recentRows) + (recentGap * std::max(0, recentRows - 1)) + px(16);
    layout.redLabelRect = MakeRect(contentLeft, slidersTop, contentRight, slidersTop + px(18));
    layout.redSliderRect = MakeRect(contentLeft, layout.redLabelRect.bottom + px(2), contentRight, layout.redLabelRect.bottom + px(26));
    layout.greenLabelRect = MakeRect(contentLeft, layout.redSliderRect.bottom + px(6), contentRight, layout.redSliderRect.bottom + px(24));
    layout.greenSliderRect = MakeRect(contentLeft, layout.greenLabelRect.bottom + px(2), contentRight, layout.greenLabelRect.bottom + px(26));
    layout.blueLabelRect = MakeRect(contentLeft, layout.greenSliderRect.bottom + px(6), contentRight, layout.greenSliderRect.bottom + px(24));
    layout.blueSliderRect = MakeRect(contentLeft, layout.blueLabelRect.bottom + px(2), contentRight, layout.blueLabelRect.bottom + px(26));

    const int fieldGap = px(18);
    const int fieldWidth = std::clamp((contentRight - contentLeft - fieldGap) / 2, px(132), px(164));
    const int fieldHeight = px(28);
    layout.widthLabelRect = MakeRect(contentLeft, layout.blueSliderRect.bottom + px(14), contentLeft + fieldWidth, layout.blueSliderRect.bottom + px(32));
    layout.widthEditRect = MakeRect(contentLeft, layout.widthLabelRect.bottom + px(4), contentLeft + fieldWidth, layout.widthLabelRect.bottom + px(4) + fieldHeight);
    layout.heightLabelRect = MakeRect(contentLeft + fieldWidth + fieldGap, layout.blueSliderRect.bottom + px(14), contentLeft + (fieldWidth * 2) + fieldGap, layout.blueSliderRect.bottom + px(32));
    layout.heightEditRect = MakeRect(contentLeft + fieldWidth + fieldGap, layout.heightLabelRect.bottom + px(4), contentLeft + (fieldWidth * 2) + fieldGap, layout.heightLabelRect.bottom + px(4) + fieldHeight);
    layout.dimensionsLabelRect = MakeRect(contentLeft, layout.heightEditRect.bottom + px(12), contentRight, layout.heightEditRect.bottom + px(30));
    layout.dimensionsComboRect = MakeRect(contentLeft, layout.dimensionsLabelRect.bottom + px(4), contentRight, layout.dimensionsLabelRect.bottom + px(32));
    layout.paletteProfileLabelRect = MakeRect(contentLeft, layout.dimensionsComboRect.bottom + px(12), contentRight, layout.dimensionsComboRect.bottom + px(30));
    layout.paletteProfileComboRect = MakeRect(contentLeft, layout.paletteProfileLabelRect.bottom + px(4), contentRight, layout.paletteProfileLabelRect.bottom + px(32));
    layout.paletteProfileApplyToggleRect = MakeRect(contentLeft, layout.paletteProfileComboRect.bottom + px(10), contentRight, layout.paletteProfileComboRect.bottom + px(38));
    layout.paletteProfileHintRect = MakeRect(contentLeft, layout.paletteProfileApplyToggleRect.bottom + px(6), contentRight, layout.paletteProfileApplyToggleRect.bottom + px(54));

    const int importContentLeft = rightLeft + panelPadding;
    const int importContentRight = importRight - panelPadding;
    layout.appendButtonRect = MakeRect(
        importContentLeft,
        top + px(44),
        importContentRight,
        top + px(76));
    layout.importHintRect = MakeRect(
        importContentLeft,
        layout.appendButtonRect.bottom + px(18),
        importContentRight,
        layout.appendButtonRect.bottom + px(180));

    const int leftContentBottom = layout.paletteProfileHintRect.bottom + panelPadding;
    const int rightContentBottom = layout.importHintRect.bottom + panelPadding;
    layout.defaultsPanel = MakeRect(defaultsLeft, top, defaultsRight, std::max(scrollViewportBottom, leftContentBottom));
    layout.importPanel = MakeRect(rightLeft, top, importRight, std::max(scrollViewportBottom, rightContentBottom));

    const int buttonGap = px(12);
    const int resetButtonWidth = px(136);
    const int applyCurrentWidth = px(164);
    const int applyFutureWidth = px(170);
    const int applyAllWidth = px(118);
    const int actionButtonTop = layout.actionBarRect.top + ((actionBarHeight - px(28)) / 2);
    int actionButtonLeft = layout.actionBarRect.left + px(16);
    layout.resetButtonRect = MakeRect(actionButtonLeft, actionButtonTop, actionButtonLeft + resetButtonWidth, actionButtonTop + px(28));
    actionButtonLeft = layout.resetButtonRect.right + buttonGap;
    layout.applyCurrentButtonRect = MakeRect(actionButtonLeft, actionButtonTop, actionButtonLeft + applyCurrentWidth, actionButtonTop + px(28));
    actionButtonLeft = layout.applyCurrentButtonRect.right + buttonGap;
    layout.applyFutureButtonRect = MakeRect(actionButtonLeft, actionButtonTop, actionButtonLeft + applyFutureWidth, actionButtonTop + px(28));
    actionButtonLeft = layout.applyFutureButtonRect.right + buttonGap;
    layout.applyAllButtonRect = MakeRect(actionButtonLeft, actionButtonTop, actionButtonLeft + applyAllWidth, actionButtonTop + px(28));

    layout.contentHeight = std::max(layout.defaultsPanel.bottom, layout.importPanel.bottom) - top + margin;
    layout.scrollbarTrackRect = MakeRect(clientRect.right - margin - px(kScrollbarWidth), top, clientRect.right - margin, scrollViewportBottom);
    const int viewportHeight = std::max(1, scrollViewportBottom - top);
    const int maxScroll = std::max(0, layout.contentHeight - viewportHeight);
    layout.showScrollbar = maxScroll > 0 && (layout.scrollbarTrackRect.bottom - layout.scrollbarTrackRect.top) > 0;
    if (layout.showScrollbar) {
        const int trackHeight = layout.scrollbarTrackRect.bottom - layout.scrollbarTrackRect.top;
        const int thumbHeight = std::clamp((viewportHeight * trackHeight) / std::max(1, layout.contentHeight), px(kScrollbarMinThumbHeight), trackHeight);
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
    offsetY(layout.hexColorEditRect);
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
    offsetY(layout.paletteProfileLabelRect);
    offsetY(layout.paletteProfileComboRect);
    offsetY(layout.paletteProfileApplyToggleRect);
    offsetY(layout.paletteProfileHintRect);
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

    currentDpi_ = owner != nullptr ? WindowDpi(owner) : SystemDpi();
    RECT windowRect { 0, 0, Scale(kWindowWidth), Scale(kWindowHeight) };
    AdjustWindowRectForDpi(&windowRect, kNewPageWindowStyle, FALSE, kNewPageWindowExStyle, currentDpi_);
    window_ = CreateWindowExW(
        kNewPageWindowExStyle,
        kWindowClassName,
        L"New Page Settings",
        kNewPageWindowStyle,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        owner,
        nullptr,
        app_.instance_,
        this);

    if (window_ == nullptr) {
        return;
    }

    currentDpi_ = WindowDpi(window_);
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
    const UINT dpi = CurrentDpi();
    const auto createButton = [&](const wchar_t* text, int id, int width) {
        return CreateWindowExW(
            0,
            L"BUTTON",
            text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0,
            0,
            width,
            ScaleForDpi(28, dpi),
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
            ScaleForDpi(148, dpi),
            ScaleForDpi(28, dpi),
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            app_.instance_,
            nullptr);
    };

    widthEdit_ = createEdit(kIdWidthEdit);
    heightEdit_ = createEdit(kIdHeightEdit);
    hexColorEdit_ = createEdit(kIdHexColorEdit);
    SetWindowSubclass(hexColorEdit_, ui::EditReturnToParentSubclassProc, 0, 0);
    appendButton_ = createButton(L"Append Pages...", kIdAppendPages, ScaleForDpi(164, dpi));
    resetButton_ = createButton(L"Reset Defaults", kIdResetDefaults, ScaleForDpi(164, dpi));
    applyCurrentButton_ = createButton(L"Apply to Current Page", kIdApplyCurrent, ScaleForDpi(180, dpi));
    applyAllButton_ = createButton(L"Apply to All", kIdApplyAll, ScaleForDpi(118, dpi));
    applyFutureButton_ = createButton(L"Apply to Future Pages", kIdApplyFuture, ScaleForDpi(180, dpi));
    SendMessageW(hexColorEdit_, EM_LIMITTEXT, 10, 0);

    SyncDraftFromApp();
    LayoutControls();
    UpdateEditValues();
}

void NewPageSettingsWindow::ApplyFonts() {
    RebuildDpiResources(CurrentDpi());
}

void NewPageSettingsWindow::RebuildDpiResources(UINT dpi) {
    currentDpi_ = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
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

    font_ = CreateFontW(FontHeightForDpi(14, currentDpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    titleFont_ = CreateFontW(FontHeightForDpi(18, currentDpi_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    backgroundBrush_ = CreateSolidBrush(kWindowBackground);
    panelBrush_ = CreateSolidBrush(kPanelFill);
    inputBrush_ = CreateSolidBrush(kInputFill);
    toolbarBrush_ = CreateSolidBrush(RGB(15, 20, 27));

    const HWND controls[] {
        widthEdit_,
        heightEdit_,
        hexColorEdit_,
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
}

void NewPageSettingsWindow::LayoutControls() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());
    contentHeight_ = layout.contentHeight;
    const int previousScrollOffset = scrollOffset_;
    UpdateScrollInfo();
    if (scrollOffset_ != previousScrollOffset) {
        layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());
        contentHeight_ = layout.contentHeight;
    }

    SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    HDWP deferred = BeginDeferWindowPos(8);
    constexpr UINT deferFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;
    DeferMoveControl(deferred, widthEdit_, layout.widthEditRect, deferFlags);
    DeferMoveControl(deferred, heightEdit_, layout.heightEditRect, deferFlags);
    DeferMoveControl(deferred, hexColorEdit_, layout.hexColorEditRect, deferFlags);
    DeferMoveControl(deferred, appendButton_, layout.appendButtonRect, deferFlags);
    DeferMoveControl(deferred, resetButton_, layout.resetButtonRect, deferFlags);
    DeferMoveControl(deferred, applyCurrentButton_, layout.applyCurrentButtonRect, deferFlags);
    DeferMoveControl(deferred, applyAllButton_, layout.applyAllButtonRect, deferFlags);
    DeferMoveControl(deferred, applyFutureButton_, layout.applyFutureButtonRect, deferFlags);
    if (deferred != nullptr) {
        EndDeferWindowPos(deferred);
    }

    SyncScrollableControl(widthEdit_, layout.widthEditRect, layout.scrollViewportRect, true);
    SyncScrollableControl(heightEdit_, layout.heightEditRect, layout.scrollViewportRect, true);
    SyncScrollableControl(hexColorEdit_, layout.hexColorEditRect, layout.scrollViewportRect, editingHexColor_);
    SyncScrollableControl(appendButton_, layout.appendButtonRect, layout.scrollViewportRect, true);
    for (HWND control : { resetButton_, applyCurrentButton_, applyFutureButton_, applyAllButton_ }) {
        if (control != nullptr) {
            SetWindowPos(
                control,
                HWND_TOP,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
        }
    }
    SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_NOERASE);
}

void NewPageSettingsWindow::UpdateScrollInfo() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());
    const int viewportHeight = std::max(1, static_cast<int>(layout.scrollViewportRect.bottom - layout.scrollViewportRect.top));
    const int maxScroll = std::max(0, contentHeight_ - viewportHeight);
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);
}

UINT NewPageSettingsWindow::CurrentDpi() const {
    return currentDpi_ == 0 ? USER_DEFAULT_SCREEN_DPI : currentDpi_;
}

int NewPageSettingsWindow::Scale(int value) const {
    return ScaleForDpi(value, CurrentDpi());
}

void NewPageSettingsWindow::ScrollTo(int scrollOffset) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());
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
    draftPaletteProfile_ = app_.ResolvedPaletteProfile(app_.newPageSettings_.paletteProfile);
    draftApplyPaletteProfileToExistingStrokes_ = app_.newPageSettings_.applyPaletteProfileToExistingStrokes;
}

void NewPageSettingsWindow::UpdateEditValues() {
    if (suppressEditRefresh_) {
        return;
    }

    suppressEditRefresh_ = true;
    SetControlText(widthEdit_, FormatDimensionValue(draftWidth_));
    SetControlText(heightEdit_, FormatDimensionValue(draftHeight_));
    if (!editingHexColor_) {
        SetControlText(hexColorEdit_, FormatHexColor(draftBackgroundColor_));
    }
    suppressEditRefresh_ = false;
}

void NewPageSettingsWindow::HandleCommand(WPARAM wParam, LPARAM lParam) {
    const int controlId = LOWORD(wParam);
    const int notification = HIWORD(wParam);
    switch (controlId) {
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
    case kIdHexColorEdit:
        if (notification == EN_KILLFOCUS) {
            CommitHexColorEdit(true);
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
    wchar_t className[32] = {};
    GetClassNameW(control, className, static_cast<int>(std::size(className)));
    if (lstrcmpW(className, L"ComboLBox") == 0) {
        return panelBrush_;
    }
    if (control == widthEdit_ || control == heightEdit_ || control == hexColorEdit_) {
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
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());

    HDC memoryDc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, clientRect.right, clientRect.bottom);
    HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    FillRect(memoryDc, &clientRect, backgroundBrush_);

    SetBkMode(memoryDc, TRANSPARENT);
    HGDIOBJ previousFont = SelectObject(memoryDc, titleFont_ != nullptr ? titleFont_ : font_);
    SetTextColor(memoryDc, kTextPrimary);
    TextOutW(memoryDc, Scale(16), Scale(12), L"New Page Settings", 17);

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
    TextOutW(memoryDc, layout.defaultsPanel.left + Scale(16), layout.defaultsPanel.top + Scale(12), L"Defaults", 8);
    TextOutW(memoryDc, layout.importPanel.left + Scale(16), layout.importPanel.top + Scale(12), L"Import", 6);

    FillRoundedRect(memoryDc, layout.colorPreviewRect, app_.ToColorRef(draftBackgroundColor_));
    StrokeRoundedRect(memoryDc, layout.colorPreviewRect, kSelectionBorder, 2);
    RECT previewTextRect = layout.colorPreviewRect;
    previewTextRect.top += Scale(10);
    previewTextRect.bottom -= Scale(10);
    const COLORREF previewText = (app_.GetColorChannel(draftBackgroundColor_, 16)
            + app_.GetColorChannel(draftBackgroundColor_, 8)
            + app_.GetColorChannel(draftBackgroundColor_, 0)) > 384
        ? RGB(22, 28, 36)
        : RGB(248, 250, 252);
    SetTextColor(memoryDc, previewText);
    if (!editingHexColor_) {
        RECT previewHexRect = layout.hexColorEditRect;
        DrawTextW(memoryDc, FormatHexColor(draftBackgroundColor_).c_str(), -1, &previewHexRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

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
        RECT textRect = labelRect;
        SetTextColor(memoryDc, kTextPrimary);
        const std::wstring valueText = [&]() -> std::wstring {
            switch (kind) {
            case SliderKind::Hue:
                return FormatHueLabel(SliderValue(kind));
            case SliderKind::Saturation:
                return FormatPercentLabel(L"Saturation", SliderValue(kind));
            case SliderKind::Lightness:
                return FormatPercentLabel(L"Lightness", SliderValue(kind));
            default:
                return label;
            }
        }();
        DrawTextW(memoryDc, valueText.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        const RECT trackRect = SliderTrackRect(sliderRect, CurrentDpi());
        const HslColor hsl = RgbToHsl(draftBackgroundColor_);
        const int maximum = kind == SliderKind::Hue ? 360 : 100;
        switch (kind) {
        case SliderKind::Hue:
            FillHuePreviewTrack(memoryDc, trackRect, hsl.saturation, hsl.lightness);
            break;
        case SliderKind::Saturation:
            FillSaturationPreviewTrack(memoryDc, trackRect, hsl.hue, hsl.lightness);
            break;
        case SliderKind::Lightness:
            FillLightnessPreviewTrack(memoryDc, trackRect, hsl.hue, hsl.saturation);
            break;
        default:
            FillRoundedRect(memoryDc, trackRect, RGB(41, 51, 66));
            break;
        }
        const int thumbCenterX = SliderThumbCenterX(trackRect, SliderValue(kind), 0, maximum);

        const int centerY = (sliderRect.top + sliderRect.bottom) / 2;
        RECT thumbRect = MakeRect(
            thumbCenterX - Scale(6),
            centerY - Scale(6),
            thumbCenterX + Scale(7),
            centerY + Scale(7));
        FillRoundedRect(memoryDc, thumbRect, RGB(244, 247, 252));
        StrokeRoundedRect(memoryDc, thumbRect, draggingSlider_ == kind ? RGB(255, 255, 255) : RGB(120, 138, 162), 1);
    };

    paintSlider(SliderKind::Hue, layout.redLabelRect, layout.redSliderRect, L"Hue");
    paintSlider(SliderKind::Saturation, layout.greenLabelRect, layout.greenSliderRect, L"Saturation");
    paintSlider(SliderKind::Lightness, layout.blueLabelRect, layout.blueSliderRect, L"Lightness");

    RECT widthLabelRect = layout.widthLabelRect;
    RECT heightLabelRect = layout.heightLabelRect;
    RECT dimensionsLabelRect = layout.dimensionsLabelRect;
    RECT paletteProfileLabelRect = layout.paletteProfileLabelRect;
    SetTextColor(memoryDc, kTextPrimary);
    DrawTextW(memoryDc, L"Width (pt)", -1, &widthLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(memoryDc, L"Height (pt)", -1, &heightLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(memoryDc, L"Common Sizes", -1, &dimensionsLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(memoryDc, L"Stroke Color Profile", -1, &paletteProfileLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    FillRoundedRect(memoryDc, layout.dimensionsComboRect, kInputFill);
    StrokeRoundedRect(memoryDc, layout.dimensionsComboRect, kPanelBorder, 1);
    RECT comboTextRect = layout.dimensionsComboRect;
    comboTextRect.left += Scale(12);
    comboTextRect.right -= Scale(30);
    const std::wstring comboLabel = DimensionPresetLabelForSelection(MatchingDimensionPresetSelection(draftWidth_, draftHeight_));
    DrawTextW(memoryDc, comboLabel.c_str(), -1, &comboTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int comboCenterX = layout.dimensionsComboRect.right - Scale(16);
    const int comboCenterY = (layout.dimensionsComboRect.top + layout.dimensionsComboRect.bottom) / 2;
    POINT comboArrow[3] {
        { comboCenterX - Scale(4), comboCenterY - Scale(2) },
        { comboCenterX + Scale(4), comboCenterY - Scale(2) },
        { comboCenterX, comboCenterY + Scale(3) },
    };
    HBRUSH arrowBrush = CreateSolidBrush(kTextPrimary);
    HPEN arrowPen = CreatePen(PS_SOLID, 1, kTextPrimary);
    HGDIOBJ previousBrush = SelectObject(memoryDc, arrowBrush);
    HGDIOBJ previousPen = SelectObject(memoryDc, arrowPen);
    Polygon(memoryDc, comboArrow, 3);
    SelectObject(memoryDc, previousPen);
    SelectObject(memoryDc, previousBrush);
    DeleteObject(arrowPen);
    DeleteObject(arrowBrush);

    FillRoundedRect(memoryDc, layout.paletteProfileComboRect, kInputFill);
    StrokeRoundedRect(memoryDc, layout.paletteProfileComboRect, kPanelBorder, 1);
    RECT paletteProfileTextRect = layout.paletteProfileComboRect;
    paletteProfileTextRect.left += Scale(12);
    paletteProfileTextRect.right -= Scale(30);
    const std::vector<std::uint32_t> previewPaletteProfile = app_.ResolvedPaletteProfile(draftPaletteProfile_);
    const int paletteSwatchSize = Scale(12);
    const int paletteSwatchGap = Scale(6);
    int paletteSwatchLeft = paletteProfileTextRect.left;
    const int paletteSwatchTop = ((layout.paletteProfileComboRect.top + layout.paletteProfileComboRect.bottom) / 2) - (paletteSwatchSize / 2);
    for (std::size_t index = 0; index < std::min<std::size_t>(previewPaletteProfile.size(), 4U); ++index) {
        const RECT swatchRect = MakeRect(
            paletteSwatchLeft,
            paletteSwatchTop,
            paletteSwatchLeft + paletteSwatchSize,
            paletteSwatchTop + paletteSwatchSize);
        FillRoundedRect(memoryDc, swatchRect, app_.ToColorRef(previewPaletteProfile[index]));
        StrokeRoundedRect(memoryDc, swatchRect, kPanelBorder, 1);
        paletteSwatchLeft += paletteSwatchSize + paletteSwatchGap;
    }
    paletteProfileTextRect.left = paletteSwatchLeft + Scale(4);
    DrawTextW(memoryDc, DraftPaletteProfileLabel().c_str(), -1, &paletteProfileTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int paletteComboCenterX = layout.paletteProfileComboRect.right - Scale(16);
    const int paletteComboCenterY = (layout.paletteProfileComboRect.top + layout.paletteProfileComboRect.bottom) / 2;
    POINT paletteComboArrow[3] {
        { paletteComboCenterX - Scale(4), paletteComboCenterY - Scale(2) },
        { paletteComboCenterX + Scale(4), paletteComboCenterY - Scale(2) },
        { paletteComboCenterX, paletteComboCenterY + Scale(3) },
    };
    HBRUSH paletteArrowBrush = CreateSolidBrush(kTextPrimary);
    HPEN paletteArrowPen = CreatePen(PS_SOLID, 1, kTextPrimary);
    previousBrush = SelectObject(memoryDc, paletteArrowBrush);
    previousPen = SelectObject(memoryDc, paletteArrowPen);
    Polygon(memoryDc, paletteComboArrow, 3);
    SelectObject(memoryDc, previousPen);
    SelectObject(memoryDc, previousBrush);
    DeleteObject(paletteArrowPen);
    DeleteObject(paletteArrowBrush);

    const RECT toggleBoxRect = MakeRect(
        layout.paletteProfileApplyToggleRect.left,
        layout.paletteProfileApplyToggleRect.top + Scale(4),
        layout.paletteProfileApplyToggleRect.left + Scale(20),
        layout.paletteProfileApplyToggleRect.top + Scale(24));
    FillRoundedRect(memoryDc, toggleBoxRect, draftApplyPaletteProfileToExistingStrokes_ ? RGB(32, 63, 102) : kInputFill);
    StrokeRoundedRect(memoryDc, toggleBoxRect, draftApplyPaletteProfileToExistingStrokes_ ? kAccent : kPanelBorder, draftApplyPaletteProfileToExistingStrokes_ ? 2 : 1);
    if (draftApplyPaletteProfileToExistingStrokes_) {
        HPEN checkPen = CreatePen(PS_SOLID, std::max(1, Scale(2)), RGB(244, 247, 252));
        previousPen = SelectObject(memoryDc, checkPen);
        MoveToEx(memoryDc, toggleBoxRect.left + Scale(5), toggleBoxRect.top + Scale(10), nullptr);
        LineTo(memoryDc, toggleBoxRect.left + Scale(9), toggleBoxRect.top + Scale(14));
        LineTo(memoryDc, toggleBoxRect.left + Scale(15), toggleBoxRect.top + Scale(6));
        SelectObject(memoryDc, previousPen);
        DeleteObject(checkPen);
    }
    RECT toggleTextRect = layout.paletteProfileApplyToggleRect;
    toggleTextRect.left = toggleBoxRect.right + Scale(10);
    SetTextColor(memoryDc, kTextPrimary);
    DrawTextW(memoryDc, L"Swap existing stroke colors on Current/All apply", -1, &toggleTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT paletteProfileHintRect = layout.paletteProfileHintRect;
    SetTextColor(memoryDc, kTextMuted);
    DrawTextW(
        memoryDc,
        L"Current and All remap strokes from each page's stored profile. Future Pages stores the selected profile for new pages.",
        -1,
        &paletteProfileHintRect,
        DT_WORDBREAK);

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

    const int paintWidth = std::max(0L, paint.rcPaint.right - paint.rcPaint.left);
    const int paintHeight = std::max(0L, paint.rcPaint.bottom - paint.rcPaint.top);
    if (paintWidth > 0 && paintHeight > 0) {
        BitBlt(windowDc, paint.rcPaint.left, paint.rcPaint.top, paintWidth, paintHeight, memoryDc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
    }

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
    const COLORREF background = controlId == kIdResetDefaults || controlId == kIdApplyCurrent || controlId == kIdApplyAll || controlId == kIdApplyFuture
        ? RGB(15, 20, 27)
        : kPanelFill;
    const COLORREF fill = selected
        ? (emphasized ? RGB(49, 82, 125) : kButtonFillActive)
        : (emphasized ? RGB(32, 63, 102) : kButtonFill);
    const COLORREF border = selected ? kAccent : fill;
    const COLORREF text = disabled ? RGB(102, 111, 124) : kTextPrimary;

    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(drawItem.hDC, &drawItem.rcItem, backgroundBrush);
    DeleteObject(backgroundBrush);

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
    CommitHexColorEdit(true);
    HandleDimensionEdit(widthEdit_, draftWidth_, true);
    HandleDimensionEdit(heightEdit_, draftHeight_, true);
    app_.ApplyConfiguredSettingsToCurrentPage(
        draftWidth_,
        draftHeight_,
        draftBackgroundColor_,
        draftPaletteProfile_,
        draftApplyPaletteProfileToExistingStrokes_);
}

void NewPageSettingsWindow::ApplyDraftToAllPages() {
    CommitHexColorEdit(true);
    HandleDimensionEdit(widthEdit_, draftWidth_, true);
    HandleDimensionEdit(heightEdit_, draftHeight_, true);
    app_.ApplyConfiguredSettingsToAllPages(
        draftWidth_,
        draftHeight_,
        draftBackgroundColor_,
        draftPaletteProfile_,
        draftApplyPaletteProfileToExistingStrokes_);
}

void NewPageSettingsWindow::ApplyDraftToFuturePages() {
    CommitHexColorEdit(true);
    HandleDimensionEdit(widthEdit_, draftWidth_, true);
    HandleDimensionEdit(heightEdit_, draftHeight_, true);

    const std::uint32_t previousColor = app_.newPageSettings_.backgroundColor;
    app_.newPageSettings_.width = std::max(72.0f, draftWidth_);
    app_.newPageSettings_.height = std::max(72.0f, draftHeight_);
    app_.newPageSettings_.backgroundColor = draftBackgroundColor_;
    app_.newPageSettings_.paletteProfile = app_.ResolvedPaletteProfile(draftPaletteProfile_);
    app_.newPageSettings_.applyPaletteProfileToExistingStrokes = draftApplyPaletteProfileToExistingStrokes_;
    app_.document_.SetNewPageDefaults({
        app_.newPageSettings_.width,
        app_.newPageSettings_.height,
        app_.newPageSettings_.backgroundColor,
        app_.newPageSettings_.paletteProfile,
    });
    app_.NotifyDocumentMutated();

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
    draftPaletteProfile_ = app_.ResolvedPaletteProfile({});
    draftApplyPaletteProfileToExistingStrokes_ = false;
    UpdateEditValues();
    InvalidateRect(window_, nullptr, FALSE);
}

void NewPageSettingsWindow::BeginHexColorEdit() {
    if (window_ == nullptr || hexColorEdit_ == nullptr) {
        return;
    }

    editingHexColor_ = true;
    SetControlText(hexColorEdit_, FormatHexColor(draftBackgroundColor_));
    ShowWindow(hexColorEdit_, SW_SHOWNA);
    SetFocus(hexColorEdit_);
    SendMessageW(hexColorEdit_, EM_SETSEL, 0, -1);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());
    RedrawWindow(window_, &layout.colorPreviewRect, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

void NewPageSettingsWindow::CommitHexColorEdit(bool applyChanges) {
    if (hexColorEdit_ == nullptr || !editingHexColor_) {
        return;
    }

    if (applyChanges) {
        wchar_t buffer[32] = {};
        GetWindowTextW(hexColorEdit_, buffer, static_cast<int>(std::size(buffer)));
        if (const auto parsed = ParseHexColor(buffer); parsed.has_value()) {
            draftBackgroundColor_ = *parsed;
        }
    }

    editingHexColor_ = false;
    ShowWindow(hexColorEdit_, SW_HIDE);
    UpdateEditValues();

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());
    RedrawWindow(window_, &layout.colorPreviewRect, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

void NewPageSettingsWindow::OpenDimensionPresetMenu() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    const int selectedIndex = MatchingDimensionPresetSelection(draftWidth_, draftHeight_);
    AppendMenuW(menu, MF_STRING | (selectedIndex == 0 ? MF_CHECKED : 0), kDimensionPresetMenuBase, L"Custom");
    for (std::size_t index = 0; index < kDimensionPresets.size(); ++index) {
        const UINT flags = MF_STRING | (selectedIndex == static_cast<int>(index + 1U) ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, kDimensionPresetMenuBase + 1U + static_cast<UINT>(index), kDimensionPresets[index].label);
    }

    POINT anchor { layout.dimensionsComboRect.left, layout.dimensionsComboRect.bottom };
    ClientToScreen(window_, &anchor);
    const UINT command = TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, anchor.x, anchor.y, window_, nullptr);
    DestroyMenu(menu);

    if (command <= kDimensionPresetMenuBase) {
        RedrawWindow(window_, &layout.dimensionsComboRect, nullptr, RDW_INVALIDATE | RDW_NOERASE);
        return;
    }

    const std::size_t presetIndex = static_cast<std::size_t>(command - kDimensionPresetMenuBase - 1U);
    if (presetIndex < kDimensionPresets.size()) {
        const DimensionPreset& preset = kDimensionPresets[presetIndex];
        draftWidth_ = preset.width;
        draftHeight_ = preset.height;
        UpdateEditValues();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
        return;
    }

    RedrawWindow(window_, &layout.dimensionsComboRect, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

std::wstring NewPageSettingsWindow::DraftPaletteProfileLabel() const {
    const std::vector<std::uint32_t> resolvedPaletteProfile = app_.ResolvedPaletteProfile(draftPaletteProfile_);
    if (resolvedPaletteProfile == app_.palette_) {
        return L"Current Palette";
    }
    for (const App::NamedPalette& palette : app_.savedPalettes_) {
        if (palette.colors == resolvedPaletteProfile && !palette.name.empty()) {
            return palette.name;
        }
    }
    return L"Custom Profile";
}

void NewPageSettingsWindow::OpenPaletteProfileMenu() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    const std::vector<std::uint32_t> resolvedPaletteProfile = app_.ResolvedPaletteProfile(draftPaletteProfile_);
    AppendMenuW(menu, MF_STRING | (resolvedPaletteProfile == app_.palette_ ? MF_CHECKED : 0), kPaletteProfileMenuBase, L"Current Palette");
    if (!app_.savedPalettes_.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    for (std::size_t index = 0; index < app_.savedPalettes_.size(); ++index) {
        const App::NamedPalette& palette = app_.savedPalettes_[index];
        if (palette.colors.empty()) {
            continue;
        }
        const UINT flags = MF_STRING | (palette.colors == resolvedPaletteProfile ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, kPaletteProfileMenuBase + 1U + static_cast<UINT>(index), palette.name.c_str());
    }

    POINT anchor { layout.paletteProfileComboRect.left, layout.paletteProfileComboRect.bottom };
    ClientToScreen(window_, &anchor);
    const UINT command = TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, anchor.x, anchor.y, window_, nullptr);
    DestroyMenu(menu);

    if (command == 0U) {
        RedrawWindow(window_, &layout.paletteProfileComboRect, nullptr, RDW_INVALIDATE | RDW_NOERASE);
        return;
    }

    if (command == kPaletteProfileMenuBase) {
        draftPaletteProfile_ = app_.ResolvedPaletteProfile({});
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    const std::size_t paletteIndex = static_cast<std::size_t>(command - kPaletteProfileMenuBase - 1U);
    if (paletteIndex < app_.savedPalettes_.size()) {
        draftPaletteProfile_ = app_.savedPalettes_[paletteIndex].colors;
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    RedrawWindow(window_, &layout.paletteProfileComboRect, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

void NewPageSettingsWindow::UpdateSliderFromPoint(SliderKind kind, POINT cursor) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());

    RECT sliderRect {};
    int maximum = 100;
    switch (kind) {
    case SliderKind::Hue:
        sliderRect = layout.redSliderRect;
        maximum = 360;
        break;
    case SliderKind::Saturation:
        sliderRect = layout.greenSliderRect;
        break;
    case SliderKind::Lightness:
        sliderRect = layout.blueSliderRect;
        break;
    }

    const RECT trackRect = SliderTrackRect(sliderRect, CurrentDpi());
    const int clampedX = std::clamp(static_cast<int>(cursor.x), static_cast<int>(trackRect.left), static_cast<int>(trackRect.right - 1));
    const int width = std::max(1, static_cast<int>(trackRect.right - trackRect.left));
    const int value = static_cast<int>(std::lround(static_cast<float>(clampedX - trackRect.left) * static_cast<float>(maximum) / static_cast<float>(width - 1)));
    SetSliderValue(kind, value);
}

std::optional<NewPageSettingsWindow::SliderKind> NewPageSettingsWindow::HitTestSlider(POINT cursor) const {
    if (window_ == nullptr) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());
    const std::array<std::pair<SliderKind, RECT>, 3> sliders {{
        { SliderKind::Hue, layout.redSliderRect },
        { SliderKind::Saturation, layout.greenSliderRect },
        { SliderKind::Lightness, layout.blueSliderRect },
    }};
    for (const auto& [kind, rect] : sliders) {
        RECT hitRect = rect;
        hitRect.top -= Scale(4);
        hitRect.bottom += Scale(4);
        if (PtInRect(&hitRect, cursor) != 0) {
            return kind;
        }
    }
    return std::nullopt;
}

int NewPageSettingsWindow::SliderValue(SliderKind kind) const {
    const HslColor hsl = RgbToHsl(draftBackgroundColor_);
    switch (kind) {
    case SliderKind::Hue:
        return static_cast<int>(std::lround(hsl.hue));
    case SliderKind::Saturation:
        return static_cast<int>(std::lround(hsl.saturation * 100.0f));
    case SliderKind::Lightness:
        return static_cast<int>(std::lround(hsl.lightness * 100.0f));
    default:
        return 0;
    }
}

void NewPageSettingsWindow::SetSliderValue(SliderKind kind, int value) {
    HslColor hsl = RgbToHsl(draftBackgroundColor_);
    switch (kind) {
    case SliderKind::Hue:
        hsl.hue = static_cast<float>(std::clamp(value, 0, 360));
        break;
    case SliderKind::Saturation:
        hsl.saturation = static_cast<float>(std::clamp(value, 0, 100)) / 100.0f;
        break;
    case SliderKind::Lightness:
        hsl.lightness = static_cast<float>(std::clamp(value, 0, 100)) / 100.0f;
        break;
    }
    draftBackgroundColor_ = HslToColor(hsl.hue, hsl.saturation, hsl.lightness);
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
        minMax->ptMinTrackSize.x = Scale(700);
        minMax->ptMinTrackSize.y = Scale(540);
        return 0;
    }
    case WM_SIZE:
        LayoutControls();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED:
        currentDpi_ = HIWORD(wParam);
        if (const RECT* suggestedRect = reinterpret_cast<const RECT*>(lParam); suggestedRect != nullptr) {
            SetWindowPos(
                window_,
                nullptr,
                suggestedRect->left,
                suggestedRect->top,
                suggestedRect->right - suggestedRect->left,
                suggestedRect->bottom - suggestedRect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RebuildDpiResources(currentDpi_);
        LayoutControls();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        wheelRemainder_ += static_cast<int>(std::lround(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) * static_cast<float>(Scale(72))));
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
        const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());

        if (editingHexColor_ && PtInRect(&layout.hexColorEditRect, cursor) == 0) {
            CommitHexColorEdit(true);
        }

        if (layout.showScrollbar && PtInRect(&layout.scrollbarThumbRect, cursor) != 0) {
            draggingScrollbar_ = true;
            scrollbarDragOffset_ = cursor.y - layout.scrollbarThumbRect.top;
            SetCapture(window_);
            InvalidateRect(window_, &layout.scrollbarTrackRect, FALSE);
            return 0;
        }
        if (layout.showScrollbar && PtInRect(&layout.scrollbarTrackRect, cursor) != 0) {
            const int pageStep = std::max(Scale(48), static_cast<int>(layout.scrollViewportRect.bottom - layout.scrollViewportRect.top) - Scale(96));
            ScrollTo(scrollOffset_ + (cursor.y < layout.scrollbarThumbRect.top ? -pageStep : pageStep));
            return 0;
        }
        if (PtInRect(&layout.dimensionsComboRect, cursor) != 0) {
            OpenDimensionPresetMenu();
            return 0;
        }
        if (PtInRect(&layout.paletteProfileComboRect, cursor) != 0) {
            OpenPaletteProfileMenu();
            return 0;
        }
        if (PtInRect(&layout.paletteProfileApplyToggleRect, cursor) != 0) {
            draftApplyPaletteProfileToExistingStrokes_ = !draftApplyPaletteProfileToExistingStrokes_;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&layout.colorPreviewRect, cursor) != 0) {
            BeginHexColorEdit();
            return 0;
        }

        for (std::size_t index = 0; index < layout.defaultColorSwatches.size(); ++index) {
            if (PtInRect(&layout.defaultColorSwatches[index], cursor) != 0) {
                draftBackgroundColor_ = kDefaultPageColors[index];
                UpdateEditValues();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
        }

        for (std::size_t index = 0; index < layout.previousColorSwatches.size(); ++index) {
            if (PtInRect(&layout.previousColorSwatches[index], cursor) != 0 && index < app_.newPageSettings_.recentColors.size()) {
                draftBackgroundColor_ = app_.newPageSettings_.recentColors[index];
                UpdateEditValues();
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
            const NewPageSettingsLayout layout = BuildLayout(clientRect, app_.newPageSettings_.recentColors.size(), scrollOffset_, CurrentDpi());
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
        editingHexColor_ = false;
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace cnote
