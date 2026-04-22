#pragma once

#include <optional>

#include <windows.h>

#include "App.h"

namespace cnote {

class SettingsWindow {
public:
    explicit SettingsWindow(App& app);
    ~SettingsWindow();

    void Toggle(HWND owner);
    void Show(HWND owner);
    bool IsOpen() const;
    void Refresh();

private:
    enum class Category {
        Palette,
        Drawing,
        Shortcuts,
    };

    enum class SliderKind {
        Hue,
        Saturation,
        Lightness,
        Stabilization,
        Spacing,
        StrokeSmoothing,
        DashLength,
        DashGap,
        AutoSave,
    };

    void CreateIfNeeded(HWND owner);
    void CreateControls();
    void ApplyFonts();
    void RebuildDpiResources(UINT dpi);
    void LayoutControls();
    void SetActiveCategory(Category category);
    void RefreshVisibleControls();
    void RefreshCategoryButtons();
    void InvalidateWindowRegion(const RECT& rect);
    void InvalidateSliderPresentation(SliderKind kind);
    void BeginHexColorEdit();
    void CommitHexColorEdit(bool applyChanges);
    void RefreshPaletteList();
    void RefreshSavedPaletteList();
    void RefreshSliderValues();
    void RefreshZoomPresetFields();
    void RefreshShortcutList();
    void RefreshShortcutButtons();
    void RefreshSavedPaletteButtons();
    void HandleCommand(WPARAM wParam, LPARAM lParam);
    void HandleHScroll(LPARAM lParam);
    HBRUSH HandleControlColor(HDC hdc, HWND control);
    void PaintWindow();
    void DrawButton(const DRAWITEMSTRUCT& drawItem);
    void DrawPaletteItem(const DRAWITEMSTRUCT& drawItem);
    void DrawSavedPaletteItem(const DRAWITEMSTRUCT& drawItem);
    void DrawShortcutItem(const DRAWITEMSTRUCT& drawItem);
    void HandleShortcutSelectionChanged();
    void HandleSavedPaletteSelectionChanged(bool syncNameField);
    void BeginShortcutCapture();
    void ClearSelectedShortcut();
    void HandleShortcutCapture(UINT key);
    void HandleZoomPresetEdit(HWND edit, bool normalizeText);
    void UpdatePaletteFromHslSliders();
    [[nodiscard]] std::optional<std::size_t> SelectedSavedPaletteIndex() const;
    [[nodiscard]] HWND SliderHandle(SliderKind kind) const;
    [[nodiscard]] int SliderValue(SliderKind kind) const;
    void UpdateSliderValue(SliderKind kind, int value);
    void UpdateSliderFromPoint(SliderKind kind, POINT cursor);
    [[nodiscard]] std::optional<SliderKind> HitTestSlider(POINT cursor) const;
    [[nodiscard]] UINT CurrentDpi() const;
    [[nodiscard]] int Scale(int value) const;
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    App& app_;
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    HFONT titleFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    std::array<HWND, 3> categoryButtons_ {};
    HWND paletteList_ = nullptr;
    HWND addColorButton_ = nullptr;
    HWND removeColorButton_ = nullptr;
    HWND hexColorEdit_ = nullptr;
    HWND savedPaletteNameEdit_ = nullptr;
    HWND savePaletteButton_ = nullptr;
    HWND applyPaletteButton_ = nullptr;
    HWND deletePaletteButton_ = nullptr;
    HWND savedPaletteList_ = nullptr;
    HWND savedPaletteHintLabel_ = nullptr;
    HWND redSlider_ = nullptr;
    HWND redLabel_ = nullptr;
    HWND greenSlider_ = nullptr;
    HWND greenLabel_ = nullptr;
    HWND blueSlider_ = nullptr;
    HWND blueLabel_ = nullptr;
    HWND stabilizationSlider_ = nullptr;
    HWND stabilizationLabel_ = nullptr;
    HWND spacingSlider_ = nullptr;
    HWND spacingLabel_ = nullptr;
    HWND strokeSmoothingSlider_ = nullptr;
    HWND strokeSmoothingLabel_ = nullptr;
    HWND dashLengthSlider_ = nullptr;
    HWND dashLengthLabel_ = nullptr;
    HWND dashGapSlider_ = nullptr;
    HWND dashGapLabel_ = nullptr;
    HWND autoSaveSlider_ = nullptr;
    HWND autoSaveLabel_ = nullptr;
    HWND autoHideToolbarSwitch_ = nullptr;
    HWND embedProjectInPdfSwitch_ = nullptr;
    std::array<HWND, App::kZoomPresetCount> zoomPresetLabels_ {};
    std::array<HWND, App::kZoomPresetCount> zoomPresetEdits_ {};
    HWND zoomPresetHintLabel_ = nullptr;
    HWND shortcutList_ = nullptr;
    HWND changeShortcutButton_ = nullptr;
    HWND clearShortcutButton_ = nullptr;
    HWND shortcutHintLabel_ = nullptr;
    std::optional<App::ShortcutAction> capturingShortcutAction_ {};
    std::optional<SliderKind> draggingSlider_ {};
    bool draggingShortcutScrollbar_ = false;
    int shortcutScrollbarDragOffset_ = 0;
    bool editingHexColor_ = false;
    Category activeCategory_ = Category::Palette;
    UINT currentDpi_ = USER_DEFAULT_SCREEN_DPI;
};

}  // namespace cnote
