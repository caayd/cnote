#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "Document.h"
#include "latex/LatexCompletion.h"
#include "render/PageRenderer.h"
#include "ui/CanvasController.h"

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
class CanvasWidget : public QWidget, public EditContentSource {
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
    static constexpr int kTextEditorMinHeight = 120;

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
    [[nodiscard]] std::optional<CanvasPageView> HitTestPage(const QPointF& logical) const;
    [[nodiscard]] Vec2 ScreenToPage(const CanvasPageView& view, const QPointF& logical, bool clampToPage) const;
    [[nodiscard]] QPointF PageToScreen(std::size_t pageIndex, Vec2 point) const;

    // --- document changes -------------------------------------------------
    void NotifyDocumentChanged();
    void SetPdfBackgroundProvider(render::PdfBackgroundProvider* provider);

    // --- interaction ------------------------------------------------------
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

    bool panning_ = false;
    QPointF panLastPosition_ {};

    bool drawing_ = false;
    std::optional<std::size_t> strokePageIndex_ {};
    QPointF strokeLastPosition_ {};

    QTextEdit* textEditor_ = nullptr;
    QPlainTextEdit* latexEditor_ = nullptr;
    QListView* latexPopup_ = nullptr;
    QStringListModel* latexPopupModel_ = nullptr;
    std::vector<latex::CompletionItem> latexSuggestions_ {};
    std::optional<std::size_t> overlayPageIndex_ {};
    std::optional<std::size_t> overlayTextIndex_ {};
    std::optional<std::size_t> overlayLatexIndex_ {};

    std::unordered_map<std::size_t, PageSurfaces> surfaces_;
    render::PdfBackgroundProvider* pdfBackground_ = nullptr;
    float cachedDevicePixelRatio_ = 0.0f;
    std::size_t lastPaintPageCount_ = 0;

    static constexpr std::size_t kScalesPerPage = 3;
};

}  // namespace cnote::ui
