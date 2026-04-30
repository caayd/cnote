#pragma once

#include <optional>

#include <windows.h>

#include "App.h"

namespace cnote {

class NewPageSettingsWindow {
public:
    explicit NewPageSettingsWindow(App& app);
    ~NewPageSettingsWindow();

    void Toggle(HWND owner);
    void Show(HWND owner);
    bool IsOpen() const;
    void Refresh();

private:
    enum class SliderKind {
        Hue,
        Saturation,
        Lightness,
    };

    void CreateIfNeeded(HWND owner);
    void CreateControls();
    void ApplyFonts();
    void RebuildDpiResources(UINT dpi);
    void LayoutControls();
    void UpdateScrollInfo();
    void ScrollTo(int scrollOffset);
    void SyncDraftFromApp();
    void UpdateEditValues();
    void HandleCommand(WPARAM wParam, LPARAM lParam);
    HBRUSH HandleControlColor(HDC hdc, HWND control);
    void PaintWindow();
    void DrawButton(const DRAWITEMSTRUCT& drawItem);
    void BeginHexColorEdit();
    void CommitHexColorEdit(bool applyChanges);
    void OpenDimensionPresetMenu();
    void OpenPaletteProfileMenu();
    void ApplyDraftToCurrentPage();
    void ApplyDraftToAllPages();
    void ApplyDraftToFuturePages();
    void ResetDraftToDefaults();
    [[nodiscard]] std::wstring DraftPaletteProfileLabel() const;
    void UpdateSliderFromPoint(SliderKind kind, POINT cursor);
    [[nodiscard]] std::optional<SliderKind> HitTestSlider(POINT cursor) const;
    [[nodiscard]] int SliderValue(SliderKind kind) const;
    void SetSliderValue(SliderKind kind, int value);
    void HandleDimensionEdit(HWND edit, float& value, bool normalizeText);
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
    HBRUSH inputBrush_ = nullptr;
    HBRUSH toolbarBrush_ = nullptr;
    HWND widthEdit_ = nullptr;
    HWND heightEdit_ = nullptr;
    HWND hexColorEdit_ = nullptr;
    HWND appendButton_ = nullptr;
    HWND resetButton_ = nullptr;
    HWND applyCurrentButton_ = nullptr;
    HWND applyAllButton_ = nullptr;
    HWND applyFutureButton_ = nullptr;
    float draftWidth_ = 612.0f;
    float draftHeight_ = 792.0f;
    std::uint32_t draftBackgroundColor_ = 0xFFFFFFFFU;
    std::vector<std::uint32_t> draftPaletteProfile_ {};
    bool draftApplyPaletteProfileToExistingStrokes_ = false;
    std::optional<SliderKind> draggingSlider_ {};
    UINT currentDpi_ = USER_DEFAULT_SCREEN_DPI;
    bool suppressEditRefresh_ = false;
    int scrollOffset_ = 0;
    int contentHeight_ = 0;
    bool draggingScrollbar_ = false;
    int scrollbarDragOffset_ = 0;
    int wheelRemainder_ = 0;
    bool editingHexColor_ = false;
};

}  // namespace cnote
