#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QImage>
#include <QRect>
#include <QString>
#include <QWidget>

#include "cnote_ui_export.h"
#include "ui/LayoutQt.h"
#include "ui/ToolbarWidget.h"  // InlineEdit, shared with the toolbar zoom editor

namespace cnote {
class Document;
}

namespace cnote::render {
class PdfBackgroundProvider;
}

class QFocusEvent;
class QTimer;
class QVariantAnimation;

namespace cnote::ui {

// Reference left-edge PDF sidebar (plan steps 6.1/6.2).
//
// 176 logical-pixel panel with a lazily rendered page-thumbnail list, the
// bottom header (page indicator, add-page and new-page-settings buttons) and
// the reference 150 ms slide open/close.  The canvas keeps its full width -
// the legacy frontend never subtracts the sidebar from App::CanvasRect
// (src/viewport/Viewport.cpp:107), so this widget is an overlay whose geometry
// the slide animation moves off the left edge.
//
// It reads the model only: SetDocument() receives the shared Document (valid
// until the controller replaces it) and an optional lazily-rendered PDF
// background provider, and every user action is reported as a navigation
// intent.  No App/HWND/HDC state.
//
// Thumbnails reproduce App::EnsureSidebarThumbnailCache (src/App.cpp:4354):
// white page, PDF/raster background, images, strokes - text and LaTeX are
// deliberately absent, matching the reference list rendering.
class CNOTE_UI_EXPORT SidebarWidget : public QWidget {
    Q_OBJECT

public:
    // Reference durations (src/App.h:986-990) shared with ToolbarWidget.
    static constexpr int kScrollAnimationMs = 90;   // kSmoothScrollAnimationDurationMs
    static constexpr int kToolbarAnimationMs = 120; // kToolbarRevealDurationMs
    static constexpr int kSlideAnimationMs = 150;   // kPdfSidebarSlideDurationMs

    explicit SidebarWidget(QWidget* parent = nullptr);
    ~SidebarWidget() override;

    // --- model ------------------------------------------------------------
    void SetDocument(const Document* document, render::PdfBackgroundProvider* pdfBackground = nullptr);
    // Session transient revision (ProjectSession::transientGeneration).
    // In-place gesture edits mutate page objects without bumping the page
    // epoch, so a thumbnail composed mid-gesture must not be served from the
    // epoch fast path after a cancel/abort restored the page.  The provider is
    // queried at lookup/composition time, so any view sharing the session is
    // seen.  Defaults to 0 when unset.
    using TransientGenerationProvider = std::function<std::uint64_t()>;
    void SetTransientGenerationProvider(TransientGenerationProvider provider);
    // Committed document change: repaint; thumbnails validate lazily against
    // Document::PageEpoch so only pages whose contents changed re-render
    // (the reference dropped the whole cache, which re-rasterized every
    // visible thumbnail on the GUI thread after each stroke commit).
    void NotifyDocumentChanged();
    void SetCurrentPage(std::size_t index);
    [[nodiscard]] std::size_t CurrentPage() const { return currentPage_; }

    // --- open/close (150 ms slide) ---------------------------------------
    // One explicit slide phase instead of a bool plus implicit animation
    // endpoints: Opening/Closing are the only transient states.
    enum class SlideState {
        Closed,
        Opening,
        Open,
        Closing,
    };

    void SetOpen(bool open);
    [[nodiscard]] bool IsOpen() const {
        return slideState_ == SlideState::Opening || slideState_ == SlideState::Open;
    }
    [[nodiscard]] SlideState State() const { return slideState_; }
    [[nodiscard]] qreal SlideProgress() const { return slideProgress_; }
    [[nodiscard]] qreal SlideTarget() const { return slideTarget_; }
    // IsPdfSidebarVisible: pages exist and the panel is open or still sliding.
    [[nodiscard]] bool SidebarVisible() const;

    // --- host placement ---------------------------------------------------
    // `clientRect` is the host window's client rectangle; `reservedTop` is
    // appui::qt::ReservedToolbarHeight() (the sidebar starts below the toolbar
    // unless auto-hide reserves nothing).  The widget positions itself, so the
    // shell must not put it in a QLayout.
    void SetHostClientRect(const QRect& clientRect, int reservedTop);
    void SetMaximized(bool maximized);

    // --- scrolling --------------------------------------------------------
    [[nodiscard]] int ScrollY() const { return scrollY_; }
    [[nodiscard]] int ScrollTarget() const { return scrollTarget_; }
    [[nodiscard]] int MaxScroll() const;
    // Programmatic scroll; `animated` uses the reference 90 ms smooth-scroll
    // duration with elapsed-time easing.
    void SetScrollY(int scrollY, bool animated = false);
    void ScrollByPixels(int pixels, bool animated = false);

    // --- geometry / testing ----------------------------------------------
    [[nodiscard]] appui::qt::SidebarMetrics Metrics() const;
    [[nodiscard]] appui::qt::SidebarHeaderLayout HeaderLayout() const;
    [[nodiscard]] std::size_t PageCount() const;
    [[nodiscard]] std::optional<QRect> PageItemRect(std::size_t index) const;
    [[nodiscard]] std::optional<std::size_t> PageAtWidgetPosition(const QPoint& position) const;
    [[nodiscard]] bool HasThumbnail(std::size_t index) const;
    [[nodiscard]] std::size_t RenderedThumbnailCount() const;
    [[nodiscard]] std::size_t VisibleThumbnailCount() const;
    // Bytes held by the thumbnail cache and how many thumbnail rasters it has
    // composed since construction.  Thumbnails are composed outside paint, so
    // the counter lets tests prove animation frames and text-only commits do
    // no raster work.
    [[nodiscard]] std::size_t ThumbnailCacheBytes() const { return thumbnailCacheBytes_; }
    [[nodiscard]] std::uint64_t ThumbnailRenderCount() const { return thumbnailRenderCount_; }
    static constexpr std::size_t kThumbnailCacheBudgetBytes = 4U * 1024U * 1024U;
    [[nodiscard]] bool PageJumpEditing() const { return pageJump_.active; }
    [[nodiscard]] QString PageIndicatorLabel() const;
    // Shift-click selects the range from the anchor (the last plain-clicked
    // page, else the current page) to the clicked page.  Empty while at most
    // the current page is highlighted; sorted otherwise.
    [[nodiscard]] const std::vector<std::size_t>& SelectedPages() const { return selectedPages_; }
    // Selects `count` pages from `first` (a moved block after a drop); the
    // first becomes the shift-click anchor.
    void SelectPages(std::size_t first, std::size_t count);
    void ClearPageSelection();
    // Drag-to-reorder: true once a pressed thumbnail moved past the drag
    // distance.  Dragging a page of a multi-page selection drags them all.
    // The drop slot is the gap (0..PageCount) the pages would land in,
    // nullopt while dropping would not move any page.
    [[nodiscard]] bool PageDragActive() const { return pageDrag_.has_value() && pageDrag_->active; }
    [[nodiscard]] std::optional<std::size_t> DropSlot() const;
    // Widget-space y of the drop indicator line for `slot`: centred in the
    // gap above page `slot`, or below the last page.
    [[nodiscard]] std::optional<int> DropIndicatorY(std::size_t slot) const;

    // Reference page-jump input (App.cpp:2840 PageIndicatorLabel,
    // ToolbarState.cpp:265-296 BeginPageJumpInput/ApplyPageJumpInput).
    void BeginPageJumpEdit();
    void CancelPageJumpEdit();
    void CommitPageJumpEdit();

signals:
    void PageActivated(std::size_t pageIndex);
    void PageJumpRequested(std::size_t pageIndex);
    void AddPageRequested();
    void NewPageSettingsRequested();
    // Right-click on a page thumbnail: the host shows the page-management
    // menu (copy/cut/paste/delete).  The page is activated first.  On a page
    // of the multi-page selection the selection stays (SelectedPages), so the
    // menu can act on all of it; elsewhere the selection is dropped.
    void PageContextMenuRequested(std::size_t pageIndex, const QPoint& globalPosition);
    // Thumbnails were dragged and dropped elsewhere: move `pages` (sorted) as
    // one block, in order, to gap `slot` (counted before the move).
    void PagesMoveRequested(const std::vector<std::size_t>& pages, std::size_t slot);
    void OpenChanged(bool open);
    void SlideProgressChanged(qreal progress);
    void ScrollChanged(int scrollY);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void UpdateGeometry();
    void UpdateVisibility();
    void StartSlideAnimation(qreal target);
    void StartScrollAnimation(int target);
    void EnsureCurrentPageVisible();
    const QImage* ThumbnailAt(std::size_t index, int width, int height);
    bool RenderThumbnail(std::size_t index, int width, int height);
    void RequestThumbnailRenders();
    void RenderVisibleThumbnails();
    void TrimThumbnailCache();
    void SnapshotPageRevisions();
    void InvalidateThumbnails();
    [[nodiscard]] std::uint64_t TransientGeneration() const;
    [[nodiscard]] int ItemWidth() const;
    [[nodiscard]] QRect ContentRect() const;
    [[nodiscard]] int ItemTop(std::size_t index) const;
    void UpdatePageDrag(const QPoint& position);
    void CancelPageDrag();
    void AutoScrollPageDrag();

    const Document* document_ = nullptr;
    render::PdfBackgroundProvider* pdfBackground_ = nullptr;
    TransientGenerationProvider transientGenerationProvider_ {};
    std::size_t currentPage_ = 0;
    std::size_t documentPageCount_ = 0;

    SlideState slideState_ = SlideState::Closed;
    qreal slideProgress_ = 0.0;
    qreal slideTarget_ = 0.0;
    // Suppresses the `finished` handler while an old run is being stopped, so
    // stopping can never publish a terminal state (only a natural end can).
    bool slideStopping_ = false;
    QVariantAnimation* slideAnimation_ = nullptr;

    QRect hostClientRect_ {};
    int reservedTop_ = 0;
    bool maximized_ = false;

    int scrollY_ = 0;
    int scrollTarget_ = 0;
    qreal wheelRemainder_ = 0.0;
    QVariantAnimation* scrollAnimation_ = nullptr;

    InlineEdit pageJump_ {};

    std::vector<std::size_t> selectedPages_ {};
    std::optional<std::size_t> selectionAnchor_ {};

    struct PageDrag {
        std::size_t pressedPage = 0;
        std::vector<std::size_t> pages {};  // what the drag moves, sorted
        QPoint pressPosition {};
        QPoint position {};
        bool active = false;
        // A plain press on a page of the multi-selection keeps the selection
        // so it can be dragged; released without a drag, it selects only
        // that page.
        bool collapseOnClick = false;
        std::optional<std::size_t> dropSlot {};
    };
    std::optional<PageDrag> pageDrag_ {};
    // Scrolls the list while a drag hovers near the content band's edges.
    QTimer* dragScrollTimer_ = nullptr;

    // Everything a cached thumbnail depends on.  `document` and the pixel size
    // pick the raster; `fingerprint` covers the page content the thumbnail
    // actually paints (background, raster, images, strokes, rotation) and
    // deliberately ignores text/LaTeX, which thumbnails do not depict.
    // `sourceEpoch` is the page epoch `fingerprint` was last checked against:
    // an unchanged epoch is a fast hit, and a changed one recomputes the
    // fingerprint once, then keeps the image when only text moved.
    struct ThumbnailKey {
        const Document* document = nullptr;
        std::uint64_t fingerprint = 0;
        int width = 0;
        int height = 0;

        [[nodiscard]] bool IsValid() const noexcept {
            return document != nullptr && width > 0 && height > 0;
        }
    };
    struct Thumbnail {
        QImage image {};
        ThumbnailKey key {};
        std::uint64_t sourceEpoch = 0;
        // Generation of in-place gesture edits when the key was last checked.
        // A change (any view's gesture sample, cancel or abort) forces the
        // fingerprint path; the epoch alone cannot see those edits.
        std::uint64_t sourceTransientGeneration = 0;
        std::uint64_t lastUse = 0;
    };
    // Visited pages are evicted least-recently-used once `thumbnailCacheBytes_`
    // exceeds the budget; only visible pages are ever composed, so a long list
    // no longer retains a raster per page ever scrolled past.
    std::unordered_map<std::size_t, Thumbnail> thumbnails_ {};
    std::size_t thumbnailCacheBytes_ = 0;
    std::uint64_t thumbnailUseClock_ = 0;
    std::uint64_t thumbnailRenderCount_ = 0;
    QTimer* thumbnailTimer_ = nullptr;
    // Visible pages whose thumbnail was missing at paint time; the zero-timer
    // composes these outside paint.  Paint appends, the timer consumes, so
    // repeated slide/paint frames coalesce into one pass over unique pages.
    std::vector<std::size_t> pendingThumbnailPages_ {};

    // Page revisions at the last SetDocument/NotifyDocumentChanged, used to
    // dirty only the item rects a commit changed.  Sizes detect layout shifts
    // (rotate/resize/reorder) that move the pages below.
    std::vector<std::uint64_t> knownPageEpochs_ {};
    std::vector<std::pair<float, float>> knownPageSizes_ {};
};

}  // namespace cnote::ui
