#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QImage>
#include <QRect>
#include <QString>
#include <QWidget>

#include "ui/LayoutQt.h"

namespace cnote {
class Document;
}

namespace cnote::render {
class PdfBackgroundProvider;
}

class QTimer;

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
class SidebarWidget : public QWidget {
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
    [[nodiscard]] const Document* DocumentPtr() const { return document_; }
    // Committed document change: drops every cached thumbnail (App.cpp:4300
    // InvalidateSidebarThumbnailCache).
    void NotifyDocumentChanged();
    void SetCurrentPage(std::size_t index);
    [[nodiscard]] std::size_t CurrentPage() const { return currentPage_; }

    // --- open/close (150 ms slide) ---------------------------------------
    void SetOpen(bool open);
    [[nodiscard]] bool IsOpen() const { return open_; }
    [[nodiscard]] qreal SlideProgress() const { return slideProgress_; }
    // IsPdfSidebarVisible: pages exist and the panel is open or still sliding.
    [[nodiscard]] bool SidebarVisible() const;

    // --- host placement ---------------------------------------------------
    // `clientRect` is the host window's client rectangle; `reservedTop` is
    // appui::qt::ReservedToolbarHeight() (the sidebar starts below the toolbar
    // unless auto-hide reserves nothing).  The widget positions itself, so the
    // shell must not put it in a QLayout.
    void SetHostClientRect(const QRect& clientRect, int reservedTop);
    [[nodiscard]] QRect HostClientRect() const { return hostClientRect_; }
    void SetMaximized(bool maximized);

    // --- scrolling --------------------------------------------------------
    [[nodiscard]] int ScrollY() const { return scrollY_; }
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
    [[nodiscard]] bool PageJumpEditing() const { return pageJumpEditing_; }
    [[nodiscard]] QString PageIndicatorLabel() const;

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
    void OpenChanged(bool open);
    void SlideProgressChanged(qreal progress);
    void ScrollChanged(int scrollY);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void UpdateGeometry();
    void UpdateVisibility();
    void StartSlideAnimation(qreal target);
    void OnSlideTick();
    void StartScrollAnimation(int target);
    void OnScrollTick();
    void EnsureCurrentPageVisible();
    const QImage* ThumbnailAt(std::size_t index, int width, int height);
    void InvalidateThumbnails();
    [[nodiscard]] int ItemWidth() const;
    [[nodiscard]] QRect ContentRect() const;
    [[nodiscard]] int ItemTop(std::size_t index) const;

    const Document* document_ = nullptr;
    render::PdfBackgroundProvider* pdfBackground_ = nullptr;
    std::size_t currentPage_ = 0;

    bool open_ = false;
    qreal slideProgress_ = 0.0;
    int slideFrom_ = 0;
    int slideTarget_ = 0;
    QTimer* slideTimer_ = nullptr;
    QElapsedTimer slideClock_ {};

    QRect hostClientRect_ {};
    int reservedTop_ = 0;
    bool maximized_ = false;

    int scrollY_ = 0;
    int scrollFrom_ = 0;
    int scrollTarget_ = 0;
    qreal wheelRemainder_ = 0.0;
    QTimer* scrollTimer_ = nullptr;
    QElapsedTimer scrollClock_ {};

    bool pageJumpEditing_ = false;
    QString pageJumpText_ {};

    struct Thumbnail {
        QImage image {};
        int width = 0;
        int height = 0;
        bool valid = false;
    };
    std::vector<Thumbnail> thumbnails_ {};
};

}  // namespace cnote::ui
