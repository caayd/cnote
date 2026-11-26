#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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

#include "cnote_ui_export.h"
#include "Document.h"
#include "latex/LatexCompletion.h"
#include "latex/LatexEngine.h"
#include "render/PageRenderer.h"
#include "settings/SettingsStoreQt.h"
#include "ui/CanvasController.h"
#include "ui/EraserTrail.h"
#include "ui/InteractionRouter.h"
#include "ui/InlineMathTextEdit.h"
#include "ui/PrecisionInputSink.h"
#include "selection/StrokeSmoothing.h"
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
class CNOTE_UI_EXPORT CanvasWidget : public QWidget, public EditContentSource, public InteractionHost, public PrecisionInputSink {
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
    // App::EraserRadiusPixels App.cpp:7426-7428).  It sizes both the round
    // erase cursor and the erase hit-test radius.
    void SetEraseBrushSize(float brushSize);
    // Leaves the canvas around the pages transparent instead of painting the
    // reference backdrop colour, so the window's backdrop shows through
    // (settings canvas_backdrop, ui::ApplyWindowBackdrop).
    void SetTransparentBackdrop(bool transparent);
    [[nodiscard]] bool TransparentBackdrop() const { return transparentBackdrop_; }
    // Builds the sample the Qt event handlers feed to the router.
    [[nodiscard]] PointerSample SampleFromEvent(const QMouseEvent& event);

    // ui::InteractionHost ---------------------------------------------------
    [[nodiscard]] std::optional<PageHit> HitTestPage(const QPointF& logical) const override;
    [[nodiscard]] Vec2 ScreenToPage(const QPointF& logical, bool clampToPage) const override;
    [[nodiscard]] Vec2 ScreenToPageOnPage(std::size_t pageIndex, const QPointF& logical, bool clampToPage) const override;
    [[nodiscard]] bool IsPointInCanvas(const QPointF& logical) const override;
    [[nodiscard]] bool HasSelection() const override;
    [[nodiscard]] bool CursorInteractsWithSelection(const QPointF& logical) const override;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> HitTestText(const QPointF& logical) const override;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> HitTestLatex(const QPointF& logical) const override;
    [[nodiscard]] std::optional<std::size_t> PageWithPdfTextAt(const QPointF& logical) const override;
    [[nodiscard]] std::optional<SelectionHit> HitTestSelectionObject(const QPointF& logical, std::size_t pageIndex) const override;
    [[nodiscard]] std::optional<TransformRequest> HitTestSelectionHandle(const QPointF& logical) const override;
    [[nodiscard]] std::vector<ObjectRef> EraseTargetsAt(std::size_t pageIndex, Vec2 start, Vec2 end) const override;
    [[nodiscard]] std::vector<ObjectRef> ScratchOutTargets(std::size_t pageIndex, const ScratchOut& scratch) const override;
    void ZoomByWheelSteps(int steps, const QPointF& anchor) override;
    void ScrollBy(const QPointF& logicalDelta) override;
    // Cursor the interaction asks for.  CursorKind::Draw shows the configured
    // drawing cursor (dot or precision crosshair, see SetDrawCursorSettings);
    // CursorKind::Arrow restores the plain default arrow.
    void SetCursor(CursorKind cursor) override;
    void InvalidateCanvas() override;
    void SetPointerCapture(bool captured) override;
    void BeginStroke(std::size_t pageIndex, Vec2 pagePoint, bool dotted, StrokeSampling sampling,
        std::optional<float> pressure) override;
    void SetStrokeDotted(bool dotted) override;
    void AppendStrokePoint(Vec2 pagePoint, bool dotted, std::optional<float> pressure) override;
    void ReplaceStrokePoints(const std::vector<Vec2>& points, bool dotted) override;
    void CommitStroke() override;
    void DiscardStroke() override;
    void ClearSelection() override;
    void UpdateSelectionOverlay(const QPointF& anchor, const QPointF& current, bool lasso, const std::vector<QPointF>& lassoPoints) override;
    void CancelSelectionOverlay() override;
    void CommitSelection(const QPointF& anchor, const QPointF& current, bool lasso, bool boxOverride, bool click,
        bool additive) override;
    void AddObjectToSelection(ObjectRef object) override;
    void ToggleObjectInSelection(ObjectRef object) override;
    void BeginMoveSelection(std::size_t pageIndex) override;
    void BeginCloneMoveSelection(std::size_t pageIndex) override;
    void UpdateMoveSelection(const QPointF& logicalDelta) override;
    void CommitMoveSelection() override;
    void CancelMoveSelection() override;
    bool BeginTransform(const TransformRequest& request) override;
    void UpdateTransform(const QPointF& logical, Qt::KeyboardModifiers modifiers) override;
    void CommitTransform() override;
    void CancelTransform() override;
    void UpdatePendingErase(const std::vector<ObjectRef>& pending) override;
    void CommitErase(const std::vector<ObjectRef>& pending) override;
    void CommitScratchOut(const std::vector<ObjectRef>& targets) override;
    void BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) override;
    void CreateTextAt(std::size_t pageIndex, Vec2 pagePoint, float width) override;
    void UpdateTextCreatePreview(std::optional<TextBoxPreview> preview) override;
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
    // Commits (Ctrl+Enter, Escape) or cancels the active edit.
    void CommitActiveEdit();
    void CancelActiveEdit();
    // The keyboard commit (Escape, Ctrl+Enter): commits and selects the object.
    void CommitActiveEditFromKeyboard();
    [[nodiscard]] bool EditOverlayActive() const;

    [[nodiscard]] QTextEdit* TextEditor() const { return textEditor_; }
    // Page-to-screen scale the live text editor was seeded with (the page
    // view's zoom at overlay open); the shell converts between the model's
    // page-unit font sizes and the editor's point sizes with it.
    [[nodiscard]] float TextEditScale() const { return textEditScale_; }
    [[nodiscard]] QPlainTextEdit* LatexEditor() const { return latexEditor_; }
    // The widest a text typed or pasted on `pageIndex` grows to before it
    // wraps (page units), for the canvas as it is now: text::MaxAutoTextWidth.
    [[nodiscard]] float MaxAutoTextWidth(std::size_t pageIndex) const;
    // Aligns the text under edit (the whole object, as the model stores it).
    void SetTextEditAlignment(TextAlignment alignment);
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
    // Logical-pixel area of the last repaint region queued by the
    // transient-state path (diagnostics for the dirty-rect work: full-widget
    // repaints report the window area).
    [[nodiscard]] std::uint64_t LastTransientRepaintArea() const { return lastTransientRepaintArea_; }

signals:
    void ViewportChanged();
    // Ctrl+B pressed in the text editor: the editor's bold command (reference
    // TextEditProc), which the shell applies like the format popup's toggle.
    // Raised from the editor's own event filter, installed when the editor is
    // created, so it works from the first keystroke of the first edit.
    void TextBoldShortcutPressed();
    // Escape or Ctrl+Enter committed an edit and left its object selected.
    void KeyboardCommitSelected();

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
    // Pen input: the same pointer routing as the mouse, plus pressure.
    // Accepting the event stops Qt from synthesizing the mouse event too.
    void tabletEvent(QTabletEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct SurfaceEntry {
        // Document::PageEpoch the raster was rendered/stamped for.  Page
        // indices shift on insert/remove, so entries also validate against
        // the current epoch before a lookup may reuse the raster.
        std::uint64_t pageEpoch = 0;
        float scale = 0.0f;
        float devicePixelRatio = 1.0f;
        render::PageSurfaceCache cache {};
    };
    struct PageSurfaces {
        // Newest first; bounded to kScalesPerPage entries.
        std::vector<SurfaceEntry> entries;
    };
    // Move/transform gesture overlay: while a page gesture is in flight the
    // selection is painted live on top of the committed raster, and a small
    // patch re-rendered once per gesture covers the region the selection
    // vacated (the cached surface still shows it at its original position).
    // `excluded` feeds the patch render (selection plus any editing
    // exclusions); `only` is the selection subset painted live.  Both are
    // sorted index lists captured at gesture begin.
    struct GestureItem {
        // Raster of the object rendered at scale * DPR, plus where its
        // top-left sits in page units.  `trackX/trackY` is the object's
        // position at gesture begin (stroke first point, otherwise x/y), so
        // the live offset is current - track.
        QImage image;
        float originX = 0.0f;
        float originY = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float trackX = 0.0f;
        float trackY = 0.0f;
        ObjectKind kind = ObjectKind::Stroke;
        std::size_t index = 0;
    };
    struct GestureSurface {
        std::size_t pageIndex = 0;
        render::ExcludedIndices excluded;
        render::ExcludedIndices only;
        // Vacated region in page units (selection bounds at gesture begin,
        // inflated for stroke widths) and its raster at scale * DPR.
        QRectF patchRegion;
        float scale = 0.0f;
        float devicePixelRatio = 0.0f;
        QImage patch;
        // Fallback when no committed raster exists to reuse (e.g. a DPR change
        // mid-gesture): rendered once with the selection excluded so later
        // moves cannot ghost.
        render::PageSurfaceCache fallback {};
        // Move gestures blit one cached raster per object (exact for pure
        // translation); transforms paint the subset live because scaling and
        // rotation change stroke widths and text layout.
        bool transform = false;
        std::vector<GestureItem> items;
        float itemsScale = 0.0f;
        float itemsDevicePixelRatio = 0.0f;
    };

    // Live-stroke ink layer: the pending stroke's stabilized prefix is
    // composited once into a raster covering the stroke's bounds (snapped to
    // the device-pixel grid), so a paint blits it and draws only the small
    // provisional tail instead of re-smoothing and re-rasterizing every point
    // per frame.  `smoothed` holds emitted smoothed points for the consumed
    // raw prefix; `consumedRaw`/`consumedLast`/`firstPoint` detect a replaced
    // point list (line/oval modes) and force a rebuild.
    struct LiveStrokeLayer {
        bool valid = false;
        std::size_t pageIndex = 0;
        float scale = 0.0f;
        float devicePixelRatio = 0.0f;
        StrokeStyle style {};
        float smoothing = 0.0f;
        bool pressure = false;         // pressure ink: composites filled outlines
        QRectF pageRect {};            // stroke bounds the image covers (page units)
        QImage image;                  // ARGB32_Premultiplied at scale * DPR
        std::vector<Vec2> smoothed;
        std::vector<float> smoothedPressures;  // parallel to `smoothed` (zeros without pressure)
        Vec2 lastEnd {};               // end of the last composited segment
        float lastEndPressure = 0.0f;
        std::size_t consumedRaw = 0;   // raw points folded into `smoothed`
        Vec2 consumedLast {};
        Vec2 firstPoint {};
        Vec2 rawLast {};
        IncrementalStrokeSmoother smoother;
    };

    // Keeps the live-stroke layer in sync with the pending stroke's stabilized
    // path (`path`, liveStroke_) and reports whether it applies (false:
    // caller falls back to PaintStrokePreview - dotted strokes, replaced
    // point lists mid-rebuild, allocation failure).
    bool SyncStrokeLayer(const Stroke& stroke, const InkPath& path, const CanvasPageView& view);
    void ResetStrokeLayer();
    // Draws the provisional tail (the smoothed segments that still change as
    // points arrive) on top of the layer at widget scale.
    void PaintStrokeTail(QPainter& painter, const Stroke& stroke, const InkPath& path, const CanvasPageView& view);

    [[nodiscard]] render::PageRenderParams RenderParamsFor(const CanvasPageView& view) const;
    const render::PageSurfaceCache* FindSurface(std::size_t pageIndex, float scale, float devicePixelRatio) const;
    // Prepares the gesture overlay for `pageIndex` (sorted selection index
    // lists, vacated-region patch, per-object rasters for moves) and clears
    // it on commit.
    void BeginGestureOverlay(std::size_t pageIndex, bool transform);
    const render::PageSurfaceCache* NewestSurface(std::size_t pageIndex) const;
    const render::PageSurfaceCache& EnsureSurface(const CanvasPageView& view, const Page& page);
    void PaintPageContents(QPainter& painter, const Page& page, const CanvasPageView& view);
    void EndGestureOverlay();
    // Widget-space dirty rect covering the selection frame plus handles.
    [[nodiscard]] QRect SelectionDirtyRect(std::size_t pageIndex) const;
    void PruneSurfaces(const std::vector<std::size_t>& visiblePages);

    // Wheel-notch smooth scroll: notches accumulate onto the target and a
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
    // Recomputes the wanted cursor from the live interaction/tool and the
    // PDF-text hover at `lastPointerPosition_`, then applies it.  The single
    // entry point keeps the hover flag from desynchronising from the tool.
    void RefreshCursor();
    [[nodiscard]] QCursor BuildDrawCursor() const;
    [[nodiscard]] QCursor BuildEraserCursor() const;

    // Selection / erase rendering helpers.
    [[nodiscard]] std::optional<SelectionBounds> SelectionBoundsForPage(std::size_t pageIndex) const;
    // Page-space corners (clockwise from the unrotated top-left) of every
    // selected object on the page; SelectionBoundsForPage is their union.
    [[nodiscard]] std::vector<std::array<Vec2, 4>> SelectedObjectOutlines(std::size_t pageIndex) const;
    // Selection frame geometry in logical pixels, shared by the overlay painter,
    // the dirty rect and the handle hit test: the frame padded around the
    // bounds, its four corner handle centres (top-left, top-right,
    // bottom-right, bottom-left) and the rotate handle above the top edge.
    [[nodiscard]] static QRectF SelectionFrameRect(const CanvasPageView& view, const SelectionBounds& bounds);
    [[nodiscard]] static std::array<QPointF, 4> SelectionHandlePositions(const QRectF& frame);
    [[nodiscard]] static QPointF SelectionRotateHandlePosition(const QRectF& frame);
    // The handle of `frame` under `logical`: rotate first, then the corners,
    // then the edge grips.
    [[nodiscard]] static std::optional<TransformHandle> SelectionHandleAt(const QRectF& frame, const QPointF& logical);
    [[nodiscard]] std::optional<ObjectRef> TopmostObjectAt(const QPointF& logical, bool includeStrokes) const;
    [[nodiscard]] bool EditOverlayOwnsPoint(const QPointF& logical) const;
    void PaintSelectionOverlay(QPainter& painter);
    // The text-tool drag's box: its logical rect, and the dashed outline.
    [[nodiscard]] QRectF TextCreatePreviewRect() const;
    void PaintTextCreatePreview(QPainter& painter);
    // Per-kind pending-erase index sets for one page; the renderer fades them
    // (App.cpp:5572-5628 pendingErase*Indices_).
    [[nodiscard]] render::PendingEraseIndices PendingEraseFor(std::size_t pageIndex) const;
    // Page-space bounds of one pending-erase candidate (stroke bounds include
    // the pen half-width), or nullopt when the ref is out of range.
    [[nodiscard]] std::optional<QRectF> PendingEraseObjectBounds(const Page& page, const ObjectRef& object) const;
    // Page-space union of a page's marked-object bounds: the region that must
    // be re-rendered for the faded overlay.
    [[nodiscard]] QRectF PendingErasePageRegion(std::size_t pageIndex) const;

    // Widget-space dirty rect covering a page-unit region of `pageIndex`.
    [[nodiscard]] QRect PageRegionDirtyRect(std::size_t pageIndex, const QRectF& pageRect) const;
    // Widget-space dirty rect covering every pending-erase object's bounds.
    [[nodiscard]] QRect PendingEraseDirtyRect() const;
    // Widget-space dirty rect covering the live marquee/lasso overlay.
    [[nodiscard]] QRect OverlayDirtyRect() const;
    // Records a transient repaint region and queues it; InvalidateCanvas then
    // repaints the accumulated region instead of the whole widget.
    void MarkTransientDirty(const QRect& logical);

    void BeginZoomSession(const QPointF& anchor);
    void EndZoomSession();
    void NotifyViewportChanged();
    // The focused (current) page is the one covering the largest share of the
    // visible canvas; ties go to the page whose centre is nearest the canvas
    // centre.  Leaves the current page alone while no page is visible.
    void SyncFocusedPage();
    void ClampViewportPan();
    [[nodiscard]] QRectF VisibleCanvasRect() const;
    // Reference MaximizedCanvasWidthForZoom (Viewport.cpp:36-52): the fit scale
    // is derived from the monitor work area, not from the window/canvas width.
    [[nodiscard]] float ZoomReferenceCanvasWidth() const;

    // Overlay plumbing.
    void EnsureTextEditor();
    void EnsureLatexEditor();
    void ShowTextOverlay(const PageText& text);
    // Wraps the editor like the committed text: at the object's width, or
    // not at all for one that grows with its content.
    void ApplyTextEditorWrap(const PageText& text);
    // textChanged: fits the edited object's box to the typed content (and
    // switches the editor to wrapping when it stops growing), then places it.
    void FitTextEditor();
    // Pans the edited text's box into the visible canvas (Excalidraw does so
    // when a typed text starts wrapping).
    void RevealEditedText(const PageText& text);
    void ShowLatexOverlay(const PageLatex& object);
    void HideOverlays();
    void PositionOverlays();
    // The LaTeX object being edited with the source box's current text: what
    // the live preview draws, so the source box is placed around it.
    [[nodiscard]] PageLatex LiveLatexPreview(const PageLatex& object) const;
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
    struct ZoomSession {
        bool anchorValid = false;
        std::size_t anchorPageIndex = 0;
        Vec2 anchorPage {};
        QPointF anchorCursor {};
    };
    std::optional<ZoomSession> zoomSession_;
    QTimer* zoomSettleTimer_ = nullptr;

    // Constructed in the constructor body: the router's constructor queries the
    // host, so it must not run while the widget is still under construction.
    std::unique_ptr<InteractionRouter> router_;
    bool pointerCaptured_ = false;
    QPointF lastPointerPosition_ {};
    // Mirrors router_->SetTool for the hover cursor (the router does not
    // expose its tool back to the host).
    Tool tool_ = Tool::Draw;

    // Wheel-notch scroll chase: notches accumulate onto the target and a
    // frame-rate-independent exponential approach moves the pan toward it.
    struct SmoothScroll {
        QPointF target {};
        QElapsedTimer clock {};
    };
    std::optional<SmoothScroll> smoothScroll_;
    QTimer* scrollTimer_ = nullptr;
    qreal precisionStartZoom_ = 1.0;

    // Cursor: the kind the current interaction/tool wants, the drawing cursor
    // settings and the pixmap caches keyed by device pixel ratio.  The draw
    // cursor's colour is the active palette slot (App::DrawCursorForCurrentSize
    // App.cpp:7481 falls back to opaque black when the palette is empty).
    CursorKind cursorKind_ = CursorKind::Arrow;
    settings::DrawCursorKind drawCursorKind_ = settings::DrawCursorKind::Dot;
    float drawCursorSize_ = 8.0f;
    std::uint32_t drawCursorColor_ = 0xFF000000U;
    bool transparentBackdrop_ = false;
    std::optional<QCursor> drawCursor_ {};
    qreal drawCursorDevicePixelRatio_ = 0.0;
    // Eraser brush size in logical pixels, defaulting as settings::Settings
    // does (SettingsStoreQt.h:84).
    float eraseBrushSize_ = 3.0f;
    std::optional<QCursor> eraserCursor_ {};
    qreal eraserCursorDevicePixelRatio_ = 0.0;

    // Transient selection gesture: the canvas owns the one live gesture and
    // CommitSelection consumes it, so the marquee/PDF drag has a single source
    // of truth.  PDF text drags suppress the marquee and highlight characters.
    struct SelectionGesture {
        enum class Mode {
            None,
            Box,
            Lasso,
            PdfText,
        };
        Mode mode = Mode::None;
        QPointF anchor {};
        QPointF current {};
        std::vector<QPointF> lassoPoints;
        std::optional<std::size_t> pdfPage {};
        [[nodiscard]] bool Active() const { return mode != Mode::None; }
    };
    SelectionGesture selection_;
    std::vector<ObjectRef> pendingErase_;
    // The erase drag's fading trail (widget logical coordinates), animated by
    // eraserTrailTimer_ until it has decayed; eraserTrailRect_ is the region
    // last painted with it, repainted on the next frame to clear old pixels.
    EraserTrail eraserTrail_ {};
    QElapsedTimer eraserTrailClock_ {};
    QTimer* eraserTrailTimer_ = nullptr;
    QRect eraserTrailRect_ {};
    // After the router handled a pointer sample at `position` (nullopt for a
    // cancel): starts, extends or releases the trail with the erase
    // interaction, then repaints/animates what changed.
    void TrackEraserTrail(std::optional<QPointF> position);
    void RefreshEraserTrail();
    // Hold-to-snap: while a stroke is live, shapeHoldTimer_ restarts whenever
    // the pointer leaves kShapeHoldTolerancePixels of shapeHoldAnchor_; when
    // it fires (the pointer rested) SnapHeldStroke snaps it to a shape.
    QTimer* shapeHoldTimer_ = nullptr;
    QPointF shapeHoldAnchor_ {};
    // After the router handled a pointer sample at `position` (nullopt for a
    // cancel): restarts or stops the hold timer with the stroke interaction.
    void TrackShapeHold(std::optional<QPointF> position);
    // Snaps the live stroke and keeps its freehand original as snapGhost_.
    void SnapHeldStroke();

    // A freehand stroke faded out instead of shown as ink: the original a
    // snap replaced, painted faded under the shape while the pointer is down
    // and faded out over kSnapGhostFadeMs after the release, or a scratch-out
    // fading from the release on (snapGhostTimer_ animates it,
    // snapGhostClock_ times it).
    struct SnapGhost {
        std::size_t pageIndex = 0;
        Stroke stroke;
        std::optional<std::int64_t> releasedAt;
    };
    std::optional<SnapGhost> snapGhost_;
    QElapsedTimer snapGhostClock_ {};
    QTimer* snapGhostTimer_ = nullptr;
    [[nodiscard]] float SnapGhostOpacity() const;
    void RepaintSnapGhost();
    // The pointer lifted (`fade`: the shape was committed) or the stroke was
    // cancelled (the ghost goes at once).  A ghost already fading is left be.
    void EndSnapGhost(bool fade);
    void RefreshSnapGhost();

    // Transform gesture session: the handle being dragged, the selection
    // bounds captured at BeginTransform and the logical press position
    // (rotation measures the pointer's start angle from it).  Cleared on
    // commit and cancel.
    struct TransformSession {
        TransformHandle handle = TransformHandle::Move;
        SelectionBounds from {};
        QPointF pressLogical {};
    };
    std::optional<TransformSession> transformSession_;

    InlineMathTextEdit* textEditor_ = nullptr;
    QPlainTextEdit* latexEditor_ = nullptr;
    QListView* latexPopup_ = nullptr;
    QStringListModel* latexPopupModel_ = nullptr;
    std::vector<latex::CompletionItem> latexSuggestions_ {};
    // Size of the last live source that parsed in this LaTeX edit.  Half-typed
    // source (e.g. a fresh "\frac{}{}") does not parse; the source box keeps
    // its place below the last drawable formula instead of jumping up.
    latex::FormulaMetrics latexPreviewMetrics_ {};
    // The overlay the canvas is showing, refreshed atomically from the
    // controller.  EditOverlayActive() queries the controller instead of this
    // cache; the cache is only for placement and painting.
    struct EditOverlayKey {
        enum class Kind {
            Text,
            Latex,
        };
        Kind kind = Kind::Text;
        std::size_t page = 0;
        std::size_t index = 0;

        friend bool operator==(const EditOverlayKey& a, const EditOverlayKey& b) {
            return a.kind == b.kind && a.page == b.page && a.index == b.index;
        }
    };
    std::optional<EditOverlayKey> editOverlay_;
    // View scale the text editor's fonts were last formatted at; the editor is
    // the live preview, so its glyphs must match the canvas render size.
    float textEditScale_ = 1.0f;
    // The box a text-tool drag is sizing, and the height it previews at
    // (one line in the pending text style, page units).
    std::optional<TextBoxPreview> textCreatePreview_;
    float textCreatePreviewHeight_ = 0.0f;

    std::unordered_map<std::size_t, PageSurfaces> surfaces_;
    GestureSurface gesture_ {};
    // Widget-space region the transient state has dirtied since the last
    // paint (live stroke growth, marquee drag, pending-erase marks);
    // InvalidateCanvas repaints it instead of the whole widget.
    QRect transientDirty_ {};
    LiveStrokeLayer strokeLayer_ {};
    // The pending stroke's raw samples run through its StrokeSampling as they
    // arrive: point-for-point what BuildStabilizedPath yields at commit, so
    // the live layer and the dirty rects follow the ink that will be drawn.
    IncrementalStrokeStabilizer liveStabilizer_ {};
    InkPath liveStroke_;
    render::PdfBackgroundProvider* pdfBackground_ = nullptr;
    float cachedDevicePixelRatio_ = 0.0f;
    std::size_t lastPaintPageCount_ = 0;
    std::uint64_t surfaceRenders_ = 0;
    std::uint64_t surfaceScaleMisses_ = 0;
    std::uint64_t lastTransientRepaintArea_ = 0;
    // Pending-erase fade patch: re-rendered region of the page under the
    // erase cursor, keyed by the page's signature (which includes the pending
    // index lists) so drag samples blit instead of re-painting the page live.
    std::size_t erasePatchPage_ = std::numeric_limits<std::size_t>::max();
    std::uint64_t erasePatchSignature_ = 0;
    QRectF erasePatchRegion_;
    QImage erasePatch_;

    static constexpr std::size_t kScalesPerPage = 3;
};

}  // namespace cnote::ui
