#pragma once

#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include "Document.h"
#include "PdfBackend.h"

namespace ctrainer {

class App {
public:
    explicit App(HINSTANCE instance);
    ~App();

    int Run(int showCommand);

private:
    enum class Tool {
        Draw,
        Pan,
        Erase,
    };

    enum class HistoryActionType {
        AddStroke,
        RemoveStroke,
    };

    enum class SliderTarget {
        None,
        Red,
        Green,
        Blue,
        BrushSize,
        Stabilization,
        Spacing,
        Smoothing,
    };

    struct PageView {
        RECT rect {};
        std::size_t pageIndex = 0;
        float scale = 1.0f;
        bool valid = false;
    };

    struct HistoryAction {
        HistoryActionType type = HistoryActionType::AddStroke;
        Stroke stroke {};
        std::size_t index = 0;
        std::size_t pageIndex = 0;
    };

    struct PageSurfaceCache {
        int width = 0;
        int height = 0;
        int stride = 0;
        std::size_t pageIndex = 0;
        std::size_t strokeCount = 0;
        int rasterWidth = 0;
        int rasterHeight = 0;
        float rasterScale = 0.0f;
        float pageScale = 0.0f;
        bool valid = false;
        std::vector<std::uint8_t> pixels {};
    };

    bool CreateMainWindow(int showCommand);
    void CreateBlankDocument();
    void ClearHistory();
    void InvalidateCanvas();
    void InvalidatePageSurfaceCache(std::size_t pageIndex);
    void InvalidateAllPageSurfaceCaches();
    void InvalidateStrokeRegion(const Stroke& stroke, std::size_t pageIndex, bool smoothLiveStroke);
    void ResetView();
    void SetActiveTool(Tool tool);
    void PreviousPage();
    void NextPage();
    void SetCurrentPageIndex(std::size_t index);
    void FocusPage(std::size_t index);
    void ToggleBrushSettings();
    void Undo();
    void Redo();
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    [[nodiscard]] bool CanGoToPreviousPage() const;
    [[nodiscard]] bool CanGoToNextPage() const;
    void AddPaletteColor();
    void RemovePaletteColor();
    [[nodiscard]] bool ApplyHexColorFromClipboard();
    void UpdateWindowTitle();
    void BeginStroke(POINT cursor);
    void ExtendStroke(POINT cursor);
    void EndStroke();
    void BeginErase(POINT cursor);
    void ExtendErase(POINT cursor);
    [[nodiscard]] bool EraseAt(POINT cursor);
    void BeginPan(POINT cursor, bool fromMiddleButton);
    void UpdatePan(POINT cursor);
    void EndPan();
    void UpdateCaptureState();
    void ExecuteCommand(unsigned int commandId);
    [[nodiscard]] bool HandleToolbarClick(POINT cursor);
    [[nodiscard]] bool HandleSettingsClick(POINT cursor);
    void UpdateActiveSlider(POINT cursor);
    [[nodiscard]] bool IsCursorInsideSettingsPanel(POINT cursor) const;
    void PanBy(float dx, float dy);
    void ZoomAt(int delta, POINT cursor);
    void ChangeBrushSize(float delta);
    void CycleColor();
    void SetColorIndex(std::size_t index);
    void OpenPdf();
    void EnsureVisiblePageRasterQuality();
    void EnsurePageRasterQuality(const PageView& pageView);

    [[nodiscard]] bool IsCursorInsidePage(POINT cursor) const;
    [[nodiscard]] std::optional<PageView> HitTestPage(POINT cursor) const;
    [[nodiscard]] Vec2 ScreenToPage(POINT cursor, std::size_t* pageIndex = nullptr) const;
    [[nodiscard]] POINT PageToScreen(std::size_t pageIndex, Vec2 point) const;
    [[nodiscard]] PageView CalculatePageView(RECT clientRect, std::size_t pageIndex) const;
    [[nodiscard]] std::vector<PageView> CalculatePageViews(RECT clientRect) const;
    [[nodiscard]] float DesiredRenderScale(const PageView& pageView) const;
    [[nodiscard]] COLORREF ToColorRef(std::uint32_t argb) const;
    [[nodiscard]] std::uint32_t ComposeColor(unsigned char red, unsigned char green, unsigned char blue) const;
    [[nodiscard]] unsigned char GetColorChannel(std::uint32_t color, int shift) const;
    [[nodiscard]] std::wstring CurrentWindowTitle() const;
    [[nodiscard]] std::wstring PageIndicatorLabel() const;

    void Paint();
    void PaintToolbar(HDC hdc, RECT clientRect) const;
    void PaintCanvas(HDC hdc, RECT clientRect);
    void PaintPage(HDC hdc, const PageView& pageView);
    void PaintSettingsPanel(HDC hdc, RECT clientRect) const;
    void PaintStroke(HDC hdc, const Stroke& stroke, const PageView& pageView, bool smoothLiveStroke) const;
    void EnsurePageSurfaceCache(const PageView& pageView);

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Document document_ = Document::CreateBlank();
    PdfiumBackend pdfBackend_ {};

    float zoom_ = 1.0f;
    float panX_ = 0.0f;
    float panY_ = 0.0f;
    float brushWidth_ = 3.0f;
    Tool activeTool_ = Tool::Draw;
    std::size_t colorIndex_ = 0;
    bool isDrawing_ = false;
    bool isErasing_ = false;
    bool isPanning_ = false;
    bool panFromMiddleButton_ = false;
    bool settingsOpen_ = false;
    std::size_t settingsPaletteIndex_ = 0;
    SliderTarget activeSlider_ = SliderTarget::None;
    std::optional<Stroke> activeStroke_ {};
    std::optional<std::size_t> activeStrokePageIndex_ {};
    std::vector<HistoryAction> undoActions_ {};
    std::vector<HistoryAction> redoActions_ {};
    POINT lastPanCursor_ {};
    ULONG_PTR gdiplusToken_ = 0;
    float inputSpacing_ = 0.85f;
    float inputSmoothing_ = 0.3f;
    float strokeSmoothing_ = 0.55f;
    std::vector<PageSurfaceCache> pageSurfaceCaches_ {};

    std::vector<std::uint32_t> palette_ {
        0xFF1F2937,
        0xFF2563EB,
        0xFFDC2626,
        0xFF059669,
        0xFFF59E0B,
    };

    static constexpr int kToolbarHeight = 54;
    static constexpr int kSettingsPanelWidth = 320;
    static constexpr int kPageGap = 26;
    static constexpr float kMinZoom = 0.35f;
    static constexpr float kMaxZoom = 4.0f;
};

}  // namespace ctrainer
