#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <QElapsedTimer>
#include <QCursor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "Document.h"
#include "latex/LatexCompletion.h"
#include "render/PageRenderer.h"
#include "settings/SettingsStoreQt.h"
#include "ui/CanvasController.h"
#include "ui/InteractionRouter.h"
#include "ui/PrecisionInputSink.h"
#include "ui/SelectionMath.h"

class QListView;
class QPlainTextEdit;
class QStringListModel;
class QTextEdit;
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

// QWidget canvas for the Qt migration (plan steps 3.1/3.2/3.3, 5.5/5.6).
//
// Owns per-view viewport state, transient interaction state, the raster caches
// derived from them, and the overlaid editors (QTextEdit for styled notes,
// QPlainTextEdit plus a QListView completion popup for LaTeX source).  It is
// only a view: committed objects come from render::PageRenderer, text layout
// from text::TextLayoutQt, edit decisions from text::TextEditingQt and every
// document mutation goes through CanvasController.
class CanvasWidget : public QWidget, public EditContentSource, public InteractionHost, public PrecisionInputSink {
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
    static constexpr int kTextEditorMinWidth = 360;

    explicit CanvasWidget(CanvasController& controller, QWidget* parent = nullptr);
    ~CanvasWidget() override;

    // --- view state -------------------------------------------------------
    [[nodiscard]] float Zoom() const { return zoom_; }
    void SetZoom(float zoom, std::optional<QPointF> anchor = std::nullopt);
    void ZoomBySteps(int steps, const QPointF& anchor);
    // Reference fit: zoom 1.0 makes the widest page fit the canvas minus the
    // zoom reference inset (App::ApplyZoomPreset of its fit preset).
    void FitToWindow();
    void ResetView();

    void PanBy(const QPointF& delta);
    void CenterOnPage(std::size_t pageIndex);

    // --- geometry ---------------------------------------------------------
    [[nodiscard]] std::vector<CanvasPageView> CalculatePageViews() const;
    [[nodiscard]] CanvasPageView CalculatePageView(std::size_t pageIndex) const;
    [[nodiscard]] std::optional<CanvasPageView> HitTestPageView(const QPointF& logical) const;
    [[nodiscard]] Vec2 ScreenToPage(const CanvasPageView& view, const QPointF& logical, bool clampToPage) const;
    [[nodiscard]] QPointF PageToScreen(std::size_t pageIndex, Vec2 point) const;

    // --- document changes -------------------------------------------------
    void NotifyDocumentChanged();
    void SetPdfBackgroundProvider(render::PdfBackgroundProvider* provider);

    // --- interaction (plan step 5.1: PointerSample-shaped entry points) ----
    // The widget is only a host for the router: it converts Qt events into
    // PointerSample/WheelSample, owns the viewport and transient state, and
    // performs the effects (geometry, caches, model mutation through the
    // controller).  The state machine itself lives in ui::InteractionRouter.
    void HandlePointerPress(const PointerSample& sample);
    void HandlePointerMove(const PointerSample& sample);
    void HandlePointerRelease(const PointerSample& sample);
    void HandlePointerCancel(CancelReason reason);
    // Sets the tool and shows its cursor: only the canvas can build the
    // configured drawing cursor (settings + device pixel ratio), so the cursor
    // call belongs here rather than in the router (MessageLoop.cpp:669-678
    // re-derived it per WM_SETCURSOR in the reference).
    void SetTool(Tool tool);
    void SetSelectionMode(SelectionMode mode) { router_->SetSelectionMode(mode); }
    void SetInteractionModifiers(InteractionModifiers modifiers) { router_->SetModifiers(modifiers); }
    [[nodiscard]] InteractionRouter& Router() { return *router_; }
    [[nodiscard]] InteractionKind ActiveInteraction() const { return router_ != nullptr ? router_->ActiveInteraction() : InteractionKind::None; }

    // --- cursor ---------------------------------------------------------
    // Draw-cursor appearance from the application settings (draw_cursor_kind /
    // draw_cursor_size, App::DrawCursorForCurrentSize).  Dot draws the reference
    // disc of this logical diameter, filled with the active palette colour
    // (argbColor), in the widget's device pixels; PrecisionCrosshair uses the
    // system crosshair.  The canvas rebuilds the pixmap whenever the setting,
    // its colour or its device pixel ratio changes and re-applies it while the
    // drawing tool is active.
    void SetDrawCursorSettings(settings::DrawCursorKind kind, float logicalPixels, std::uint32_t argbColor);
    // Eraser brush size from the application settings (eraseBrushSize,
    // App::EraserRadiusPixels App.cpp:7426-7428).  It sizes both the square
    // erase cursor and the erase hit-test radius.
    void SetEraseBrushSize(float brushSize);
    // Builds the sample the Qt event handlers feed to the router.
    [[nodiscard]] PointerSample SampleFromEvent(const QMouseEvent& event);

    // ui::InteractionHost ---------------------------------------------------
    [[nodiscard]] std::optional<PageHit> HitTestPage(const QPointF& logical) const override;
    [[nodiscard]] Vec2 ScreenToPage(const QPointF& logical, bool clampToPage) const override;
    [[nodiscard]] bool IsPointInCanvas(const QPointF& logical) const override;
    [[nodiscard]] bool HasSelection() const override;
    [[nodiscard]] bool CursorInteractsWithSelection(const QPointF& logical) const override;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> HitTestText(const QPointF& logical) const override;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> HitTestLatex(const QPointF& logical) const override;
    [[nodiscard]] std::optional<std::size_t> PageWithPdfTextAt(const QPointF& logical) const override;
    [[nodiscard]] std::optional<SelectionHit> HitTestSelectionObject(const QPointF& logical, std::size_t pageIndex) const override;
    [[nodiscard]] std::optional<TransformRequest> HitTestSelectionHandle(const QPointF& logical) const override;
    [[nodiscard]] std::vector<ObjectRef> EraseTargetsAt(std::size_t pageIndex, Vec2 start, Vec2 end) const override;
    void ZoomByWheelSteps(int steps, const QPointF& anchor) override;
    void ScrollBy(const QPointF& logicalDelta) override;
    void SetViewPage(std::size_t pageIndex) override;
    // Cursor the interaction asks for.  CursorKind::Draw shows the configured
    // drawing cursor (dot or precision crosshair, see SetDrawCursorSettings);
    // CursorKind::Arrow restores the plain default arrow.
    void SetCursor(CursorKind cursor) override;
    void InvalidateCanvas() override;
    void SetPointerCapture(bool captured) override;
    void BeginStroke(std::size_t pageIndex, Vec2 pagePoint, bool dotted) override;
    void SetStrokeDotted(bool dotted) override;
    void AppendStrokePoint(Vec2 pagePoint, bool dotted) override;
    void ReplaceStrokePoints(const std::vector<Vec2>& points, bool dotted) override;
    void CommitStroke() override;
    void DiscardStroke() override;
    void ClearSelection() override;
    void UpdateSelectionOverlay(const QPointF& anchor, const QPointF& current, bool lasso, const std::vector<QPointF>& lassoPoints) override;
    void CommitSelection(const QPointF& anchor, const QPointF& current, bool lasso, bool boxOverride, bool click) override;
    void AddObjectToSelection(ObjectRef object) override;
    void BeginMoveSelection(std::size_t pageIndex) override;
    void UpdateMoveSelection(const QPointF& logicalDelta) override;
    void CommitMoveSelection() override;
    void BeginTransform(const TransformRequest& request) override;
    void UpdateTransform(const QPointF& logical, Qt::KeyboardModifiers modifiers) override;
    void CommitTransform() override;
    void UpdatePendingErase(const std::vector<ObjectRef>& pending) override;
    void CommitErase(const std::vector<ObjectRef>& pending) override;
    void BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) override;
    void CreateTextAt(std::size_t pageIndex, Vec2 pagePoint) override;
    void BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex) override;
    void CreateLatexAt(std::size_t pageIndex, Vec2 pagePoint) override;

    // ui::PrecisionInputSink -------------------------------------------------
    [[nodiscard]] PrecisionInputRoute RoutePrecisionInput(const QPoint& globalPosition) const override;
    void BeginPrecisionGesture() override;
    void ZoomAtAnchor(qreal cumulativeScale, const QPoint& globalAnchor) override;
    void PanBy(qreal deltaX, qreal deltaY) override;
    void EndPrecisionGesture() override;

    // --- programmatic stroke path (used by tests/harness) -----------------
    void BeginStrokeAt(const QPointF& logical);
    bool ExtendStrokeTo(const QPointF& logical);
    void EndStroke();
    void CancelStroke();

    // --- editing overlay --------------------------------------------------
    // Begins editing the topmost text or LaTeX object under `logical`; when no
    // object is hit, nothing happens (the click starts a stroke instead).
    bool BeginEditAt(const QPointF& logical);
    // Shows the overlay for an edit the controller already started (e.g. a new
    // object created from a shell command), or hides it when none is active.
    void SyncEditOverlay();
    // Commits (Ctrl+Enter) or cancels (Escape) the active edit.
    void CommitActiveEdit();
    void CancelActiveEdit();
    [[nodiscard]] bool EditOverlayActive() const;

    [[nodiscard]] QTextEdit* TextEditor() const { return textEditor_; }
    [[nodiscard]] QPlainTextEdit* LatexEditor() const { return latexEditor_; }
    // Applies a style to the text overlay's current selection.
    void ApplyTextFormatToSelection(const PageTextStyle& style);

    void SetLatexSource(const QString& source, int cursorPosition);
    [[nodiscard]] QString LatexSource() const;
    [[nodiscard]] bool LatexPopupVisible() const;
    [[nodiscard]] QStringList LatexPopupItems() const;
    [[nodiscard]] int LatexPopupRow() const;
    void SetLatexPopupRow(int row);
    void RefreshLatexPopup();
    bool AcceptLatexSuggestion();

    // ui::EditContentSource (overlay content for manager-driven commits).
    [[nodiscard]] PageText CaptureEditedText() const override;
    [[nodiscard]] QString CaptureEditedLatexSource() const override;

    // Diagnostics used by the gate harness/timings.
    // Page-surface re-renders (a signature change) and scale/DPR cache misses.
    [[nodiscard]] std::uint64_t SurfaceRenderCount() const { return surfaceRenders_; }
    [[nodiscard]] float ViewportPanX() const { return panX_; }
    [[nodiscard]] float ViewportPanY() const { return panY_; }
    [[nodiscard]] float ZoomReferenceWidth() const { return ZoomReferenceCanvasWidth(); }
    [[nodiscard]] std::uint64_t SurfaceScaleMissCount() const { return surfaceScaleMisses_; }
    [[nodiscard]] std::size_t CachedSurfaceCount() const;
    [[nodiscard]] std::size_t LastPaintPageCount() const { return lastPaintPageCount_; }

signals:
    void ViewportChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // The cursor is re-applied when the pointer crosses the widget boundary: a
    // child (the text/LaTeX overlay) or the window chrome can leave the canvas
    // without its configured cursor, and entering is where the reference's
    // WM_SETCURSOR ran for the new position.
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    // Mutable cache lookup for incremental stroke commits (same key as FindSurface).
    render::PageSurfaceCache* MutableSurface(std::size_t pageIndex, float scale, float devicePixelRatio);
    const render::PageSurfaceCache* FindSurface(std::size_t pageIndex, float scale, float devicePixelRatio) const;
    const render::PageSurfaceCache* NewestSurface(std::size_t pageIndex) const;
    const render::PageSurfaceCache& EnsureSurface(const CanvasPageView& view, const Page& page);
    void PaintPageContents(QPainter& painter, const Page& page, const CanvasPageView& view);
    void PruneSurfaces(const std::vector<std::size_t>& visiblePages);

    // Wheel-notch smooth scroll: notches accumulate onto scrollTarget_ and a
    // frame-rate-independent exponential chase moves the pan toward it, so a
    // fast flick travels its full distance instead of capping at one notch
    // per animation restart.
    void StartSmoothScroll(const QPointF& delta);
    void StopSmoothScroll();
    void UpdateSmoothScroll();

    // Cursor plumbing.  ApplyCursor() maps the kind the interaction asked for to
    // a real QCursor; the drawing and eraser cursors' pixmaps are built from the
    // settings at the widget's device pixel ratio and cached until either
    // changes.
    void ApplyCursor();
    [[nodiscard]] QCursor BuildDrawCursor() const;
    [[nodiscard]] QCursor BuildEraserCursor() const;

    // Selection / erase rendering helpers.
    [[nodiscard]] std::optional<SelectionBounds> SelectionBoundsForPage(std::size_t pageIndex) const;
    // Selection frame geometry in logical pixels, shared by the overlay painter
    // and the handle hit test.  Eight resize handle centres in the order the
    // overlay draws them (four corners, then the four edge midpoints) and the
    // rotate handle above the top edge midpoint.
    [[nodiscard]] static std::array<QPointF, 8> SelectionHandlePositions(const QRectF& frame);
    [[nodiscard]] static QPointF SelectionRotateHandlePosition(const QRectF& frame);
    [[nodiscard]] std::optional<ObjectRef> TopmostObjectAt(const QPointF& logical, bool includeStrokes) const;
    [[nodiscard]] bool EditOverlayOwnsPoint(const QPointF& logical) const;
    void PaintSelectionOverlay(QPainter& painter);
    // Per-kind pending-erase index sets for one page; the renderer fades them
    // (App.cpp:5572-5628 pendingErase*Indices_).
    [[nodiscard]] render::PendingEraseIndices PendingEraseFor(std::size_t pageIndex) const;

    void BeginZoomSession(const QPointF& anchor);
    void EndZoomSession();
    void NotifyViewportChanged();
    void ClampViewportPan();
    [[nodiscard]] QRectF VisibleCanvasRect() const;
    // Reference MaximizedCanvasWidthForZoom (Viewport.cpp:36-52): the fit scale
    // is derived from the monitor work area, not from the window/canvas width.
    [[nodiscard]] float ZoomReferenceCanvasWidth() const;

    // Overlay plumbing.
    void EnsureTextEditor();
    void EnsureLatexEditor();
    void ShowTextOverlay(const PageText& text);
    void ShowLatexOverlay(const PageLatex& object);
    void HideOverlays();
    void PositionOverlays();
    void SyncLatexPopup();
    bool HandleEditorKey(QObject* watched, QKeyEvent* event);
    bool HandleLatexPairKey(QPlainTextEdit& editor, const QString& text);

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

    // Constructed in the constructor body: the router's constructor queries the
    // host, so it must not run while the widget is still under construction.
    std::unique_ptr<InteractionRouter> router_;
    bool pointerCaptured_ = false;
    QPointF lastPointerPosition_ {};

    // Wheel-notch scroll chase (accumulating target, exponential approach).
    QTimer* scrollTimer_ = nullptr;
    QElapsedTimer scrollClock_;
    QPointF scrollTarget_ {};
    bool scrolling_ = false;
    qreal precisionStartZoom_ = 1.0;

    // Cursor: the kind the current interaction/tool wants, the drawing cursor
    // settings and the pixmap caches keyed by device pixel ratio.  The draw
    // cursor's colour is the active palette slot (App::DrawCursorForCurrentSize
    // App.cpp:7481 falls back to opaque black when the palette is empty).
    CursorKind cursorKind_ = CursorKind::Arrow;
    settings::DrawCursorKind drawCursorKind_ = settings::DrawCursorKind::Dot;
    float drawCursorSize_ = 8.0f;
    std::uint32_t drawCursorColor_ = 0xFF000000U;
    std::optional<QCursor> drawCursor_ {};
    qreal drawCursorDevicePixelRatio_ = 0.0;
    // Eraser brush size in logical pixels, defaulting as settings::Settings
    // does (SettingsStoreQt.h:84).
    float eraseBrushSize_ = 3.0f;
    std::optional<QCursor> eraserCursor_ {};
    qreal eraserCursorDevicePixelRatio_ = 0.0;

    // Transient selection/erase overlay state (never part of cache signatures).
    bool selectionOverlayActive_ = false;
    QPointF selectionAnchor_ {};
    QPointF selectionCurrent_ {};
    bool selectionOverlayLasso_ = false;
    std::vector<QPointF> lassoPoints_;
    std::vector<ObjectRef> pendingErase_;

    // Transform gesture state: the handle being dragged, the selection bounds
    // captured at BeginTransform and the logical press position (rotation
    // measures the pointer's start angle from it).  Cleared on commit.
    std::optional<TransformHandle> transformHandle_ {};
    SelectionBounds transformFrom_ {};
    QPointF transformPressLogical_ {};
    // Position of the last HitTestSelectionHandle probe: BeginTransform runs
    // right after it and captures the press point this way.
    mutable QPointF lastHandleProbeLogical_ {};

    QTextEdit* textEditor_ = nullptr;
    QPlainTextEdit* latexEditor_ = nullptr;
    QListView* latexPopup_ = nullptr;
    QStringListModel* latexPopupModel_ = nullptr;
    std::vector<latex::CompletionItem> latexSuggestions_ {};
    std::optional<std::size_t> overlayPageIndex_ {};
    std::optional<std::size_t> overlayTextIndex_ {};
    std::optional<std::size_t> overlayLatexIndex_ {};
    // View scale the text editor's fonts were last formatted at; the editor is
    // the live preview, so its glyphs must match the canvas render size.
    float textEditScale_ = 1.0f;

    std::unordered_map<std::size_t, PageSurfaces> surfaces_;
    render::PdfBackgroundProvider* pdfBackground_ = nullptr;
    float cachedDevicePixelRatio_ = 0.0f;
    std::size_t lastPaintPageCount_ = 0;
    std::uint64_t surfaceRenders_ = 0;
    std::uint64_t surfaceScaleMisses_ = 0;

    static constexpr std::size_t kScalesPerPage = 3;
};

}  // namespace cnote::ui
