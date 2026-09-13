#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <QPointF>
#include <QRectF>
#include <QWidget>

#include "Document.h"
#include "render/PageRenderer.h"
#include "ui/CanvasController.h"

class QTimer;

namespace cnote::ui {

// One page's placement in widget (logical) coordinates.  Document coordinates
// stay page units; `scale` converts page units to logical pixels.
struct CanvasPageView {
    std::size_t pageIndex = 0;
    QRectF rect {};
    float scale = 1.0f;
    bool valid = false;
};

// QWidget canvas for the Qt migration (plan steps 3.1/3.2/3.3).
//
// Owns per-view viewport state and transient interaction state, and the raster
// caches derived from them.  Committed-object drawing is delegated to
// render::PageRenderer and text layout to text::TextLayoutQt; this widget never
// re-implements object primitives.  Mutations go through CanvasController, so
// the widget has no dependency on App, HWND/HDC or the document owner.
//
// Geometry follows the reference Windows frontend: 96-DPI reference scale
// (Qt logical pixels), fit scale = (canvasWidth - 120) / widestPage, 26 px page
// gap, 40 px top inset, pan clamping slack 20% clamped to [96, 260]/[96, 320],
// zoom range [0.35, 4.0] with 1.1/0.9 wheel steps anchored at the cursor.
class CanvasWidget : public QWidget {
    Q_OBJECT

public:
    static constexpr float kMinZoom = 0.35f;
    static constexpr float kMaxZoom = 4.0f;
    static constexpr float kWheelZoomInFactor = 1.1f;
    static constexpr float kWheelZoomOutFactor = 0.9f;
    static constexpr int kPageGap = 26;
    static constexpr int kTopInset = 40;
    static constexpr int kZoomReferenceInset = 120;
    static constexpr int kWheelScrollLines = 3;
    static constexpr int kWheelScrollUnitPixels = 18;
    static constexpr int kZoomSessionDelayMs = 100;

    explicit CanvasWidget(CanvasController& controller, QWidget* parent = nullptr);
    ~CanvasWidget() override;

    // --- view state -------------------------------------------------------
    [[nodiscard]] float Zoom() const { return zoom_; }
    void SetZoom(float zoom, std::optional<QPointF> anchor = std::nullopt);
    void ZoomBySteps(int steps, const QPointF& anchor);
    void SetZoomAt(const QPointF& anchor, float zoom);
    // Reference fit: zoom 1.0 makes the widest page fit the canvas minus the
    // zoom reference inset (App::ApplyZoomPreset of its fit preset).
    void FitToWindow();
    void ResetView();

    void PanBy(const QPointF& delta);
    void CenterOnPage(std::size_t pageIndex);
    void FocusPage(std::size_t pageIndex) { CenterOnPage(pageIndex); }

    // --- geometry ---------------------------------------------------------
    [[nodiscard]] std::vector<CanvasPageView> CalculatePageViews() const;
    [[nodiscard]] CanvasPageView CalculatePageView(std::size_t pageIndex) const;
    [[nodiscard]] std::optional<CanvasPageView> HitTestPage(const QPointF& logical) const;
    [[nodiscard]] Vec2 ScreenToPage(const CanvasPageView& view, const QPointF& logical, bool clampToPage) const;
    [[nodiscard]] QPointF PageToScreen(std::size_t pageIndex, Vec2 point) const;

    // --- document changes -------------------------------------------------
    // Drops raster caches.  Called automatically when the controller's revision
    // changes; the shell may also call it directly after loading a document.
    void NotifyDocumentChanged();

    // --- interaction (also the programmatic path used by tests/harness) ----
    void BeginStrokeAt(const QPointF& logical);
    bool ExtendStrokeTo(const QPointF& logical);
    void EndStroke();
    void CancelStroke();
    [[nodiscard]] bool HasActiveStroke() const;

    // Diagnostics used by the smoke harness/timings.
    [[nodiscard]] std::size_t CachedSurfaceCount() const;
    [[nodiscard]] std::size_t LastPaintPageCount() const { return lastPaintPageCount_; }

signals:
    void ViewportChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct SurfaceEntry {
        float scale = 0.0f;
        float devicePixelRatio = 1.0f;
        render::PageSurfaceCache cache {};
    };
    struct PageSurfaces {
        // Newest first; bounded to kScalesPerPage entries.
        std::vector<SurfaceEntry> entries;
    };

    [[nodiscard]] render::PageRenderParams RenderParamsFor(const CanvasPageView& view) const;
    const render::PageSurfaceCache* FindSurface(std::size_t pageIndex, float scale, float devicePixelRatio) const;
    const render::PageSurfaceCache* NewestSurface(std::size_t pageIndex) const;
    const render::PageSurfaceCache& EnsureSurface(const CanvasPageView& view, const Page& page);
    void PaintPageContents(QPainter& painter, const Page& page, const CanvasPageView& view);
    void PruneSurfaces(const std::vector<std::size_t>& visiblePages);

    void BeginZoomSession(const QPointF& anchor);
    void EndZoomSession();
    void NotifyViewportChanged();
    void ClampViewportPan();
    [[nodiscard]] QRectF VisibleCanvasRect() const;

    CanvasController& controller_;
    float zoom_ = 1.0f;
    float panX_ = 0.0f;
    float panY_ = 0.0f;

    // Zoom session: while the wheel zoom is in flight, pages that have no
    // surface at the exact scale reuse their newest raster (the zoom snapshot)
    // instead of re-rendering; the settle timer re-renders exactly once.
    bool zoomSessionActive_ = false;
    bool zoomAnchorValid_ = false;
    std::size_t zoomAnchorPageIndex_ = 0;
    Vec2 zoomAnchorPage_ {};
    QPointF zoomAnchorCursor_ {};
    QTimer* zoomSettleTimer_ = nullptr;

    // Pan gesture state.
    bool panning_ = false;
    QPointF panLastPosition_ {};

    // Stroke gesture state.
    bool drawing_ = false;
    std::optional<std::size_t> strokePageIndex_;
    QPointF strokeLastPosition_ {};

    std::unordered_map<std::size_t, PageSurfaces> surfaces_;
    float cachedDevicePixelRatio_ = 0.0f;
    std::uint64_t cachedRevision_ = 0;
    std::size_t lastPaintPageCount_ = 0;

    static constexpr std::size_t kScalesPerPage = 3;
};

}  // namespace cnote::ui
