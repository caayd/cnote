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
    struct PageView {
        RECT rect {};
        float scale = 1.0f;
        bool valid = false;
    };

    bool CreateMainWindow(int showCommand);
    void CreateBlankDocument();
    void ResetView();
    void BeginStroke(POINT cursor);
    void ExtendStroke(POINT cursor);
    void EndStroke();
    void BeginPan(POINT cursor);
    void UpdatePan(POINT cursor);
    void EndPan();
    void UpdateCaptureState();
    void PanBy(float dx, float dy);
    void ZoomAt(int delta, POINT cursor);
    void ChangeBrushSize(float delta);
    void CycleColor();
    void SetColorIndex(std::size_t index);
    void OpenPdf();

    [[nodiscard]] bool IsCursorInsidePage(POINT cursor) const;
    [[nodiscard]] Vec2 ScreenToPage(POINT cursor) const;
    [[nodiscard]] POINT PageToScreen(Vec2 point) const;
    [[nodiscard]] PageView CalculatePageView(RECT clientRect) const;
    [[nodiscard]] COLORREF ToColorRef(std::uint32_t argb) const;
    [[nodiscard]] std::wstring BuildStatusLine() const;

    void Paint();
    void PaintToolbar(HDC hdc, RECT clientRect) const;
    void PaintCanvas(HDC hdc, RECT clientRect) const;
    void PaintPage(HDC hdc, const PageView& pageView) const;
    void PaintPlaceholderContent(HDC hdc, const PageView& pageView) const;
    void PaintStroke(HDC hdc, const Stroke& stroke, const PageView& pageView) const;

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Document document_ = Document::CreateBlank();
    StubPdfBackend pdfBackend_ {};

    float zoom_ = 1.0f;
    float panX_ = 0.0f;
    float panY_ = 0.0f;
    float brushWidth_ = 3.0f;
    std::size_t colorIndex_ = 0;
    bool isDrawing_ = false;
    bool isPanning_ = false;
    std::optional<Stroke> activeStroke_ {};
    POINT lastPanCursor_ {};
    ULONG_PTR gdiplusToken_ = 0;

    std::vector<std::uint32_t> palette_ {
        0xFF1F2937,
        0xFF2563EB,
        0xFFDC2626,
        0xFF059669,
        0xFFF59E0B,
    };

    static constexpr int kToolbarHeight = 72;
    static constexpr float kMinZoom = 0.35f;
    static constexpr float kMaxZoom = 4.0f;
    static constexpr float kInputSpacing = 0.85f;
    static constexpr float kInputSmoothing = 0.3f;
};

}  // namespace ctrainer
