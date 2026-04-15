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
#include <windowsx.h>

#include "DpiUtil.h"
#include "resource.h"
#include "ShortcutConfig.h"
#include "ui/UiUtil.h"

namespace cnote {

namespace ui = cnote::ui;

namespace {

constexpr wchar_t kSettingsWindowClassName[] = L"CnoteSettingsWindow";
constexpr int kWindowWidth = 920;
constexpr int kWindowHeight = 720;
constexpr DWORD kSettingsWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
constexpr DWORD kSettingsWindowExStyle = WS_EX_DLGMODALFRAME;
constexpr UINT kMsgShortcutListInvalidated = WM_APP + 41;

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
using ui::kButtonFill;
using ui::kButtonFillActive;
using ui::kButtonFillHover;
using ui::kPanelBorder;
using ui::kPanelFill;
using ui::kScrollbarGap;
using ui::kScrollbarThumbActiveFill;
using ui::kScrollbarThumbFill;
using ui::kScrollbarTrackFill;
using ui::kScrollbarWidth;
using ui::kSelectionBorder;
using ui::kTextMuted;
using ui::kTextPrimary;
using ui::kWindowBackground;
using ui::MakeRect;
using ui::RgbToHsl;
using ui::SetControlText;
using ui::SliderThumbCenterX;
using ui::SliderTrackRect;
using ui::StrokeRoundedRect;

constexpr int kIdCategoryPalette = 20;
constexpr int kIdCategoryDrawing = 21;
constexpr int kIdCategoryShortcuts = 22;

constexpr int kIdPaletteList = 100;
constexpr int kIdAddColor = 101;
constexpr int kIdRemoveColor = 102;
constexpr int kIdHexColorEdit = 103;
constexpr int kIdSavedPaletteName = 104;
constexpr int kIdSavePalette = 105;
constexpr int kIdApplyPalette = 106;
constexpr int kIdDeletePalette = 107;
constexpr int kIdSavedPaletteList = 108;
constexpr int kIdShortcutList = 120;
constexpr int kIdChangeShortcut = 121;
constexpr int kIdClearShortcut = 122;
constexpr int kIdHueSlider = 200;
constexpr int kIdSaturationSlider = 201;
constexpr int kIdLightnessSlider = 202;
constexpr int kIdStabilizationSlider = 205;
constexpr int kIdSpacingSlider = 206;
constexpr int kIdStrokeSmoothingSlider = 207;
constexpr int kIdAutoSaveSlider = 208;
constexpr int kIdAutoHideToolbarSwitch = 209;
constexpr int kIdZoomPresetBase = 300;

struct SettingsWindowLayout {
    RECT tabRail {};
    RECT tabRailLabelRect {};
    std::array<RECT, 3> categoryButtons {};
    RECT contentPanel {};

    RECT paletteSection {};
    RECT paletteListRect {};
    RECT addColorButton {};
    RECT removeColorButton {};
    RECT savedPaletteSection {};
    RECT savedPaletteLabelRect {};
    RECT savedPaletteNameRect {};
    RECT savePaletteButton {};
    RECT applyPaletteButton {};
    RECT deletePaletteButton {};
    RECT savedPaletteListRect {};
    RECT savedPaletteHintRect {};
    RECT colorSection {};
    RECT colorPreviewRect {};
    RECT hexColorEditRect {};
    RECT redLabelRect {};
    RECT redSliderRect {};
    RECT greenLabelRect {};
    RECT greenSliderRect {};
    RECT blueLabelRect {};
    RECT blueSliderRect {};
    RECT drawingSection {};
    RECT drawingMetaSection {};
    RECT stabilizationLabelRect {};
    RECT stabilizationSliderRect {};
    RECT spacingLabelRect {};
    RECT spacingSliderRect {};
    RECT strokeSmoothingLabelRect {};
    RECT strokeSmoothingSliderRect {};
    RECT autoSaveLabelRect {};
    RECT autoSaveSliderRect {};
    RECT autoHideToolbarSwitchRect {};
    std::array<RECT, App::kZoomPresetCount> zoomPresetLabelRects {};
    std::array<RECT, App::kZoomPresetCount> zoomPresetEditRects {};
    RECT zoomPresetHintRect {};
    RECT shortcutListSection {};
    RECT shortcutListRect {};
    RECT shortcutScrollbarTrackRect {};
    RECT shortcutActionsSection {};
    RECT changeShortcutButton {};
    RECT clearShortcutButton {};
    RECT shortcutHintRect {};
};

std::wstring FormatPaletteEntry(std::size_t index, std::uint32_t color) {
    wchar_t buffer[32] = {};
    std::swprintf(buffer, std::size(buffer), L"%zu. #%06X", index + 1U, color & 0xFFFFFFU);
    return buffer;
}

LRESULT CALLBACK ShortcutListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEWHEEL) {
        RECT listRect {};
        GetClientRect(window, &listRect);
        const int itemHeight = std::max(1, static_cast<int>(SendMessageW(window, LB_GETITEMHEIGHT, 0, 0)));
        const int itemCount = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
        const int visibleItems = std::max(1, static_cast<int>((listRect.bottom - listRect.top) / itemHeight));
        const int maxTopIndex = std::max(0, itemCount - visibleItems);
        const int topIndex = static_cast<int>(SendMessageW(window, LB_GETTOPINDEX, 0, 0));
        UINT wheelLines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
        int step = wheelLines == WHEEL_PAGESCROLL ? std::max(1, visibleItems - 1) : static_cast<int>(wheelLines);
        if (step <= 0) {
            step = 3;
        }
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const int newTopIndex = std::clamp(topIndex - ((delta / WHEEL_DELTA) * step), 0, maxTopIndex);
        if (newTopIndex != topIndex) {
            SendMessageW(window, LB_SETTOPINDEX, static_cast<WPARAM>(newTopIndex), 0);
            PostMessageW(GetParent(window), kMsgShortcutListInvalidated, 0, 0);
        }
        return 0;
    }

    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    switch (message) {
    case WM_VSCROLL:
    case WM_KEYDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
        PostMessageW(GetParent(window), kMsgShortcutListInvalidated, 0, 0);
        break;
    default:
        break;
    }
    return result;
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

std::wstring FormatZoomPresetValue(float zoom) {
    if (zoom <= 0.0f) {
        return L"";
    }

    wchar_t buffer[32] = {};
    const float percent = zoom * 100.0f;
    if (std::abs(percent - std::round(percent)) < 0.05f) {
        std::swprintf(buffer, std::size(buffer), L"%.0f", percent);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%.1f", percent);
    }
    std::wstring value = buffer;
    if (value.find(L'.') != std::wstring::npos) {
        while (!value.empty() && value.back() == L'0') {
            value.pop_back();
        }
        if (!value.empty() && value.back() == L'.') {
            value.pop_back();
        }
    }
    return value;
}

std::wstring TrimWide(std::wstring_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::iswspace(value[start]) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::iswspace(value[end - 1U]) != 0) {
        --end;
    }

    return std::wstring(value.substr(start, end - start));
}

RECT UnionRectCopy(const RECT& a, const RECT& b) {
    RECT out {};
    UnionRect(&out, &a, &b);
    return out;
}

SettingsWindowLayout BuildSettingsWindowLayout(RECT clientRect, UINT dpi) {
    SettingsWindowLayout layout {};
    const auto px = [dpi](int value) {
        return ScaleForDpi(value, dpi);
    };
    const int outerMargin = px(16);
    const int contentTop = px(52);
    const int contentBottom = std::max(contentTop + px(300), static_cast<int>(clientRect.bottom) - outerMargin);
    const int railWidth = std::clamp((static_cast<int>(clientRect.right) - (outerMargin * 3)) / 5, px(152), px(184));
    const int railGap = px(16);
    const int tabPadding = px(10);
    const int tabButtonHeight = px(42);
    const int tabButtonGap = px(8);

    layout.tabRail = MakeRect(outerMargin, contentTop, outerMargin + railWidth, contentBottom);
    layout.tabRailLabelRect = MakeRect(
        layout.tabRail.left + px(14),
        layout.tabRail.top + px(14),
        layout.tabRail.right - px(14),
        layout.tabRail.top + px(30));

    const int tabTop = layout.tabRailLabelRect.bottom + px(10);
    for (std::size_t index = 0; index < layout.categoryButtons.size(); ++index) {
        const int top = tabTop + static_cast<int>(index) * (tabButtonHeight + tabButtonGap);
        layout.categoryButtons[index] = MakeRect(
            layout.tabRail.left + tabPadding,
            top,
            layout.tabRail.right - tabPadding,
            top + tabButtonHeight);
    }

    layout.contentPanel = MakeRect(
        layout.tabRail.right + railGap,
        contentTop,
        static_cast<int>(clientRect.right) - outerMargin,
        contentBottom);

    const int sectionPadding = px(14);
    const int sectionGap = px(16);
    const int contentPadding = px(18);
    const int buttonHeight = px(30);
    const int sectionHeaderContentTop = px(84);
    const int contentLeft = layout.contentPanel.left + contentPadding;
    const int contentTopInner = layout.contentPanel.top + contentPadding;
    const int contentRight = layout.contentPanel.right - contentPadding;
    const int contentBottomInner = layout.contentPanel.bottom - contentPadding;
    const int availableWidth = std::max(px(320), contentRight - contentLeft);

    const int paletteColumnWidth = (availableWidth - (sectionGap * 2)) / 3;
    layout.savedPaletteSection = MakeRect(contentLeft, contentTopInner, contentLeft + paletteColumnWidth, contentBottomInner);
    layout.paletteSection = MakeRect(layout.savedPaletteSection.right + sectionGap, contentTopInner, layout.savedPaletteSection.right + sectionGap + paletteColumnWidth, contentBottomInner);
    layout.colorSection = MakeRect(layout.paletteSection.right + sectionGap, contentTopInner, contentRight, contentBottomInner);

    const int paletteButtonGap = px(8);
    const int paletteRowTop = layout.paletteSection.bottom - sectionPadding - buttonHeight;
    const int paletteHalfWidth = (layout.paletteSection.right - layout.paletteSection.left - (sectionPadding * 2) - paletteButtonGap) / 2;
    layout.paletteListRect = MakeRect(
        layout.paletteSection.left + sectionPadding,
        layout.paletteSection.top + sectionHeaderContentTop,
        layout.paletteSection.right - sectionPadding,
        paletteRowTop - px(48));
    layout.addColorButton = MakeRect(
        layout.paletteSection.left + sectionPadding,
        paletteRowTop - buttonHeight - px(8),
        layout.paletteSection.left + sectionPadding + paletteHalfWidth,
        paletteRowTop - px(8));
    layout.removeColorButton = MakeRect(
        layout.addColorButton.right + paletteButtonGap,
        layout.addColorButton.top,
        layout.paletteSection.right - sectionPadding,
        layout.addColorButton.bottom);

    layout.savedPaletteLabelRect = MakeRect(
        layout.savedPaletteSection.left + sectionPadding,
        layout.savedPaletteSection.top + sectionHeaderContentTop,
        layout.savedPaletteSection.right - sectionPadding,
        layout.savedPaletteSection.top + sectionHeaderContentTop + px(20));
    layout.savedPaletteNameRect = MakeRect(
        layout.savedPaletteSection.left + sectionPadding,
        layout.savedPaletteLabelRect.bottom + px(6),
        layout.savedPaletteSection.right - sectionPadding,
        layout.savedPaletteLabelRect.bottom + px(34));
    const int savedButtonGap = px(8);
    const int savedButtonWidth = ((layout.savedPaletteSection.right - layout.savedPaletteSection.left) - (sectionPadding * 2) - (savedButtonGap * 2)) / 3;
    const int savedButtonTop = layout.savedPaletteNameRect.bottom + px(10);
    layout.savePaletteButton = MakeRect(
        layout.savedPaletteSection.left + sectionPadding,
        savedButtonTop,
        layout.savedPaletteSection.left + sectionPadding + savedButtonWidth,
        savedButtonTop + buttonHeight);
    layout.applyPaletteButton = MakeRect(
        layout.savePaletteButton.right + savedButtonGap,
        savedButtonTop,
        layout.savePaletteButton.right + savedButtonGap + savedButtonWidth,
        savedButtonTop + buttonHeight);
    layout.deletePaletteButton = MakeRect(
        layout.applyPaletteButton.right + savedButtonGap,
        savedButtonTop,
        layout.savedPaletteSection.right - sectionPadding,
        savedButtonTop + buttonHeight);
    layout.savedPaletteHintRect = MakeRect(
        layout.savedPaletteSection.left + sectionPadding,
        layout.savedPaletteSection.bottom - sectionPadding - px(60),
        layout.savedPaletteSection.right - sectionPadding,
        layout.savedPaletteSection.bottom - sectionPadding);
    const int sharedPaletteListBottom = std::min(layout.addColorButton.top, layout.savedPaletteHintRect.top) - px(8);
    layout.paletteListRect.bottom = sharedPaletteListBottom;
    layout.savedPaletteListRect = MakeRect(
        layout.savedPaletteSection.left + sectionPadding,
        layout.savePaletteButton.bottom + px(12),
        layout.savedPaletteSection.right - sectionPadding,
        sharedPaletteListBottom);

    layout.colorPreviewRect = MakeRect(
        layout.colorSection.left + sectionPadding,
        layout.colorSection.top + sectionHeaderContentTop,
        layout.colorSection.right - sectionPadding,
        layout.colorSection.top + sectionHeaderContentTop + px(128));
    const int previewWidth = static_cast<int>(layout.colorPreviewRect.right - layout.colorPreviewRect.left);
    const int hexEditWidth = std::max(px(72), std::min(px(124), previewWidth - px(24)));
    layout.hexColorEditRect = MakeRect(
        layout.colorPreviewRect.left + ((layout.colorPreviewRect.right - layout.colorPreviewRect.left - hexEditWidth) / 2),
        layout.colorPreviewRect.top + px(10),
        layout.colorPreviewRect.left + ((layout.colorPreviewRect.right - layout.colorPreviewRect.left - hexEditWidth) / 2) + hexEditWidth,
        layout.colorPreviewRect.top + px(34));
    const int colorSliderLeft = layout.colorSection.left + sectionPadding;
    const int colorSliderRight = layout.colorSection.right - sectionPadding;
    layout.redLabelRect = MakeRect(colorSliderLeft, layout.colorPreviewRect.bottom + px(18), colorSliderRight, layout.colorPreviewRect.bottom + px(36));
    layout.redSliderRect = MakeRect(colorSliderLeft, layout.redLabelRect.bottom + px(4), colorSliderRight, layout.redLabelRect.bottom + px(32));
    layout.greenLabelRect = MakeRect(colorSliderLeft, layout.redSliderRect.bottom + px(16), colorSliderRight, layout.redSliderRect.bottom + px(34));
    layout.greenSliderRect = MakeRect(colorSliderLeft, layout.greenLabelRect.bottom + px(4), colorSliderRight, layout.greenLabelRect.bottom + px(32));
    layout.blueLabelRect = MakeRect(colorSliderLeft, layout.greenSliderRect.bottom + px(16), colorSliderRight, layout.greenSliderRect.bottom + px(34));
    layout.blueSliderRect = MakeRect(colorSliderLeft, layout.blueLabelRect.bottom + px(4), colorSliderRight, layout.blueLabelRect.bottom + px(32));

    const int drawingLeftWidth = (availableWidth * 56) / 100;
    layout.drawingSection = MakeRect(contentLeft, contentTopInner, contentLeft + drawingLeftWidth, contentBottomInner);
    layout.drawingMetaSection = MakeRect(layout.drawingSection.right + sectionGap, contentTopInner, contentRight, contentBottomInner);
    const int drawingSliderLeft = layout.drawingSection.left + sectionPadding;
    const int drawingSliderRight = layout.drawingSection.right - sectionPadding;
    layout.stabilizationLabelRect = MakeRect(drawingSliderLeft, layout.drawingSection.top + sectionHeaderContentTop, drawingSliderRight, layout.drawingSection.top + sectionHeaderContentTop + px(18));
    layout.stabilizationSliderRect = MakeRect(drawingSliderLeft, layout.stabilizationLabelRect.bottom + px(4), drawingSliderRight, layout.stabilizationLabelRect.bottom + px(32));
    layout.spacingLabelRect = MakeRect(drawingSliderLeft, layout.stabilizationSliderRect.bottom + px(16), drawingSliderRight, layout.stabilizationSliderRect.bottom + px(34));
    layout.spacingSliderRect = MakeRect(drawingSliderLeft, layout.spacingLabelRect.bottom + px(4), drawingSliderRight, layout.spacingLabelRect.bottom + px(32));
    layout.strokeSmoothingLabelRect = MakeRect(drawingSliderLeft, layout.spacingSliderRect.bottom + px(16), drawingSliderRight, layout.spacingSliderRect.bottom + px(34));
    layout.strokeSmoothingSliderRect = MakeRect(drawingSliderLeft, layout.strokeSmoothingLabelRect.bottom + px(4), drawingSliderRight, layout.strokeSmoothingLabelRect.bottom + px(32));

    const int metaLeft = layout.drawingMetaSection.left + sectionPadding;
    const int metaRight = layout.drawingMetaSection.right - sectionPadding;
    layout.autoSaveLabelRect = MakeRect(metaLeft, layout.drawingMetaSection.top + sectionHeaderContentTop, metaRight, layout.drawingMetaSection.top + sectionHeaderContentTop + px(18));
    layout.autoSaveSliderRect = MakeRect(metaLeft, layout.autoSaveLabelRect.bottom + px(4), metaRight, layout.autoSaveLabelRect.bottom + px(32));
    layout.autoHideToolbarSwitchRect = MakeRect(metaLeft, layout.autoSaveSliderRect.bottom + px(16), metaRight, layout.autoSaveSliderRect.bottom + px(42));
    const int zoomLabelWidth = px(74);
    const int zoomEditWidth = px(90);
    const int zoomRowTop = layout.autoHideToolbarSwitchRect.bottom + px(20);
    const int zoomRowHeight = px(26);
    const int zoomRowGap = px(10);
    for (std::size_t index = 0; index < layout.zoomPresetLabelRects.size(); ++index) {
        const int top = zoomRowTop + static_cast<int>(index) * (zoomRowHeight + zoomRowGap);
        layout.zoomPresetLabelRects[index] = MakeRect(metaLeft, top, metaLeft + zoomLabelWidth, top + zoomRowHeight);
        layout.zoomPresetEditRects[index] = MakeRect(metaLeft + zoomLabelWidth + px(10), top - px(2), metaLeft + zoomLabelWidth + px(10) + zoomEditWidth, top + zoomRowHeight);
    }
    layout.zoomPresetHintRect = MakeRect(
        metaLeft,
        layout.zoomPresetEditRects.back().bottom + px(10),
        metaRight,
        layout.zoomPresetEditRects.back().bottom + px(58));

    const int shortcutLeftWidth = (availableWidth * 62) / 100;
    layout.shortcutListSection = MakeRect(contentLeft, contentTopInner, contentLeft + shortcutLeftWidth, contentBottomInner);
    layout.shortcutActionsSection = MakeRect(layout.shortcutListSection.right + sectionGap, contentTopInner, contentRight, contentBottomInner);
    const int shortcutScrollbarReserve = px(kScrollbarGap + kScrollbarWidth);
    layout.shortcutListRect = MakeRect(
        layout.shortcutListSection.left + sectionPadding,
        layout.shortcutListSection.top + sectionHeaderContentTop,
        layout.shortcutListSection.right - sectionPadding - shortcutScrollbarReserve,
        layout.shortcutListSection.bottom - sectionPadding);
    layout.shortcutScrollbarTrackRect = MakeRect(
        layout.shortcutListRect.right + px(kScrollbarGap),
        layout.shortcutListRect.top,
        layout.shortcutListRect.right + px(kScrollbarGap + kScrollbarWidth),
        layout.shortcutListRect.bottom);
    layout.changeShortcutButton = MakeRect(
        layout.shortcutActionsSection.left + sectionPadding,
        layout.shortcutActionsSection.top + px(96),
        layout.shortcutActionsSection.right - sectionPadding,
        layout.shortcutActionsSection.top + px(126));
    layout.clearShortcutButton = MakeRect(
        layout.shortcutActionsSection.left + sectionPadding,
        layout.changeShortcutButton.bottom + px(10),
        layout.shortcutActionsSection.right - sectionPadding,
        layout.changeShortcutButton.bottom + px(40));
    layout.shortcutHintRect = MakeRect(
        layout.shortcutActionsSection.left + sectionPadding,
        layout.clearShortcutButton.bottom + px(14),
        layout.shortcutActionsSection.right - sectionPadding,
        layout.clearShortcutButton.bottom + px(96));

    return layout;
}

bool ComputeListScrollbarThumbRect(HWND listbox, const RECT& listRect, const RECT& trackRect, int itemHeight, UINT dpi, RECT& thumbRect) {
    if (listbox == nullptr || itemHeight <= 0) {
        return false;
    }

    const int itemCount = static_cast<int>(SendMessageW(listbox, LB_GETCOUNT, 0, 0));
    if (itemCount <= 0) {
        return false;
    }

    const int viewportHeight = std::max(1, static_cast<int>(listRect.bottom - listRect.top));
    const int visibleItems = std::max(1, viewportHeight / itemHeight);
    if (itemCount <= visibleItems) {
        return false;
    }

    const int topIndex = static_cast<int>(SendMessageW(listbox, LB_GETTOPINDEX, 0, 0));
    const int trackHeight = std::max(1, static_cast<int>(trackRect.bottom - trackRect.top));
    const int thumbHeight = std::min(trackHeight, std::max(ScaleForDpi(ui::kScrollbarMinThumbHeight, dpi), (trackHeight * visibleItems) / itemCount));
    const int maxTopIndex = std::max(1, itemCount - visibleItems);
    const int travel = std::max(1, trackHeight - thumbHeight);
    const int thumbTop = trackRect.top + ((std::clamp(topIndex, 0, maxTopIndex) * travel) / maxTopIndex);
    thumbRect = MakeRect(trackRect.left, thumbTop, trackRect.right, thumbTop + thumbHeight);
    return true;
}

RECT ControlRectInParent(HWND control, HWND parent) {
    RECT rect {};
    if (control == nullptr || parent == nullptr) {
        return rect;
    }

    GetWindowRect(control, &rect);
    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<LPPOINT>(&rect), 2);
    return rect;
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
    RefreshSavedPaletteList();
    RefreshSliderValues();
    RefreshZoomPresetFields();
    RefreshShortcutList();
    RefreshShortcutButtons();
    RefreshSavedPaletteButtons();
    RefreshCategoryButtons();
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

    currentDpi_ = owner != nullptr ? WindowDpi(owner) : SystemDpi();
    RECT windowRect { 0, 0, Scale(kWindowWidth), Scale(kWindowHeight) };
    AdjustWindowRectForDpi(&windowRect, kSettingsWindowStyle, FALSE, kSettingsWindowExStyle, currentDpi_);
    window_ = CreateWindowExW(
        kSettingsWindowExStyle,
        kSettingsWindowClassName,
        L"Settings",
        kSettingsWindowStyle,
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

void SettingsWindow::CreateControls() {
    const UINT dpi = CurrentDpi();
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
    const auto createEdit = [&](int id, int x, int y, int width, int height) {
        return CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            x, y, width, height, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app_.instance_, nullptr);
    };

    categoryButtons_[0] = createButton(L"Palette", kIdCategoryPalette, 0, 0, ScaleForDpi(150, dpi), ScaleForDpi(42, dpi));
    categoryButtons_[1] = createButton(L"Drawing", kIdCategoryDrawing, 0, 0, ScaleForDpi(150, dpi), ScaleForDpi(42, dpi));
    categoryButtons_[2] = createButton(L"Shortcuts", kIdCategoryShortcuts, 0, 0, ScaleForDpi(150, dpi), ScaleForDpi(42, dpi));

    paletteList_ = CreateWindowExW(
        0,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        0,
        0,
        ScaleForDpi(260, dpi),
        ScaleForDpi(320, dpi),
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPaletteList)),
        app_.instance_,
        nullptr);
    addColorButton_ = createButton(L"Add Color", kIdAddColor, 0, 0, ScaleForDpi(120, dpi), ScaleForDpi(30, dpi));
    removeColorButton_ = createButton(L"Remove Color", kIdRemoveColor, 0, 0, ScaleForDpi(120, dpi), ScaleForDpi(30, dpi));
    hexColorEdit_ = createEdit(kIdHexColorEdit, 0, 0, ScaleForDpi(124, dpi), ScaleForDpi(24, dpi));
    SetWindowSubclass(hexColorEdit_, ui::EditReturnToParentSubclassProc, 0, 0);
    savedPaletteNameEdit_ = createEdit(kIdSavedPaletteName, 0, 0, ScaleForDpi(220, dpi), ScaleForDpi(24, dpi));
    savePaletteButton_ = createButton(L"Save", kIdSavePalette, 0, 0, ScaleForDpi(120, dpi), ScaleForDpi(30, dpi));
    applyPaletteButton_ = createButton(L"Apply", kIdApplyPalette, 0, 0, ScaleForDpi(120, dpi), ScaleForDpi(30, dpi));
    deletePaletteButton_ = createButton(L"Delete", kIdDeletePalette, 0, 0, ScaleForDpi(120, dpi), ScaleForDpi(30, dpi));
    savedPaletteList_ = CreateWindowExW(
        0,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        0,
        0,
        ScaleForDpi(240, dpi),
        ScaleForDpi(240, dpi),
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSavedPaletteList)),
        app_.instance_,
        nullptr);
    savedPaletteHintLabel_ = createLabel(L"Save the current swatches under a name, then select and apply them later.", 0, 0, ScaleForDpi(280, dpi), ScaleForDpi(60, dpi));

    redLabel_ = createLabel(L"Hue", 0, 0, ScaleForDpi(220, dpi), ScaleForDpi(20, dpi));
    redSlider_ = createSlider(kIdHueSlider, 0, 0, ScaleForDpi(320, dpi), ScaleForDpi(30, dpi));
    greenLabel_ = createLabel(L"Saturation", 0, 0, ScaleForDpi(220, dpi), ScaleForDpi(20, dpi));
    greenSlider_ = createSlider(kIdSaturationSlider, 0, 0, ScaleForDpi(320, dpi), ScaleForDpi(30, dpi));
    blueLabel_ = createLabel(L"Lightness", 0, 0, ScaleForDpi(220, dpi), ScaleForDpi(20, dpi));
    blueSlider_ = createSlider(kIdLightnessSlider, 0, 0, ScaleForDpi(320, dpi), ScaleForDpi(30, dpi));
    stabilizationLabel_ = createLabel(L"Stabilization", 0, 0, ScaleForDpi(260, dpi), ScaleForDpi(20, dpi));
    stabilizationSlider_ = createSlider(kIdStabilizationSlider, 0, 0, ScaleForDpi(320, dpi), ScaleForDpi(30, dpi));
    spacingLabel_ = createLabel(L"Point Spacing", 0, 0, ScaleForDpi(260, dpi), ScaleForDpi(20, dpi));
    spacingSlider_ = createSlider(kIdSpacingSlider, 0, 0, ScaleForDpi(320, dpi), ScaleForDpi(30, dpi));
    strokeSmoothingLabel_ = createLabel(L"Stroke Smoothing", 0, 0, ScaleForDpi(260, dpi), ScaleForDpi(20, dpi));
    strokeSmoothingSlider_ = createSlider(kIdStrokeSmoothingSlider, 0, 0, ScaleForDpi(320, dpi), ScaleForDpi(30, dpi));
    autoSaveLabel_ = createLabel(L"Auto Save", 0, 0, ScaleForDpi(260, dpi), ScaleForDpi(20, dpi));
    autoSaveSlider_ = createSlider(kIdAutoSaveSlider, 0, 0, ScaleForDpi(320, dpi), ScaleForDpi(30, dpi));
    autoHideToolbarSwitch_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"Auto-hide toolbar",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0,
        0,
        ScaleForDpi(260, dpi),
        ScaleForDpi(24, dpi),
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoHideToolbarSwitch)),
        app_.instance_,
        nullptr);
    for (std::size_t index = 0; index < zoomPresetLabels_.size(); ++index) {
        const std::wstring label = L"Preset " + std::to_wstring(index + 1U) + L" (%)";
        zoomPresetLabels_[index] = createLabel(label.c_str(), 0, 0, ScaleForDpi(80, dpi), ScaleForDpi(20, dpi));
        zoomPresetEdits_[index] = createEdit(kIdZoomPresetBase + static_cast<int>(index), 0, 0, ScaleForDpi(96, dpi), ScaleForDpi(24, dpi));
    }
    zoomPresetHintLabel_ = createLabel(L"Leave blank to keep a preset unassigned.", 0, 0, ScaleForDpi(280, dpi), ScaleForDpi(56, dpi));

    shortcutList_ = CreateWindowExW(
        0,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        0,
        0,
        ScaleForDpi(300, dpi),
        ScaleForDpi(420, dpi),
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdShortcutList)),
        app_.instance_,
        nullptr);
    SetWindowSubclass(shortcutList_, ShortcutListSubclassProc, 0, 0);
    ShowScrollBar(shortcutList_, SB_VERT, FALSE);
    changeShortcutButton_ = createButton(L"Change Shortcut", kIdChangeShortcut, 0, 0, ScaleForDpi(148, dpi), ScaleForDpi(30, dpi));
    clearShortcutButton_ = createButton(L"Clear Shortcut", kIdClearShortcut, 0, 0, ScaleForDpi(148, dpi), ScaleForDpi(30, dpi));
    shortcutHintLabel_ = createLabel(L"Select a command to edit its binding.", 0, 0, ScaleForDpi(180, dpi), ScaleForDpi(84, dpi));

    SendMessageW(redSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 360));
    SendMessageW(greenSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(blueSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(stabilizationSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(5, 95));
    SendMessageW(spacingSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(5, 405));
    SendMessageW(strokeSmoothingSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(autoSaveSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 300));
    SendMessageW(autoSaveSlider_, TBM_SETTICFREQ, 15, 0);
    SendMessageW(paletteList_, LB_SETITEMHEIGHT, 0, ScaleForDpi(46, dpi));
    SendMessageW(savedPaletteList_, LB_SETITEMHEIGHT, 0, ScaleForDpi(58, dpi));
    SendMessageW(shortcutList_, LB_SETITEMHEIGHT, 0, ScaleForDpi(26, dpi));
    SendMessageW(hexColorEdit_, EM_LIMITTEXT, 10, 0);
    SendMessageW(savedPaletteNameEdit_, EM_LIMITTEXT, 48, 0);
    LayoutControls();
}

void SettingsWindow::ApplyFonts() {
    RebuildDpiResources(CurrentDpi());
}

void SettingsWindow::RebuildDpiResources(UINT dpi) {
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

    font_ = CreateFontW(FontHeightForDpi(14, currentDpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    titleFont_ = CreateFontW(FontHeightForDpi(18, currentDpi_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    backgroundBrush_ = CreateSolidBrush(kWindowBackground);
    panelBrush_ = CreateSolidBrush(kPanelFill);
    std::vector<HWND> controls;
    controls.reserve(37);
    controls.insert(controls.end(), categoryButtons_.begin(), categoryButtons_.end());
    controls.push_back(paletteList_);
    controls.push_back(addColorButton_);
    controls.push_back(removeColorButton_);
    controls.push_back(hexColorEdit_);
    controls.push_back(savedPaletteNameEdit_);
    controls.push_back(savePaletteButton_);
    controls.push_back(applyPaletteButton_);
    controls.push_back(deletePaletteButton_);
    controls.push_back(savedPaletteList_);
    controls.push_back(savedPaletteHintLabel_);
    controls.push_back(redLabel_);
    controls.push_back(redSlider_);
    controls.push_back(greenLabel_);
    controls.push_back(greenSlider_);
    controls.push_back(blueLabel_);
    controls.push_back(blueSlider_);
    controls.push_back(stabilizationLabel_);
    controls.push_back(stabilizationSlider_);
    controls.push_back(spacingLabel_);
    controls.push_back(spacingSlider_);
    controls.push_back(strokeSmoothingLabel_);
    controls.push_back(strokeSmoothingSlider_);
    controls.push_back(autoSaveLabel_);
    controls.push_back(autoSaveSlider_);
    controls.push_back(autoHideToolbarSwitch_);
    controls.push_back(zoomPresetHintLabel_);
    controls.push_back(shortcutList_);
    controls.push_back(changeShortcutButton_);
    controls.push_back(clearShortcutButton_);
    controls.push_back(shortcutHintLabel_);
    for (HWND control : zoomPresetLabels_) {
        controls.push_back(control);
    }
    for (HWND control : zoomPresetEdits_) {
        controls.push_back(control);
    }

    for (HWND control : controls) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }
    if (paletteList_ != nullptr) {
        SendMessageW(paletteList_, LB_SETITEMHEIGHT, 0, ScaleForDpi(46, currentDpi_));
    }
    if (savedPaletteList_ != nullptr) {
        SendMessageW(savedPaletteList_, LB_SETITEMHEIGHT, 0, ScaleForDpi(58, currentDpi_));
    }
    if (shortcutList_ != nullptr) {
        SendMessageW(shortcutList_, LB_SETITEMHEIGHT, 0, ScaleForDpi(26, currentDpi_));
    }
}

void SettingsWindow::LayoutControls() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());

    HDWP deferred = BeginDeferWindowPos(41);
    for (std::size_t index = 0; index < categoryButtons_.size(); ++index) {
        DeferMoveControl(deferred, categoryButtons_[index], layout.categoryButtons[index]);
    }
    DeferMoveControl(deferred, paletteList_, layout.paletteListRect);
    DeferMoveControl(deferred, addColorButton_, layout.addColorButton);
    DeferMoveControl(deferred, removeColorButton_, layout.removeColorButton);
    DeferMoveControl(deferred, hexColorEdit_, layout.hexColorEditRect);
    DeferMoveControl(deferred, savedPaletteNameEdit_, layout.savedPaletteNameRect);
    DeferMoveControl(deferred, savePaletteButton_, layout.savePaletteButton);
    DeferMoveControl(deferred, applyPaletteButton_, layout.applyPaletteButton);
    DeferMoveControl(deferred, deletePaletteButton_, layout.deletePaletteButton);
    DeferMoveControl(deferred, savedPaletteList_, layout.savedPaletteListRect);
    DeferMoveControl(deferred, savedPaletteHintLabel_, layout.savedPaletteHintRect);
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
    DeferMoveControl(deferred, autoHideToolbarSwitch_, layout.autoHideToolbarSwitchRect);
    for (std::size_t index = 0; index < zoomPresetLabels_.size(); ++index) {
        DeferMoveControl(deferred, zoomPresetLabels_[index], layout.zoomPresetLabelRects[index]);
        DeferMoveControl(deferred, zoomPresetEdits_[index], layout.zoomPresetEditRects[index]);
    }
    DeferMoveControl(deferred, zoomPresetHintLabel_, layout.zoomPresetHintRect);
    DeferMoveControl(deferred, shortcutList_, layout.shortcutListRect);
    DeferMoveControl(deferred, changeShortcutButton_, layout.changeShortcutButton);
    DeferMoveControl(deferred, clearShortcutButton_, layout.clearShortcutButton);
    DeferMoveControl(deferred, shortcutHintLabel_, layout.shortcutHintRect);
    if (deferred != nullptr) {
        EndDeferWindowPos(deferred);
    }

    RefreshVisibleControls();
}

UINT SettingsWindow::CurrentDpi() const {
    return currentDpi_ == 0 ? USER_DEFAULT_SCREEN_DPI : currentDpi_;
}

int SettingsWindow::Scale(int value) const {
    return ScaleForDpi(value, CurrentDpi());
}

void SettingsWindow::SetActiveCategory(Category category) {
    if (activeCategory_ == category) {
        return;
    }

    SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    if (capturingShortcutAction_.has_value()) {
        capturingShortcutAction_.reset();
        RefreshShortcutButtons();
    }
    if (editingHexColor_) {
        CommitHexColorEdit(true);
    }
    draggingSlider_.reset();
    activeCategory_ = category;
    LayoutControls();
    RefreshCategoryButtons();
    SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_NOERASE);
}

void SettingsWindow::RefreshVisibleControls() {
    const auto setVisible = [](HWND control, bool visible) {
        if (control != nullptr) {
            SetWindowPos(
                control,
                nullptr,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        }
    };

    for (HWND button : categoryButtons_) {
        setVisible(button, true);
    }

    const bool showPalette = activeCategory_ == Category::Palette;
    const bool showDrawing = activeCategory_ == Category::Drawing;
    const bool showShortcuts = activeCategory_ == Category::Shortcuts;

    setVisible(paletteList_, showPalette);
    setVisible(addColorButton_, showPalette);
    setVisible(removeColorButton_, showPalette);
    setVisible(hexColorEdit_, showPalette && editingHexColor_);
    setVisible(savedPaletteNameEdit_, showPalette);
    setVisible(savePaletteButton_, showPalette);
    setVisible(applyPaletteButton_, showPalette);
    setVisible(deletePaletteButton_, showPalette);
    setVisible(savedPaletteList_, showPalette);
    setVisible(savedPaletteHintLabel_, false);

    setVisible(redLabel_, showPalette);
    setVisible(redSlider_, false);
    setVisible(greenLabel_, showPalette);
    setVisible(greenSlider_, false);
    setVisible(blueLabel_, showPalette);
    setVisible(blueSlider_, false);

    setVisible(stabilizationLabel_, showDrawing);
    setVisible(stabilizationSlider_, false);
    setVisible(spacingLabel_, showDrawing);
    setVisible(spacingSlider_, false);
    setVisible(strokeSmoothingLabel_, showDrawing);
    setVisible(strokeSmoothingSlider_, false);
    setVisible(autoSaveLabel_, showDrawing);
    setVisible(autoSaveSlider_, false);
    setVisible(autoHideToolbarSwitch_, showDrawing);
    for (HWND control : zoomPresetLabels_) {
        setVisible(control, showDrawing);
    }
    for (HWND control : zoomPresetEdits_) {
        setVisible(control, showDrawing);
    }
    setVisible(zoomPresetHintLabel_, showDrawing);

    setVisible(shortcutList_, showShortcuts);
    setVisible(changeShortcutButton_, showShortcuts);
    setVisible(clearShortcutButton_, showShortcuts);
    setVisible(shortcutHintLabel_, showShortcuts);
}

void SettingsWindow::RefreshCategoryButtons() {
    for (HWND button : categoryButtons_) {
        if (button != nullptr) {
            InvalidateRect(button, nullptr, FALSE);
        }
    }
}

void SettingsWindow::BeginHexColorEdit() {
    if (window_ == nullptr || hexColorEdit_ == nullptr || app_.palette_.empty()) {
        return;
    }

    editingHexColor_ = true;
    SetControlText(hexColorEdit_, FormatHexColor(app_.palette_[app_.settings_.paletteIndex]));
    RefreshVisibleControls();
    SetFocus(hexColorEdit_);
    SendMessageW(hexColorEdit_, EM_SETSEL, 0, -1);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());
    InvalidateWindowRegion(layout.colorPreviewRect);
}

void SettingsWindow::CommitHexColorEdit(bool applyChanges) {
    if (hexColorEdit_ == nullptr || !editingHexColor_) {
        return;
    }

    if (applyChanges && !app_.palette_.empty()) {
        wchar_t buffer[32] = {};
        GetWindowTextW(hexColorEdit_, buffer, static_cast<int>(std::size(buffer)));
        if (const auto parsed = ui::ParseHexColor(buffer); parsed.has_value()) {
            app_.palette_[app_.settings_.paletteIndex] = *parsed;
            app_.SaveAppSettings();
            RefreshPaletteList();
            RefreshSliderValues();
            InvalidateRect(app_.window_, nullptr, FALSE);
        }
    }

    editingHexColor_ = false;
    ShowWindow(hexColorEdit_, SW_HIDE);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());
    InvalidateWindowRegion(layout.colorPreviewRect);
    InvalidateSliderPresentation(SliderKind::Hue);
    InvalidateSliderPresentation(SliderKind::Saturation);
    InvalidateSliderPresentation(SliderKind::Lightness);
}

void SettingsWindow::InvalidateWindowRegion(const RECT& rect) {
    if (window_ == nullptr) {
        return;
    }

    RECT dirtyRect = rect;
    InflateRect(&dirtyRect, Scale(8), Scale(8));
    RedrawWindow(window_, &dirtyRect, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

void SettingsWindow::InvalidateSliderPresentation(SliderKind kind) {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());

    RECT dirtyRect {};
    switch (kind) {
    case SliderKind::Hue:
    case SliderKind::Saturation:
    case SliderKind::Lightness:
        dirtyRect = UnionRectCopy(
            layout.colorPreviewRect,
            UnionRectCopy(
                UnionRectCopy(layout.redLabelRect, layout.redSliderRect),
                UnionRectCopy(layout.greenLabelRect, layout.greenSliderRect)));
        dirtyRect = UnionRectCopy(dirtyRect, UnionRectCopy(layout.blueLabelRect, layout.blueSliderRect));
        break;
    case SliderKind::Stabilization:
        dirtyRect = UnionRectCopy(layout.stabilizationLabelRect, layout.stabilizationSliderRect);
        break;
    case SliderKind::Spacing:
        dirtyRect = UnionRectCopy(layout.spacingLabelRect, layout.spacingSliderRect);
        break;
    case SliderKind::StrokeSmoothing:
        dirtyRect = UnionRectCopy(layout.strokeSmoothingLabelRect, layout.strokeSmoothingSliderRect);
        break;
    case SliderKind::AutoSave:
        dirtyRect = UnionRectCopy(layout.autoSaveLabelRect, layout.autoSaveSliderRect);
        break;
    default:
        return;
    }

    InvalidateWindowRegion(dirtyRect);
}

void SettingsWindow::RefreshPaletteList() {
    if (paletteList_ == nullptr) {
        return;
    }

    const int selection = static_cast<int>(std::min(app_.settings_.paletteIndex, app_.palette_.empty() ? 0U : app_.palette_.size() - 1U));
    SendMessageW(paletteList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(paletteList_, LB_RESETCONTENT, 0, 0);
    for (std::size_t index = 0; index < app_.palette_.size(); ++index) {
        const std::wstring entry = FormatPaletteEntry(index, app_.palette_[index]);
        SendMessageW(paletteList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.c_str()));
    }
    if (!app_.palette_.empty()) {
        SendMessageW(paletteList_, LB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    }
    SendMessageW(paletteList_, WM_SETREDRAW, TRUE, 0);
    EnableWindow(removeColorButton_, app_.palette_.size() > 1U);
    InvalidateRect(paletteList_, nullptr, FALSE);
}

void SettingsWindow::RefreshSavedPaletteList() {
    if (savedPaletteList_ == nullptr) {
        return;
    }

    const int previousSelection = static_cast<int>(SendMessageW(savedPaletteList_, LB_GETCURSEL, 0, 0));
    SendMessageW(savedPaletteList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(savedPaletteList_, LB_RESETCONTENT, 0, 0);
    for (const App::NamedPalette& palette : app_.savedPalettes_) {
        SendMessageW(savedPaletteList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(palette.name.c_str()));
    }

    if (!app_.savedPalettes_.empty()) {
        const int clampedSelection = previousSelection >= 0
            ? std::clamp(previousSelection, 0, static_cast<int>(app_.savedPalettes_.size()) - 1)
            : 0;
        SendMessageW(savedPaletteList_, LB_SETCURSEL, static_cast<WPARAM>(clampedSelection), 0);
    }

    SendMessageW(savedPaletteList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(savedPaletteList_, nullptr, FALSE);
}

void SettingsWindow::RefreshSliderValues() {
    if (autoHideToolbarSwitch_ != nullptr) {
        InvalidateRect(autoHideToolbarSwitch_, nullptr, FALSE);
    }

    if (app_.palette_.empty()) {
        return;
    }

    const std::uint32_t color = app_.palette_[app_.settings_.paletteIndex];
    const HslColor hsl = RgbToHsl(color);
    const int hue = static_cast<int>(std::lround(hsl.hue));
    const int saturation = static_cast<int>(std::lround(hsl.saturation * 100.0f));
    const int lightness = static_cast<int>(std::lround(hsl.lightness * 100.0f));
    SendMessageW(redSlider_, TBM_SETPOS, TRUE, hue);
    SendMessageW(greenSlider_, TBM_SETPOS, TRUE, saturation);
    SendMessageW(blueSlider_, TBM_SETPOS, TRUE, lightness);
    SendMessageW(stabilizationSlider_, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(app_.settings_.inputSmoothing * 100.0f)));
    SendMessageW(spacingSlider_, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(app_.settings_.inputSpacing * 100.0f)));
    SendMessageW(strokeSmoothingSlider_, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(app_.settings_.strokeSmoothing * 100.0f)));
    SendMessageW(autoSaveSlider_, TBM_SETPOS, TRUE, static_cast<LPARAM>(app_.settings_.autoSaveIntervalSeconds));

    SetControlText(redLabel_, FormatHueLabel(hue));
    SetControlText(greenLabel_, FormatPercentLabel(L"Saturation", saturation));
    SetControlText(blueLabel_, FormatPercentLabel(L"Lightness", lightness));
    SetControlText(stabilizationLabel_, FormatFloatSliderLabel(L"Stabilization", app_.settings_.inputSmoothing, 100));
    SetControlText(spacingLabel_, FormatSpacingLabel(app_.settings_.inputSpacing));
    SetControlText(strokeSmoothingLabel_, FormatFloatSliderLabel(L"Stroke Smoothing", app_.settings_.strokeSmoothing, 100));
    SetControlText(autoSaveLabel_, FormatAutoSaveLabel(app_.settings_.autoSaveIntervalSeconds));
}

void SettingsWindow::RefreshZoomPresetFields() {
    for (std::size_t index = 0; index < zoomPresetEdits_.size(); ++index) {
        SetControlText(zoomPresetEdits_[index], FormatZoomPresetValue(app_.settings_.zoomPresets[index]));
    }
}

void SettingsWindow::RefreshShortcutList() {
    if (shortcutList_ == nullptr) {
        return;
    }

    int selection = static_cast<int>(SendMessageW(shortcutList_, LB_GETCURSEL, 0, 0));
    SendMessageW(shortcutList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(shortcutList_, LB_RESETCONTENT, 0, 0);
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

void SettingsWindow::RefreshSavedPaletteButtons() {
    if (savedPaletteNameEdit_ == nullptr) {
        return;
    }

    wchar_t nameBuffer[128] = {};
    GetWindowTextW(savedPaletteNameEdit_, nameBuffer, static_cast<int>(std::size(nameBuffer)));
    const bool hasName = !TrimWide(nameBuffer).empty();
    const bool hasSelection = SelectedSavedPaletteIndex().has_value();
    EnableWindow(savePaletteButton_, hasName);
    EnableWindow(applyPaletteButton_, hasSelection);
    EnableWindow(deletePaletteButton_, hasSelection);
}

void SettingsWindow::HandleCommand(WPARAM wParam, LPARAM lParam) {
    const int controlId = LOWORD(wParam);
    const int notification = HIWORD(wParam);
    switch (controlId) {
    case kIdCategoryPalette:
        if (notification == BN_CLICKED) {
            SetActiveCategory(Category::Palette);
        }
        break;
    case kIdCategoryDrawing:
        if (notification == BN_CLICKED) {
            SetActiveCategory(Category::Drawing);
        }
        break;
    case kIdCategoryShortcuts:
        if (notification == BN_CLICKED) {
            SetActiveCategory(Category::Shortcuts);
        }
        break;
    case kIdPaletteList:
        if (notification == LBN_SELCHANGE || notification == LBN_DBLCLK) {
            const int selection = static_cast<int>(SendMessageW(paletteList_, LB_GETCURSEL, 0, 0));
            if (selection >= 0) {
                app_.settings_.paletteIndex = static_cast<std::size_t>(selection);
                app_.SaveAppSettings();
                RefreshPaletteList();
                RefreshSliderValues();
                InvalidateRect(window_, nullptr, FALSE);
            }
        }
        break;
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
    case kIdHexColorEdit:
        if (notification == EN_KILLFOCUS) {
            CommitHexColorEdit(true);
        }
        break;
    case kIdSavedPaletteName:
        if (notification == EN_CHANGE) {
            RefreshSavedPaletteButtons();
        }
        break;
    case kIdSavePalette:
        if (notification == BN_CLICKED) {
            wchar_t nameBuffer[128] = {};
            GetWindowTextW(savedPaletteNameEdit_, nameBuffer, static_cast<int>(std::size(nameBuffer)));
            const std::wstring trimmedName = TrimWide(nameBuffer);
            if (app_.SaveNamedPalette(trimmedName)) {
                if (const auto savedPalette = std::find_if(app_.savedPalettes_.begin(), app_.savedPalettes_.end(), [&](const App::NamedPalette& palette) {
                        return palette.name == trimmedName;
                    });
                    savedPalette != app_.savedPalettes_.end()) {
                    const std::size_t savedIndex = static_cast<std::size_t>(std::distance(app_.savedPalettes_.begin(), savedPalette));
                    SendMessageW(savedPaletteList_, LB_SETCURSEL, static_cast<WPARAM>(savedIndex), 0);
                }
                HandleSavedPaletteSelectionChanged(false);
            }
        }
        break;
    case kIdApplyPalette:
        if (notification == BN_CLICKED) {
            if (const auto selected = SelectedSavedPaletteIndex(); selected.has_value()) {
                static_cast<void>(app_.ApplyNamedPalette(*selected));
                HandleSavedPaletteSelectionChanged(true);
            }
        }
        break;
    case kIdDeletePalette:
        if (notification == BN_CLICKED) {
            if (const auto selected = SelectedSavedPaletteIndex(); selected.has_value()) {
                static_cast<void>(app_.DeleteNamedPalette(*selected));
                HandleSavedPaletteSelectionChanged(true);
            }
        }
        break;
    case kIdSavedPaletteList:
        if (notification == LBN_SELCHANGE) {
            HandleSavedPaletteSelectionChanged(true);
        } else if (notification == LBN_DBLCLK) {
            if (const auto selected = SelectedSavedPaletteIndex(); selected.has_value()) {
                static_cast<void>(app_.ApplyNamedPalette(*selected));
                HandleSavedPaletteSelectionChanged(true);
            }
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
    case kIdAutoHideToolbarSwitch:
        if (notification == BN_CLICKED) {
            const bool enabled = !app_.settings_.autoHideToolbar;
            app_.SetToolbarAutoHideEnabled(enabled);
            app_.SaveAppSettings();
            RefreshSliderValues();
        }
        break;
    default:
        if (controlId >= kIdZoomPresetBase && controlId < kIdZoomPresetBase + static_cast<int>(zoomPresetEdits_.size())) {
            if (notification == EN_CHANGE) {
                HandleZoomPresetEdit(reinterpret_cast<HWND>(lParam), false);
            } else if (notification == EN_KILLFOCUS) {
                HandleZoomPresetEdit(reinterpret_cast<HWND>(lParam), true);
            }
        }
        static_cast<void>(lParam);
        break;
    }
}

void SettingsWindow::HandleHScroll(LPARAM lParam) {
    if (reinterpret_cast<HWND>(lParam) == redSlider_
        || reinterpret_cast<HWND>(lParam) == greenSlider_
        || reinterpret_cast<HWND>(lParam) == blueSlider_) {
        UpdatePaletteFromHslSliders();
        RefreshPaletteList();
        RefreshSliderValues();
        InvalidateRect(app_.window_, nullptr, FALSE);
        if (reinterpret_cast<HWND>(lParam) == redSlider_) {
            InvalidateSliderPresentation(SliderKind::Hue);
        } else if (reinterpret_cast<HWND>(lParam) == greenSlider_) {
            InvalidateSliderPresentation(SliderKind::Saturation);
        } else {
            InvalidateSliderPresentation(SliderKind::Lightness);
        }
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
    if (reinterpret_cast<HWND>(lParam) == stabilizationSlider_) {
        InvalidateSliderPresentation(SliderKind::Stabilization);
    } else if (reinterpret_cast<HWND>(lParam) == spacingSlider_) {
        InvalidateSliderPresentation(SliderKind::Spacing);
    } else if (reinterpret_cast<HWND>(lParam) == strokeSmoothingSlider_) {
        InvalidateSliderPresentation(SliderKind::StrokeSmoothing);
    } else if (reinterpret_cast<HWND>(lParam) == autoSaveSlider_) {
        InvalidateSliderPresentation(SliderKind::AutoSave);
    }
}

HBRUSH SettingsWindow::HandleControlColor(HDC hdc, HWND control) {
    const bool isZoomEdit = std::find(zoomPresetEdits_.begin(), zoomPresetEdits_.end(), control) != zoomPresetEdits_.end();
    if (isZoomEdit || control == hexColorEdit_ || control == savedPaletteNameEdit_ || control == savedPaletteList_ || control == paletteList_ || control == shortcutList_) {
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, kPanelFill);
        SetTextColor(hdc, kTextPrimary);
        return panelBrush_;
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, (control == shortcutHintLabel_ || control == zoomPresetHintLabel_ || control == savedPaletteHintLabel_) ? kTextMuted : kTextPrimary);
    return panelBrush_;
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
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());

    SetBkMode(memoryDc, TRANSPARENT);
    HGDIOBJ previousFont = SelectObject(memoryDc, titleFont_ != nullptr ? titleFont_ : font_);
    SetTextColor(memoryDc, kTextPrimary);
    TextOutW(memoryDc, Scale(16), Scale(14), L"Settings", 8);

    FillRoundedRect(memoryDc, layout.tabRail, RGB(15, 20, 27));
    FillRoundedRect(memoryDc, layout.contentPanel, kPanelFill);
    StrokeRoundedRect(memoryDc, layout.tabRail, kPanelBorder, 1);
    StrokeRoundedRect(memoryDc, layout.contentPanel, kPanelBorder, 1);

    SelectObject(memoryDc, font_);
    SetTextColor(memoryDc, kTextMuted);
    DrawTextW(memoryDc, L"Categories", -1, const_cast<RECT*>(&layout.tabRailLabelRect), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const auto paintWrappedDescription = [&](const RECT& sectionRect, const wchar_t* text) {
        RECT descriptionRect = MakeRect(
            sectionRect.left + Scale(14),
            sectionRect.top + Scale(34),
            sectionRect.right - Scale(14),
            sectionRect.top + Scale(74));
        DrawTextW(memoryDc, text, -1, &descriptionRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    };

    switch (activeCategory_) {
    case Category::Palette:
    {
        SetTextColor(memoryDc, kTextPrimary);
        TextOutW(memoryDc, layout.paletteSection.left + Scale(14), layout.paletteSection.top + Scale(14), L"Current Palette", 15);
        TextOutW(memoryDc, layout.savedPaletteSection.left + Scale(14), layout.savedPaletteSection.top + Scale(14), L"Saved Palettes", 14);
        TextOutW(memoryDc, layout.colorSection.left + Scale(14), layout.colorSection.top + Scale(14), L"Selected Color", 14);
        SetTextColor(memoryDc, kTextMuted);
        paintWrappedDescription(layout.savedPaletteSection, L"Save the current swatches under a name, then select and apply them later.");
        paintWrappedDescription(layout.paletteSection, L"Manage your active swatches without leaving the document.");
        paintWrappedDescription(layout.colorSection, L"Adjust the active palette color in HSL.");
        RECT savedPaletteLabelRect = layout.savedPaletteLabelRect;
        DrawTextW(memoryDc, L"Palette Name", -1, &savedPaletteLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (!app_.palette_.empty()) {
            RECT previewRect = layout.colorPreviewRect;
            const std::uint32_t color = app_.palette_[app_.settings_.paletteIndex];
            FillRoundedRect(memoryDc, previewRect, app_.ToColorRef(color));
            StrokeRoundedRect(memoryDc, previewRect, kSelectionBorder, 2);

            RECT previewLabel = layout.hexColorEditRect;
            SetTextColor(memoryDc, (app_.GetColorChannel(color, 16)
                    + app_.GetColorChannel(color, 8)
                    + app_.GetColorChannel(color, 0)) > 384
                ? RGB(22, 28, 36)
                : RGB(248, 250, 252));
            const std::wstring previewText = FormatHexColor(color);
            DrawTextW(memoryDc, previewText.c_str(), -1, &previewLabel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    }
    case Category::Drawing:
        SetTextColor(memoryDc, kTextPrimary);
        TextOutW(memoryDc, layout.drawingSection.left + Scale(14), layout.drawingSection.top + Scale(14), L"Stroke Feel", 11);
        TextOutW(memoryDc, layout.drawingMetaSection.left + Scale(14), layout.drawingMetaSection.top + Scale(14), L"Persistence", 11);
        SetTextColor(memoryDc, kTextMuted);
        paintWrappedDescription(layout.drawingSection, L"Tune input filtering and smoothing.");
        paintWrappedDescription(layout.drawingMetaSection, L"Configure auto save and quick zoom presets.");
        break;
    case Category::Shortcuts:
        SetTextColor(memoryDc, kTextPrimary);
        TextOutW(memoryDc, layout.shortcutListSection.left + Scale(14), layout.shortcutListSection.top + Scale(14), L"Commands", 8);
        TextOutW(memoryDc, layout.shortcutActionsSection.left + Scale(14), layout.shortcutActionsSection.top + Scale(14), L"Binding", 7);
        SetTextColor(memoryDc, kTextMuted);
        paintWrappedDescription(layout.shortcutListSection, L"Pick a command to inspect or rebind.");
        paintWrappedDescription(layout.shortcutActionsSection, L"Capture a new key or clear the selected binding.");
        break;
    }

    const auto paintSlider = [&](SliderKind kind, RECT sliderRect, int minimum, int maximum) {
        const RECT trackRect = SliderTrackRect(sliderRect, CurrentDpi());
        const float hue = static_cast<float>(SliderValue(SliderKind::Hue));
        const float saturation = static_cast<float>(SliderValue(SliderKind::Saturation)) / 100.0f;
        const float lightness = static_cast<float>(SliderValue(SliderKind::Lightness)) / 100.0f;
        switch (kind) {
        case SliderKind::Hue:
            FillHuePreviewTrack(memoryDc, trackRect, saturation, lightness);
            break;
        case SliderKind::Saturation:
            FillSaturationPreviewTrack(memoryDc, trackRect, hue, lightness);
            break;
        case SliderKind::Lightness:
            FillLightnessPreviewTrack(memoryDc, trackRect, hue, saturation);
            break;
        default:
            FillRoundedRect(memoryDc, trackRect, RGB(41, 51, 66));
            break;
        }
        const int thumbCenterX = SliderThumbCenterX(trackRect, SliderValue(kind), minimum, maximum);
        if (kind != SliderKind::Hue && kind != SliderKind::Saturation && kind != SliderKind::Lightness) {
            RECT fillRect = trackRect;
            fillRect.right = static_cast<LONG>(std::max<int>(fillRect.left + 1, thumbCenterX + 1));
            FillRoundedRect(memoryDc, fillRect, RGB(92, 137, 187));
        }

        const int centerY = (sliderRect.top + sliderRect.bottom) / 2;
        RECT thumbRect = MakeRect(
            thumbCenterX - Scale(6),
            centerY - Scale(6),
            thumbCenterX + Scale(7),
            centerY + Scale(7));
        FillRoundedRect(memoryDc, thumbRect, RGB(244, 247, 252));
        StrokeRoundedRect(memoryDc, thumbRect, draggingSlider_ == kind ? RGB(255, 255, 255) : RGB(120, 138, 162), 1);
    };

    if (activeCategory_ == Category::Palette) {
        paintSlider(SliderKind::Hue, layout.redSliderRect, 0, 360);
        paintSlider(SliderKind::Saturation, layout.greenSliderRect, 0, 100);
        paintSlider(SliderKind::Lightness, layout.blueSliderRect, 0, 100);
    } else if (activeCategory_ == Category::Drawing) {
        paintSlider(SliderKind::Stabilization, layout.stabilizationSliderRect, 5, 95);
        paintSlider(SliderKind::Spacing, layout.spacingSliderRect, 5, 405);
        paintSlider(SliderKind::StrokeSmoothing, layout.strokeSmoothingSliderRect, 0, 100);
        paintSlider(SliderKind::AutoSave, layout.autoSaveSliderRect, 0, 300);
    } else if (activeCategory_ == Category::Shortcuts) {
        const RECT shortcutListRect = ControlRectInParent(shortcutList_, window_);
        const RECT shortcutTrackRect = MakeRect(
            shortcutListRect.right + Scale(kScrollbarGap),
            shortcutListRect.top,
            shortcutListRect.right + Scale(kScrollbarGap + kScrollbarWidth),
            shortcutListRect.bottom);
        RECT shortcutThumbRect {};
        if (ComputeListScrollbarThumbRect(shortcutList_, shortcutListRect, shortcutTrackRect, Scale(26), CurrentDpi(), shortcutThumbRect)) {
            FillRoundedRect(memoryDc, shortcutTrackRect, kScrollbarTrackFill);
            StrokeRoundedRect(memoryDc, shortcutTrackRect, kPanelBorder, 1);
            FillRoundedRect(memoryDc, shortcutThumbRect, draggingShortcutScrollbar_ ? kScrollbarThumbActiveFill : kScrollbarThumbFill);
        }
    }

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

void SettingsWindow::DrawButton(const DRAWITEMSTRUCT& drawItem) {
    const bool selected = (drawItem.itemState & ODS_SELECTED) != 0;
    const bool focused = (drawItem.itemState & ODS_FOCUS) != 0;
    const bool disabled = (drawItem.itemState & ODS_DISABLED) != 0;
    if (drawItem.CtlID == kIdAutoHideToolbarSwitch) {
        HBRUSH backgroundBrush = CreateSolidBrush(kPanelFill);
        FillRect(drawItem.hDC, &drawItem.rcItem, backgroundBrush);
        DeleteObject(backgroundBrush);

        const bool checked = app_.settings_.autoHideToolbar;
        RECT labelRect = drawItem.rcItem;
        labelRect.left += Scale(2);
        labelRect.right -= Scale(66);
        SetBkMode(drawItem.hDC, TRANSPARENT);
        SetTextColor(drawItem.hDC, disabled ? RGB(102, 111, 124) : kTextPrimary);
        SelectObject(drawItem.hDC, font_);

        wchar_t label[128] = {};
        GetWindowTextW(drawItem.hwndItem, label, static_cast<int>(std::size(label)));
        DrawTextW(drawItem.hDC, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const int switchWidth = Scale(30);
        const int switchHeight = Scale(16);
        RECT switchRect = MakeRect(
            drawItem.rcItem.right - switchWidth - Scale(2),
            drawItem.rcItem.top + ((drawItem.rcItem.bottom - drawItem.rcItem.top - switchHeight) / 2),
            drawItem.rcItem.right - Scale(2),
            drawItem.rcItem.top + ((drawItem.rcItem.bottom - drawItem.rcItem.top - switchHeight) / 2) + switchHeight);
        const COLORREF trackFill = checked ? RGB(52, 96, 143) : RGB(35, 44, 57);
        const COLORREF trackBorder = checked ? RGB(92, 137, 187) : RGB(65, 78, 96);
        FillRoundedRect(drawItem.hDC, switchRect, selected ? RGB(58, 74, 94) : trackFill);
        StrokeRoundedRect(drawItem.hDC, switchRect, focused ? kSelectionBorder : trackBorder, focused ? 2 : 1);

        const int knobInset = Scale(2);
        const int knobSize = std::max(1, switchHeight - (knobInset * 2));
        const int knobLeft = checked
            ? switchRect.right - knobInset - knobSize
            : switchRect.left + knobInset;
        RECT knobRect = MakeRect(
            knobLeft,
            switchRect.top + knobInset,
            knobLeft + knobSize,
            switchRect.top + knobInset + knobSize);
        FillRoundedRect(drawItem.hDC, knobRect, disabled ? RGB(150, 158, 169) : RGB(244, 247, 252));
        StrokeRoundedRect(drawItem.hDC, knobRect, checked ? RGB(205, 221, 239) : RGB(118, 132, 150), 1);
        return;
    }

    const bool isCategoryButton = drawItem.CtlID >= kIdCategoryPalette && drawItem.CtlID <= kIdCategoryShortcuts;
    bool isActiveCategory = false;
    if (isCategoryButton) {
        switch (drawItem.CtlID) {
        case kIdCategoryPalette:
            isActiveCategory = activeCategory_ == Category::Palette;
            break;
        case kIdCategoryDrawing:
            isActiveCategory = activeCategory_ == Category::Drawing;
            break;
        case kIdCategoryShortcuts:
            isActiveCategory = activeCategory_ == Category::Shortcuts;
            break;
        default:
            break;
        }
    }

    const COLORREF background = isCategoryButton ? RGB(15, 20, 27) : kPanelFill;
    COLORREF fill = kButtonFill;
    COLORREF border = fill;
    COLORREF text = disabled ? RGB(102, 111, 124) : kTextPrimary;
    if (isCategoryButton) {
        fill = isActiveCategory ? kButtonFillActive : (selected ? kButtonFillHover : RGB(22, 28, 36));
        border = isActiveCategory ? RGB(92, 137, 187) : fill;
        text = isActiveCategory ? kTextPrimary : (disabled ? RGB(102, 111, 124) : RGB(202, 210, 220));
    } else if (selected) {
        fill = kButtonFillActive;
        border = RGB(92, 137, 187);
    }

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
    if (isCategoryButton) {
        labelRect.left += Scale(14);
        DrawTextW(drawItem.hDC, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        DrawTextW(drawItem.hDC, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void SettingsWindow::DrawPaletteItem(const DRAWITEMSTRUCT& drawItem) {
    if (drawItem.itemID == static_cast<UINT>(-1) || drawItem.itemID >= app_.palette_.size()) {
        return;
    }

    const bool selected = (drawItem.itemState & ODS_SELECTED) != 0;
    HBRUSH itemBrush = CreateSolidBrush(selected ? RGB(32, 40, 52) : kPanelFill);
    FillRect(drawItem.hDC, &drawItem.rcItem, itemBrush);
    DeleteObject(itemBrush);

    if (selected) {
        StrokeRoundedRect(drawItem.hDC, drawItem.rcItem, RGB(92, 137, 187), 1);
    }

    const std::uint32_t color = app_.palette_[drawItem.itemID];
    RECT swatchRect = drawItem.rcItem;
    swatchRect.left += Scale(8);
    swatchRect.top += Scale(7);
    swatchRect.right = swatchRect.left + Scale(34);
    swatchRect.bottom -= Scale(7);
    FillRoundedRect(drawItem.hDC, swatchRect, app_.ToColorRef(color));
    StrokeRoundedRect(drawItem.hDC, swatchRect, RGB(200, 208, 218), 1);

    RECT textRect = drawItem.rcItem;
    textRect.left = swatchRect.right + Scale(12);
    textRect.right -= Scale(8);
    SetBkMode(drawItem.hDC, TRANSPARENT);
    SetTextColor(drawItem.hDC, kTextPrimary);
    SelectObject(drawItem.hDC, font_);
    const std::wstring label = FormatPaletteEntry(drawItem.itemID, color);
    DrawTextW(drawItem.hDC, label.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void SettingsWindow::DrawSavedPaletteItem(const DRAWITEMSTRUCT& drawItem) {
    if (drawItem.itemID == static_cast<UINT>(-1) || drawItem.itemID >= app_.savedPalettes_.size()) {
        return;
    }

    const bool selected = (drawItem.itemState & ODS_SELECTED) != 0;
    HBRUSH itemBrush = CreateSolidBrush(selected ? RGB(32, 40, 52) : kPanelFill);
    FillRect(drawItem.hDC, &drawItem.rcItem, itemBrush);
    DeleteObject(itemBrush);

    if (selected) {
        StrokeRoundedRect(drawItem.hDC, drawItem.rcItem, RGB(92, 137, 187), 1);
    }

    const App::NamedPalette& palette = app_.savedPalettes_[drawItem.itemID];
    RECT nameRect = drawItem.rcItem;
    nameRect.left += Scale(10);
    nameRect.right -= Scale(10);
    nameRect.top += Scale(6);
    nameRect.bottom = nameRect.top + Scale(18);

    SetBkMode(drawItem.hDC, TRANSPARENT);
    SetTextColor(drawItem.hDC, kTextPrimary);
    SelectObject(drawItem.hDC, font_);
    DrawTextW(drawItem.hDC, palette.name.c_str(), -1, &nameRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const int swatchSize = Scale(14);
    const int swatchGap = Scale(6);
    int swatchLeft = drawItem.rcItem.left + Scale(10);
    const int swatchTop = drawItem.rcItem.top + Scale(30);
    const int swatchBottom = swatchTop + swatchSize;
    const int availableRight = drawItem.rcItem.right - Scale(10);

    std::size_t visibleCount = 0;
    for (std::size_t index = 0; index < palette.colors.size(); ++index) {
        const int nextRight = swatchLeft + swatchSize;
        if (nextRight > availableRight) {
            break;
        }
        RECT swatchRect = MakeRect(swatchLeft, swatchTop, nextRight, swatchBottom);
        FillRoundedRect(drawItem.hDC, swatchRect, app_.ToColorRef(palette.colors[index]));
        StrokeRoundedRect(drawItem.hDC, swatchRect, RGB(200, 208, 218), 1);
        swatchLeft = nextRight + swatchGap;
        visibleCount = index + 1U;
    }

    if (visibleCount < palette.colors.size()) {
        RECT moreRect = MakeRect(swatchLeft, swatchTop - Scale(1), availableRight, swatchBottom + Scale(1));
        SetTextColor(drawItem.hDC, kTextMuted);
        const std::wstring moreText = L"+" + std::to_wstring(palette.colors.size() - visibleCount);
        DrawTextW(drawItem.hDC, moreText.c_str(), -1, &moreRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
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
    textRect.left += Scale(10);
    SetBkMode(drawItem.hDC, TRANSPARENT);
    SetTextColor(drawItem.hDC, kTextPrimary);
    SelectObject(drawItem.hDC, font_);
    DrawTextW(drawItem.hDC, buffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void SettingsWindow::HandleShortcutSelectionChanged() {
    RefreshShortcutButtons();
}

void SettingsWindow::HandleSavedPaletteSelectionChanged(bool syncNameField) {
    if (syncNameField) {
        if (const auto selected = SelectedSavedPaletteIndex(); selected.has_value() && *selected < app_.savedPalettes_.size()) {
            SetControlText(savedPaletteNameEdit_, app_.savedPalettes_[*selected].name);
        } else {
            SetControlText(savedPaletteNameEdit_, L"");
        }
    }
    RefreshSavedPaletteButtons();
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

std::optional<std::size_t> SettingsWindow::SelectedSavedPaletteIndex() const {
    if (savedPaletteList_ == nullptr) {
        return std::nullopt;
    }

    const int selection = static_cast<int>(SendMessageW(savedPaletteList_, LB_GETCURSEL, 0, 0));
    if (selection < 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(selection);
}

void SettingsWindow::HandleZoomPresetEdit(HWND edit, bool normalizeText) {
    if (edit == nullptr) {
        return;
    }

    const auto presetIt = std::find(zoomPresetEdits_.begin(), zoomPresetEdits_.end(), edit);
    if (presetIt == zoomPresetEdits_.end()) {
        return;
    }

    const std::size_t presetIndex = static_cast<std::size_t>(std::distance(zoomPresetEdits_.begin(), presetIt));
    wchar_t buffer[64] = {};
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    std::wstring text = buffer;
    text.erase(std::remove_if(text.begin(), text.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0 || ch == L'%';
    }), text.end());

    if (text.empty()) {
        app_.settings_.zoomPresets[presetIndex] = 0.0f;
        app_.SaveAppSettings();
        if (normalizeText) {
            SetControlText(edit, L"");
        }
        return;
    }

    wchar_t* end = nullptr;
    const float parsedPercent = std::wcstof(text.c_str(), &end);
    if (end == text.c_str() || *end != L'\0' || parsedPercent <= 0.0f) {
        if (normalizeText) {
            SetControlText(edit, FormatZoomPresetValue(app_.settings_.zoomPresets[presetIndex]));
        }
        return;
    }

    app_.settings_.zoomPresets[presetIndex] = std::clamp(parsedPercent / 100.0f, App::kMinZoom, App::kMaxZoom);
    app_.SaveAppSettings();
    if (normalizeText) {
        SetControlText(edit, FormatZoomPresetValue(app_.settings_.zoomPresets[presetIndex]));
    }
}

void SettingsWindow::UpdatePaletteFromHslSliders() {
    if (app_.palette_.empty()) {
        return;
    }

    const float hue = static_cast<float>(SendMessageW(redSlider_, TBM_GETPOS, 0, 0));
    const float saturation = static_cast<float>(SendMessageW(greenSlider_, TBM_GETPOS, 0, 0)) / 100.0f;
    const float lightness = static_cast<float>(SendMessageW(blueSlider_, TBM_GETPOS, 0, 0)) / 100.0f;
    app_.palette_[app_.settings_.paletteIndex] = HslToColor(hue, saturation, lightness);
    app_.SaveAppSettings();
}

HWND SettingsWindow::SliderHandle(SliderKind kind) const {
    switch (kind) {
    case SliderKind::Hue:
        return redSlider_;
    case SliderKind::Saturation:
        return greenSlider_;
    case SliderKind::Lightness:
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
        case SliderKind::Hue:
            SendMessageW(slider, TBM_SETPOS, TRUE, clampValue(0, 360));
            UpdatePaletteFromHslSliders();
            RefreshPaletteList();
            RefreshSliderValues();
            InvalidateRect(app_.window_, nullptr, FALSE);
            InvalidateSliderPresentation(SliderKind::Hue);
            return;
        case SliderKind::Saturation:
        case SliderKind::Lightness:
            SendMessageW(slider, TBM_SETPOS, TRUE, clampValue(0, 100));
            UpdatePaletteFromHslSliders();
            RefreshPaletteList();
            RefreshSliderValues();
            InvalidateRect(app_.window_, nullptr, FALSE);
            InvalidateSliderPresentation(kind);
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
    InvalidateSliderPresentation(kind);
}

void SettingsWindow::UpdateSliderFromPoint(SliderKind kind, POINT cursor) {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());
    RECT sliderRect {};
    int minimum = 0;
    int maximum = 100;
    switch (kind) {
    case SliderKind::Hue:
        sliderRect = layout.redSliderRect;
        minimum = 0;
        maximum = 360;
        break;
    case SliderKind::Saturation:
        sliderRect = layout.greenSliderRect;
        minimum = 0;
        maximum = 100;
        break;
    case SliderKind::Lightness:
        sliderRect = layout.blueSliderRect;
        minimum = 0;
        maximum = 100;
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

    const RECT trackRect = SliderTrackRect(sliderRect, CurrentDpi());
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
    const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());

    if (activeCategory_ == Category::Palette) {
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

    if (activeCategory_ == Category::Drawing) {
        const std::array<std::pair<SliderKind, RECT>, 4> sliders {{
            { SliderKind::Stabilization, layout.stabilizationSliderRect },
            { SliderKind::Spacing, layout.spacingSliderRect },
            { SliderKind::StrokeSmoothing, layout.strokeSmoothingSliderRect },
            { SliderKind::AutoSave, layout.autoSaveSliderRect },
        }};
        for (const auto& [kind, rect] : sliders) {
            RECT hitRect = rect;
            hitRect.top -= Scale(4);
            hitRect.bottom += Scale(4);
            if (PtInRect(&hitRect, cursor) != 0) {
                return kind;
            }
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
        minMax->ptMinTrackSize.x = Scale(kWindowWidth);
        minMax->ptMinTrackSize.y = Scale(kWindowHeight);
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
        if (activeCategory_ == Category::Shortcuts) {
            POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(window_, &cursor);
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());
            const RECT shortcutListRect = ControlRectInParent(shortcutList_, window_);
            const RECT shortcutTrackRect = MakeRect(
                shortcutListRect.right + Scale(kScrollbarGap),
                shortcutListRect.top,
                shortcutListRect.right + Scale(kScrollbarGap + kScrollbarWidth),
                shortcutListRect.bottom);
            if (PtInRect(&layout.shortcutListSection, cursor) != 0 || PtInRect(&shortcutTrackRect, cursor) != 0) {
                SendMessageW(shortcutList_, WM_MOUSEWHEEL, wParam, lParam);
                return 0;
            }
        }
        break;
    case WM_LBUTTONDOWN:
    {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());

        if (editingHexColor_ && PtInRect(&layout.hexColorEditRect, cursor) == 0) {
            CommitHexColorEdit(true);
        }
        if (activeCategory_ == Category::Shortcuts) {
            const RECT shortcutListRect = ControlRectInParent(shortcutList_, window_);
            const RECT shortcutTrackRect = MakeRect(
                shortcutListRect.right + Scale(kScrollbarGap),
                shortcutListRect.top,
                shortcutListRect.right + Scale(kScrollbarGap + kScrollbarWidth),
                shortcutListRect.bottom);
            RECT shortcutThumbRect {};
            if (ComputeListScrollbarThumbRect(shortcutList_, shortcutListRect, shortcutTrackRect, Scale(26), CurrentDpi(), shortcutThumbRect)) {
                if (PtInRect(&shortcutThumbRect, cursor) != 0) {
                    draggingShortcutScrollbar_ = true;
                    shortcutScrollbarDragOffset_ = cursor.y - shortcutThumbRect.top;
                    SetCapture(window_);
                    InvalidateWindowRegion(shortcutTrackRect);
                    return 0;
                }
                if (PtInRect(&shortcutTrackRect, cursor) != 0) {
                    const int pageStep = std::max(1, static_cast<int>((shortcutListRect.bottom - shortcutListRect.top) / Scale(26)) - 2);
                    int topIndex = static_cast<int>(SendMessageW(shortcutList_, LB_GETTOPINDEX, 0, 0));
                    topIndex += cursor.y < shortcutThumbRect.top ? -pageStep : pageStep;
                    SendMessageW(shortcutList_, LB_SETTOPINDEX, static_cast<WPARAM>(std::max(0, topIndex)), 0);
                    InvalidateWindowRegion(layout.shortcutListSection);
                    return 0;
                }
            }
        }
        if (activeCategory_ == Category::Palette && PtInRect(&layout.colorPreviewRect, cursor) != 0) {
            BeginHexColorEdit();
            return 0;
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
        if (draggingShortcutScrollbar_) {
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());
            const RECT shortcutListRect = ControlRectInParent(shortcutList_, window_);
            const RECT shortcutTrackRect = MakeRect(
                shortcutListRect.right + Scale(kScrollbarGap),
                shortcutListRect.top,
                shortcutListRect.right + Scale(kScrollbarGap + kScrollbarWidth),
                shortcutListRect.bottom);
            RECT shortcutThumbRect {};
            if (!ComputeListScrollbarThumbRect(shortcutList_, shortcutListRect, shortcutTrackRect, Scale(26), CurrentDpi(), shortcutThumbRect)) {
                return 0;
            }

            const int thumbHeight = static_cast<int>(shortcutThumbRect.bottom - shortcutThumbRect.top);
            const int itemCount = static_cast<int>(SendMessageW(shortcutList_, LB_GETCOUNT, 0, 0));
            const int visibleItems = std::max(1, static_cast<int>((shortcutListRect.bottom - shortcutListRect.top) / Scale(26)));
            const int maxTopIndex = std::max(0, itemCount - visibleItems);
            const int travel = std::max(1, static_cast<int>(shortcutTrackRect.bottom - shortcutTrackRect.top) - thumbHeight);
            const int thumbTop = std::clamp(
                GET_Y_LPARAM(lParam) - shortcutScrollbarDragOffset_,
                static_cast<int>(shortcutTrackRect.top),
                static_cast<int>(shortcutTrackRect.bottom) - thumbHeight);
            const int newTopIndex = maxTopIndex > 0
                ? ((thumbTop - static_cast<int>(shortcutTrackRect.top)) * maxTopIndex) / travel
                : 0;
            SendMessageW(shortcutList_, LB_SETTOPINDEX, static_cast<WPARAM>(newTopIndex), 0);
            InvalidateWindowRegion(layout.shortcutListSection);
            return 0;
        }
        if (draggingSlider_.has_value()) {
            UpdateSliderFromPoint(*draggingSlider_, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (draggingShortcutScrollbar_) {
            draggingShortcutScrollbar_ = false;
            ReleaseCapture();
            const RECT shortcutListRect = ControlRectInParent(shortcutList_, window_);
            const RECT shortcutTrackRect = MakeRect(
                shortcutListRect.right + Scale(kScrollbarGap),
                shortcutListRect.top,
                shortcutListRect.right + Scale(kScrollbarGap + kScrollbarWidth),
                shortcutListRect.bottom);
            InvalidateWindowRegion(shortcutTrackRect);
            return 0;
        }
        if (draggingSlider_.has_value()) {
            const SliderKind slider = *draggingSlider_;
            draggingSlider_.reset();
            ReleaseCapture();
            InvalidateSliderPresentation(slider);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (draggingShortcutScrollbar_) {
            draggingShortcutScrollbar_ = false;
            const RECT shortcutListRect = ControlRectInParent(shortcutList_, window_);
            const RECT shortcutTrackRect = MakeRect(
                shortcutListRect.right + Scale(kScrollbarGap),
                shortcutListRect.top,
                shortcutListRect.right + Scale(kScrollbarGap + kScrollbarWidth),
                shortcutListRect.bottom);
            InvalidateWindowRegion(shortcutTrackRect);
        }
        if (draggingSlider_.has_value()) {
            const SliderKind slider = *draggingSlider_;
            draggingSlider_.reset();
            InvalidateSliderPresentation(slider);
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
    case WM_MEASUREITEM:
    {
        auto* measureItem = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measureItem == nullptr) {
            return FALSE;
        }
        if (measureItem->CtlID == kIdPaletteList) {
            measureItem->itemHeight = static_cast<UINT>(Scale(46));
            return TRUE;
        }
        if (measureItem->CtlID == kIdSavedPaletteList) {
            measureItem->itemHeight = static_cast<UINT>(Scale(58));
            return TRUE;
        }
        if (measureItem->CtlID == kIdShortcutList) {
            measureItem->itemHeight = static_cast<UINT>(Scale(26));
            return TRUE;
        }
        break;
    }
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
            if (drawItem->CtlID == kIdSavedPaletteList) {
                DrawSavedPaletteItem(*drawItem);
                return TRUE;
            }
            if (drawItem->CtlID == kIdShortcutList) {
                DrawShortcutItem(*drawItem);
                return TRUE;
            }
        }
        break;
    }
    case kMsgShortcutListInvalidated:
    {
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        const SettingsWindowLayout layout = BuildSettingsWindowLayout(clientRect, CurrentDpi());
        InvalidateWindowRegion(layout.shortcutListSection);
        return 0;
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
        draggingSlider_.reset();
        draggingShortcutScrollbar_ = false;
        activeCategory_ = Category::Palette;
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace cnote
