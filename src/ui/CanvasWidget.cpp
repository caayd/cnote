#include "ui/CanvasWidget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <QColor>
#include <QCursor>
#include <QEnterEvent>
#include <QImage>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScreen>
#include <QPen>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QWheelEvent>

#include "latex/LatexEngine.h"
#include "ui/SelectionMath.h"
#include "text/TextEditingQt.h"
#include "text/TextLayoutQt.h"

namespace cnote::ui {

namespace {

// Reference App::PaintCanvas canvas backdrop: RGB(22, 26, 33).
constexpr std::uint32_t kCanvasBackdrop = 0xFF161A21U;
// Reference App::PaintPage page border: 1 px RGB(94, 105, 121).
constexpr std::uint32_t kPageBorderColor = 0xFF5E6979U;

// Reference App::ClampViewportPan slack.
constexpr float kSlackRatio = 0.2f;
constexpr float kHorizontalSlackMin = 96.0f;
constexpr float kHorizontalSlackMax = 260.0f;
constexpr float kVerticalSlackMin = 96.0f;
constexpr float kVerticalSlackMax = 320.0f;

constexpr float kMinimumPageScale = 0.1f;

// Page units of extra repaint margin around a live stroke segment.
constexpr float kStrokeDirtyMarginPixels = 12.0f;

// Reference App.cpp:170-171: the drawing cursor's diameter bounds in logical
// pixels (SettingsStoreQt clamps the persisted value to the same 4..24 range).
constexpr float kMinDrawCursorDiameter = 4.0f;
constexpr float kMaxDrawCursorDiameter = 24.0f;

// Reference App.cpp:168-169: eraser geometry in logical pixels.  The minimum
// radius floors the brush size (App::EraserRadiusPixels, App.cpp:7426-7428) and
// the outline width is the ring the erase cursor is drawn with.
constexpr float kEraserMinRadiusPixels = 3.5f;
constexpr qreal kEraserOutlinePixels = 0.75f;

// LaTeX source box and completion popup metrics (logical pixels).
constexpr int kLatexSourceBoxHeight = 120;
constexpr int kLatexSourceBoxGap = 6;
constexpr int kLatexPopupWidth = 260;
constexpr int kLatexPopupHeight = 140;
constexpr int kLatexPopupPageStep = 8;

QString FromUtf16(const std::u16string& value) {
    return QString::fromUtf16(value.data(), static_cast<qsizetype>(value.size()));
}

std::u16string ToUtf16(const QString& value) {
    return value.toStdU16String();
}

Qt::Alignment TextAlignmentFor(TextAlignment alignment) {
    switch (alignment) {
    case TextAlignment::Center:
        return Qt::AlignHCenter;
    case TextAlignment::Right:
        return Qt::AlignRight;
    case TextAlignment::Left:
    default:
        return Qt::AlignLeft;
    }
}
}  // namespace

CanvasWidget::CanvasWidget(CanvasController& controller, QWidget* parent)
    : QWidget(parent),
      controller_(controller) {
    // The whole widget is repainted by us; Qt need not clear it first.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::StrongFocus);

    router_ = std::make_unique<InteractionRouter>(*this, InteractionTuning {});

    zoomSettleTimer_ = new QTimer(this);
    zoomSettleTimer_->setSingleShot(true);
    zoomSettleTimer_->setInterval(kZoomSessionDelayMs);
    connect(zoomSettleTimer_, &QTimer::timeout, this, [this]() { EndZoomSession(); });
}

CanvasWidget::~CanvasWidget() = default;

// --- view state ------------------------------------------------------------

void CanvasWidget::SetZoom(float zoom, std::optional<QPointF> anchor) {
    // Reference App::SetZoom/App::ZoomAt (Viewport.cpp:382/676) end an in-flight
    // smooth scroll before changing the scale: the animation carries pan targets
    // computed at the old scale and would overwrite the anchored pan below.
    StopSmoothScroll();
    const float clamped = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (std::abs(clamped - zoom_) < 0.001f) {
        update();
        return;
    }

    const QPointF focus = anchor.has_value() ? *anchor : VisibleCanvasRect().center();
    BeginZoomSession(focus);
    zoom_ = clamped;

    if (zoomAnchorValid_) {
        const QPointF anchored = PageToScreen(zoomAnchorPageIndex_, zoomAnchorPage_);
        panX_ += static_cast<float>(zoomAnchorCursor_.x() - anchored.x());
        panY_ += static_cast<float>(zoomAnchorCursor_.y() - anchored.y());
    }
    ClampViewportPan();
    NotifyViewportChanged();
}

void CanvasWidget::ZoomBySteps(int steps, const QPointF& anchor) {
    if (steps == 0) {
        return;
    }
    float factor = 1.0f;
    if (steps > 0) {
        for (int step = 0; step < steps; ++step) {
            factor *= kWheelZoomInFactor;
        }
    } else {
        for (int step = 0; step < -steps; ++step) {
            factor *= kWheelZoomOutFactor;
        }
    }
    SetZoom(zoom_ * factor, anchor);
}

void CanvasWidget::FitToWindow() {
    SetZoom(1.0f);
}

void CanvasWidget::ResetView() {
    // Reference App::ResetView (Viewport.cpp:62-64) stops the smooth scroll
    // before zeroing the pan; a running animation would restore it on its next
    // tick.
    StopSmoothScroll();
    zoomSettleTimer_->stop();
    zoomSessionActive_ = false;
    zoomAnchorValid_ = false;
    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
    NotifyViewportChanged();
}

void CanvasWidget::BeginZoomSession(const QPointF& anchor) {
    zoomSessionActive_ = true;
    zoomAnchorValid_ = false;
    if (const std::optional<CanvasPageView> hit = HitTestPageView(anchor); hit.has_value()) {
        zoomAnchorValid_ = true;
        zoomAnchorPageIndex_ = hit->pageIndex;
        zoomAnchorPage_ = ScreenToPage(*hit, anchor, false);
        zoomAnchorCursor_ = anchor;
    }
    zoomSettleTimer_->start();
}

void CanvasWidget::EndZoomSession() {
    if (!zoomSessionActive_) {
        return;
    }
    zoomSessionActive_ = false;
    zoomAnchorValid_ = false;
    update();
}

// --- panning ---------------------------------------------------------------

void CanvasWidget::PanBy(const QPointF& delta) {
    // Reference App::PanBy (Viewport.cpp:116-119) cancels an in-flight smooth
    // scroll before applying the delta: a precision pan (native touchpad frame
    // or a pixelDelta event) must land 1:1 and must not fight the 90 ms
    // animation of an earlier wheel notch for the same viewport.
    StopSmoothScroll();
    panX_ += static_cast<float>(delta.x());
    panY_ += static_cast<float>(delta.y());
    ClampViewportPan();
    NotifyViewportChanged();
}

void CanvasWidget::CenterOnPage(std::size_t pageIndex) {
    const Document& document = controller_.DocumentRef();
    if (pageIndex >= document.PageCount()) {
        return;
    }

    const CanvasPageView view = CalculatePageView(pageIndex);
    if (!view.valid) {
        update();
        return;
    }

    // Reference App::FocusPage (Viewport.cpp:94-95).
    StopSmoothScroll();
    const QRectF canvasRect = VisibleCanvasRect();
    const qreal pageCenterY = view.rect.top() + (view.rect.height() / 2.0);
    const qreal desiredCenterY = canvasRect.top() + (canvasRect.height() / 2.0);
    panY_ += static_cast<float>(desiredCenterY - pageCenterY);
    ClampViewportPan();
    NotifyViewportChanged();
}

float CanvasWidget::ZoomReferenceCanvasWidth() const {
    // Plan step 3.2 keeps fit-to-monitor: the reference uses
    // MaximizedCanvasWidthForZoom(window, canvasWidth, sidebarWidth=0, dpi)
    // = work-area width - 2 * ScaleForDpi(8, dpi); the sidebar is a sibling
    // widget here, so it does not narrow the canvas widget's reference band.
    const QScreen* widgetScreen = screen();
    if (widgetScreen != nullptr) {
        const QRect available = widgetScreen->availableGeometry();
        if (available.width() > 0) {
            return std::max(1.0f, static_cast<float>(available.width()) - 16.0f);
        }
    }
    return std::max(1.0f, static_cast<float>(width()));
}

QRectF CanvasWidget::VisibleCanvasRect() const {
    // The Qt canvas is its own widget, so the reference frontend's reserved
    // toolbar height and maximized-frame insets do not apply here.
    return QRectF(rect());
}

void CanvasWidget::ClampViewportPan() {
    const Document& document = controller_.DocumentRef();
    if (document.PageCount() == 0U) {
        return;
    }

    const QRectF canvasRect = VisibleCanvasRect();
    const float canvasWidth = std::max(1.0f, static_cast<float>(canvasRect.width()));
    const float canvasHeight = std::max(1.0f, static_cast<float>(canvasRect.height()));

    float widestPage = 1.0f;
    float totalPageHeight = 0.0f;
    for (std::size_t index = 0; index < document.PageCount(); ++index) {
        const Page& page = document.PageAt(index);
        widestPage = std::max(widestPage, page.width);
        totalPageHeight += page.height;
    }

    const float fitScale = (ZoomReferenceCanvasWidth() - static_cast<float>(kZoomReferenceInset)) / widestPage;
    const float scale = std::max(kMinimumPageScale, fitScale * zoom_);
    const float scaledDocumentWidth = widestPage * scale;
    const float scaledDocumentHeight = (totalPageHeight * scale)
        + static_cast<float>(std::max<std::size_t>(document.PageCount() - 1U, 0U) * kPageGap);
    const float canvasCenterX = static_cast<float>(canvasRect.left()) + (canvasWidth / 2.0f);
    const float documentLeft = canvasCenterX - (scaledDocumentWidth * 0.5f);
    const float documentTop = static_cast<float>(canvasRect.top()) + static_cast<float>(kTopInset);
    const float documentRight = canvasCenterX + (scaledDocumentWidth * 0.5f);
    const float documentBottom = documentTop + scaledDocumentHeight;

    const float horizontalSlack = std::clamp(
        canvasWidth * kSlackRatio, kHorizontalSlackMin, kHorizontalSlackMax);
    const float verticalSlack = std::clamp(
        canvasHeight * kSlackRatio, kVerticalSlackMin, kVerticalSlackMax);

    const float minPanX = (static_cast<float>(canvasRect.left()) - horizontalSlack) - documentRight;
    const float maxPanX = (static_cast<float>(canvasRect.right()) + horizontalSlack) - documentLeft;
    const float minPanY = (static_cast<float>(canvasRect.top()) - verticalSlack) - documentBottom;
    const float maxPanY = (static_cast<float>(canvasRect.bottom()) + verticalSlack) - documentTop;

    panX_ = std::clamp(panX_, minPanX, maxPanX);
    panY_ = std::clamp(panY_, minPanY, maxPanY);
}

void CanvasWidget::NotifyViewportChanged() {
    PositionOverlays();
    update();
    emit ViewportChanged();
}

// --- geometry --------------------------------------------------------------

std::vector<CanvasPageView> CanvasWidget::CalculatePageViews() const {
    const Document& document = controller_.DocumentRef();
    const QRectF canvasRect = VisibleCanvasRect();
    const float canvasWidth = std::max(1.0f, static_cast<float>(canvasRect.width()));

    float widestPage = 1.0f;
    for (std::size_t index = 0; index < document.PageCount(); ++index) {
        widestPage = std::max(widestPage, document.PageAt(index).width);
    }

    const float referenceWidth = ZoomReferenceCanvasWidth();
    const float fitScale = (referenceWidth - static_cast<float>(kZoomReferenceInset)) / widestPage;
    const float scale = std::max(kMinimumPageScale, fitScale * zoom_);
    const float canvasCenterX = static_cast<float>(canvasRect.left()) + (canvasWidth / 2.0f);
    float top = static_cast<float>(canvasRect.top()) + static_cast<float>(kTopInset)
        + static_cast<float>(std::lround(panY_));

    std::vector<CanvasPageView> views;
    views.reserve(document.PageCount());
    for (std::size_t index = 0; index < document.PageCount(); ++index) {
        const Page& page = document.PageAt(index);
        const int pageWidth = static_cast<int>(std::lround(page.width * scale));
        const int pageHeight = static_cast<int>(std::lround(page.height * scale));
        const int left = static_cast<int>(std::lround(canvasCenterX - (static_cast<float>(pageWidth) / 2.0f) + panX_));

        views.push_back(CanvasPageView {
            index,
            QRectF(static_cast<qreal>(left), static_cast<qreal>(top),
                   static_cast<qreal>(pageWidth), static_cast<qreal>(pageHeight)),
            scale,
            pageWidth > 0 && pageHeight > 0,
        });
        top += static_cast<float>(pageHeight + kPageGap);
    }
    return views;
}

CanvasPageView CanvasWidget::CalculatePageView(std::size_t pageIndex) const {
    const std::vector<CanvasPageView> views = CalculatePageViews();
    if (pageIndex >= views.size()) {
        return {};
    }
    return views[pageIndex];
}

std::optional<CanvasPageView> CanvasWidget::HitTestPageView(const QPointF& logical) const {
    for (const CanvasPageView& view : CalculatePageViews()) {
        if (view.valid && view.rect.contains(logical)) {
            return view;
        }
    }
    return std::nullopt;
}

Vec2 CanvasWidget::ScreenToPage(const CanvasPageView& view, const QPointF& logical, bool clampToPage) const {
    const Document& document = controller_.DocumentRef();
    if (!view.valid || view.pageIndex >= document.PageCount() || view.scale <= 0.0f) {
        return {};
    }

    const Page& page = document.PageAt(view.pageIndex);
    const float pageX = static_cast<float>(logical.x() - view.rect.left()) / view.scale;
    const float pageY = static_cast<float>(logical.y() - view.rect.top()) / view.scale;
    if (!clampToPage) {
        return { pageX, pageY };
    }
    return {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
}

QPointF CanvasWidget::PageToScreen(std::size_t pageIndex, Vec2 point) const {
    const CanvasPageView view = CalculatePageView(pageIndex);
    return QPointF(
        view.rect.left() + static_cast<qreal>(std::lround(point.x * view.scale)),
        view.rect.top() + static_cast<qreal>(std::lround(point.y * view.scale)));
}

// --- caches ----------------------------------------------------------------

render::PageRenderParams CanvasWidget::RenderParamsFor(const CanvasPageView& view) const {
    render::PageRenderParams params;
    params.scale = view.scale;
    params.devicePixelRatio = cachedDevicePixelRatio_ > 0.0f ? cachedDevicePixelRatio_ : 1.0f;
    params.strokeSmoothing = controller_.InputSettings().strokeSmoothing;
    // Lazy PDF page backgrounds for pages that have a PDF source and no stored
    // raster; PageRenderer asks the provider at the current raster scale.
    params.pdfBackground = pdfBackground_;
    // Objects being edited stay out of the committed raster cache.
    if (controller_.EditingPageIndex() == view.pageIndex) {
        params.excludedTextIndex = controller_.EditingTextIndex();
        params.excludedLatexIndex = controller_.EditingLatexIndex();
    }
    return params;
}

const render::PageSurfaceCache* CanvasWidget::FindSurface(
    std::size_t pageIndex, float scale, float devicePixelRatio) const {
    const auto page = surfaces_.find(pageIndex);
    if (page == surfaces_.end()) {
        return nullptr;
    }
    for (const SurfaceEntry& entry : page->second.entries) {
        if (entry.scale == scale && entry.devicePixelRatio == devicePixelRatio && entry.cache.valid) {
            return &entry.cache;
        }
    }
    return nullptr;
}

render::PageSurfaceCache* CanvasWidget::MutableSurface(
    std::size_t pageIndex, float scale, float devicePixelRatio) {
    const auto page = surfaces_.find(pageIndex);
    if (page == surfaces_.end()) {
        return nullptr;
    }
    for (SurfaceEntry& entry : page->second.entries) {
        if (entry.scale == scale && entry.devicePixelRatio == devicePixelRatio && entry.cache.valid) {
            return &entry.cache;
        }
    }
    return nullptr;
}

const render::PageSurfaceCache* CanvasWidget::NewestSurface(std::size_t pageIndex) const {
    const auto page = surfaces_.find(pageIndex);
    if (page == surfaces_.end()) {
        return nullptr;
    }
    for (const SurfaceEntry& entry : page->second.entries) {
        if (entry.cache.valid && !entry.cache.image.isNull()) {
            return &entry.cache;
        }
    }
    return nullptr;
}

const render::PageSurfaceCache& CanvasWidget::EnsureSurface(const CanvasPageView& view, const Page& page) {
    PageSurfaces& perPage = surfaces_[view.pageIndex];
    const float devicePixelRatio = RenderParamsFor(view).devicePixelRatio;

    for (SurfaceEntry& entry : perPage.entries) {
        if (entry.scale == view.scale && entry.devicePixelRatio == devicePixelRatio) {
            const std::uint64_t before = entry.cache.signature;
            render::EnsurePageSurface(entry.cache, page, RenderParamsFor(view));
            if (entry.cache.signature != before) {
                ++surfaceRenders_;
            }
            // The surface is rendered at scale * DPR pixels; stamping the DPR
            // lets the paint path blit it 1:1 in device pixels.
            if (!entry.cache.image.isNull()) {
                entry.cache.image.setDevicePixelRatio(devicePixelRatio);
            }
            return entry.cache;
        }
    }

    ++surfaceScaleMisses_;
    perPage.entries.insert(perPage.entries.begin(), SurfaceEntry { view.scale, devicePixelRatio, {} });
    if (perPage.entries.size() > kScalesPerPage) {
        perPage.entries.resize(kScalesPerPage);
    }
    render::EnsurePageSurface(perPage.entries.front().cache, page, RenderParamsFor(view));
    if (!perPage.entries.front().cache.image.isNull()) {
        perPage.entries.front().cache.image.setDevicePixelRatio(devicePixelRatio);
    }
    return perPage.entries.front().cache;
}

void CanvasWidget::PruneSurfaces(const std::vector<std::size_t>& visiblePages) {
    for (auto entry = surfaces_.begin(); entry != surfaces_.end();) {
        if (std::find(visiblePages.begin(), visiblePages.end(), entry->first) == visiblePages.end()) {
            entry = surfaces_.erase(entry);
        } else {
            ++entry;
        }
    }
}

void CanvasWidget::NotifyDocumentChanged() {
    // Repaint request, not a cache drop: every committed change is covered by
    // the per-page signature (content hash) the renderer checks on the next
    // paint, and CommitStroke already composites its stroke into the cached
    // raster. Clearing here re-rendered a dense page (~1 s) on the paint after
    // each commit, which delayed the first frame of the next stroke by ~10
    // refresh intervals (measured on dense-1000x100 at a 0.8 s cadence).
    update();
}

void CanvasWidget::SetPdfBackgroundProvider(render::PdfBackgroundProvider* provider) {
    if (pdfBackground_ == provider) {
        return;
    }
    pdfBackground_ = provider;
    surfaces_.clear();
    update();
}

std::size_t CanvasWidget::CachedSurfaceCount() const {
    std::size_t count = 0;
    for (const auto& page : surfaces_) {
        count += page.second.entries.size();
    }
    return count;
}

// --- painting --------------------------------------------------------------

void CanvasWidget::paintEvent(QPaintEvent* event) {
    const float devicePixelRatio = static_cast<float>(devicePixelRatioF());
    if (cachedDevicePixelRatio_ != devicePixelRatio) {
        // Display/DPR change: raster caches are keyed by their effective scale.
        cachedDevicePixelRatio_ = devicePixelRatio;
        surfaces_.clear();
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(event->rect(), QColor::fromRgba(kCanvasBackdrop));

    const Document& document = controller_.DocumentRef();
    const QRectF clip = QRectF(event->rect());
    const std::vector<CanvasPageView> views = CalculatePageViews();
    // Caches are keyed to pages the user can see, not to this paint's clip: a
    // clipped repaint (live stroke segment, partial update) must never evict the
    // page raster it just drew from, or the next full paint re-renders it.
    std::vector<std::size_t> visiblePages;
    for (const CanvasPageView& candidate : views) {
        if (candidate.valid && candidate.rect.intersects(QRectF(rect()))) {
            visiblePages.push_back(candidate.pageIndex);
        }
    }
    std::size_t paintedPages = 0;

    for (const CanvasPageView& view : views) {
        if (!view.valid || view.pageIndex >= document.PageCount()) {
            continue;
        }

        // Reference culling: a page is considered when it is within the paint
        // bounds plus 20 px (0 while a zoom session previews a snapshot).
        const qreal padding = zoomSessionActive_ ? 0.0 : 20.0;
        if (view.rect.bottom() < clip.top() - padding || view.rect.top() > clip.bottom() + padding) {
            continue;
        }
        if (!view.rect.intersects(clip)) {
            continue;
        }

        const Page& page = document.PageAt(view.pageIndex);
        const bool liveStrokeOnPage = controller_.PendingStroke() != nullptr
            && controller_.PendingStrokePage() == view.pageIndex;
        // Reference App::PaintPage (App.cpp:9453-9472): a page with pending-erase
        // objects skips the surface cache and repaints live so the marked
        // objects can draw at kPendingEraseObjectOpacity.
        const render::PendingEraseIndices pendingErase = PendingEraseFor(view.pageIndex);
        const bool pageHasPendingErase = !pendingErase.Empty();
        const render::PageSurfaceCache* surface = nullptr;

        if (!pageHasPendingErase) {
            if (zoomSessionActive_ && !liveStrokeOnPage) {
                surface = FindSurface(view.pageIndex, view.scale, cachedDevicePixelRatio_);
                if (surface == nullptr) {
                    // Zoom snapshot: reuse the newest raster (drawn scaled) until the
                    // session settles, instead of re-rendering the page mid-gesture.
                    surface = NewestSurface(view.pageIndex);
                }
                if (surface == nullptr) {
                    surface = &EnsureSurface(view, page);
                }
            } else {
                surface = &EnsureSurface(view, page);
            }
        }

        painter.save();
        painter.setClipRect(view.rect);
        if (pageHasPendingErase) {
            render::PageRenderParams params = RenderParamsFor(view);
            params.pendingErase = pendingErase;
            render::PaintPage(painter, page, view.rect.topLeft(), params);
        } else {
            // The cached surface already has this scale * DPR resolution and carries
            // its device pixel ratio, so blit it 1:1: drawing it into a target rect
            // would resample the whole (up to ~8 MP) image every frame, which is what
            // made dense-page panning and drawing crawl.  The device pixel ratio
            // alone does not prove the raster is at this view's scale: during a zoom
            // session FindSurface misses and the newest raster (a snapshot at the
            // previous scale) is reused, and it must be resampled into view.rect or
            // the page renders at the stale zoom for the whole gesture.
            const qreal rasterDpr = surface->image.devicePixelRatio() > 0.0
                ? surface->image.devicePixelRatio()
                : 1.0;
            const bool rasterMatchesView = !surface->image.isNull()
                && std::abs(rasterDpr - cachedDevicePixelRatio_) < 0.001
                && std::abs((surface->image.width() / rasterDpr) - view.rect.width()) < 1.0
                && std::abs((surface->image.height() / rasterDpr) - view.rect.height()) < 1.0;
            if (rasterMatchesView) {
                painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                painter.drawImage(view.rect.topLeft(), surface->image);
            } else {
                painter.drawImage(view.rect, surface->image);
            }
        }

        if (liveStrokeOnPage) {
            if (const Stroke* pending = controller_.PendingStroke(); pending != nullptr) {
                painter.save();
                painter.translate(view.rect.topLeft());
                render::PaintStrokePreview(painter, *pending, RenderParamsFor(view), 1.0f);
                painter.restore();
            }
        }

        // Live LaTeX preview while the source box is open.
        if (latexEditor_ != nullptr && latexEditor_->isVisible()
            && overlayPageIndex_.has_value() && *overlayPageIndex_ == view.pageIndex
            && overlayLatexIndex_.has_value()) {
            if (const PageLatex* object = controller_.EditingLatexObject(); object != nullptr) {
                PageLatex preview = *object;
                preview.content = ToUtf16(latexEditor_->toPlainText());
                if (const latex::FormulaHandle formula = latex::Parse(preview); formula != nullptr) {
                    painter.save();
                    painter.translate(view.rect.topLeft());
                    latex::Draw(formula, painter, preview.x * view.scale, preview.y * view.scale, view.scale, preview.color);
                    painter.restore();
                }
            }
        }
        painter.restore();

        // Page border, drawn on top of the page edge like the reference.
        painter.save();
        painter.setPen(QPen(QColor::fromRgba(kPageBorderColor), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(view.rect.adjusted(0.5, 0.5, -0.5, -0.5));
        painter.restore();

        ++paintedPages;
    }

    lastPaintPageCount_ = paintedPages;
    PruneSurfaces(visiblePages);
    PaintSelectionOverlay(painter);
    PositionOverlays();
}

void CanvasWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // The fit scale depends on the canvas width, so the page layout changes with
    // the widget; the reference frontend re-derives it on every paint as well.
    PositionOverlays();
    update();
}

// --- interaction -----------------------------------------------------------

void CanvasWidget::BeginStrokeAt(const QPointF& logical) {
    PointerSample sample;
    sample.position = logical;
    sample.button = Qt::LeftButton;
    sample.buttons = Qt::LeftButton;
    sample.timestamp = 0;
    HandlePointerPress(sample);
}

bool CanvasWidget::ExtendStrokeTo(const QPointF& logical) {
    PointerSample sample;
    sample.position = logical;
    sample.buttons = Qt::LeftButton;
    HandlePointerMove(sample);
    return true;
}

void CanvasWidget::EndStroke() {
    PointerSample sample;
    sample.position = lastPointerPosition_;
    HandlePointerRelease(sample);
}

void CanvasWidget::CancelStroke() {
    HandlePointerCancel(CancelReason::Escape);
}

// --- editing overlay -------------------------------------------------------

bool CanvasWidget::EditOverlayActive() const {
    return overlayTextIndex_.has_value() || overlayLatexIndex_.has_value();
}

bool CanvasWidget::BeginEditAt(const QPointF& logical) {
    const std::optional<CanvasPageView> hit = HitTestPageView(logical);
    if (!hit.has_value()) {
        return false;
    }
    const Document& document = controller_.DocumentRef();
    const Page& page = document.PageAt(hit->pageIndex);
    const Vec2 pagePoint = ScreenToPage(*hit, logical, false);

    // Topmost first: LaTeX renders above text, which renders above strokes.
    for (std::size_t index = page.latexes.size(); index-- > 0U;) {
        const PageLatex& object = page.latexes[index];
        const latex::FormulaMetrics metrics = latex::Measure(latex::Parse(object));
        if (pagePoint.x >= object.x && pagePoint.x <= object.x + metrics.width
            && pagePoint.y >= object.y && pagePoint.y <= object.y + metrics.height) {
            if (controller_.BeginLatexEdit(hit->pageIndex, index)) {
                overlayPageIndex_ = hit->pageIndex;
                overlayTextIndex_.reset();
                overlayLatexIndex_ = index;
                ShowLatexOverlay(object);
                return true;
            }
        }
    }
    for (std::size_t index = page.texts.size(); index-- > 0U;) {
        const PageText& object = page.texts[index];
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(object, page.width);
        if (pagePoint.x >= bounds.left && pagePoint.x <= bounds.right
            && pagePoint.y >= bounds.top && pagePoint.y <= bounds.bottom) {
            if (controller_.BeginTextEdit(hit->pageIndex, index)) {
                overlayPageIndex_ = hit->pageIndex;
                overlayLatexIndex_.reset();
                overlayTextIndex_ = index;
                ShowTextOverlay(object);
                return true;
            }
        }
    }
    return false;
}

bool CanvasWidget::EditOverlayOwnsPoint(const QPointF& logical) const {
    if (textEditor_ != nullptr && textEditor_->isVisible() && textEditor_->geometry().contains(logical.toPoint())) {
        return true;
    }
    if (latexEditor_ != nullptr && latexEditor_->isVisible() && latexEditor_->geometry().contains(logical.toPoint())) {
        return true;
    }
    return false;
}

void CanvasWidget::SyncEditOverlay() {
    if (controller_.EditingTextIndex().has_value() && controller_.EditingTextObject() != nullptr) {
        if (overlayTextIndex_ == controller_.EditingTextIndex()
            && overlayPageIndex_ == controller_.EditingPageIndex()
            && textEditor_ != nullptr && textEditor_->isVisible()) {
            return;
        }
        overlayPageIndex_ = controller_.EditingPageIndex();
        overlayTextIndex_ = controller_.EditingTextIndex();
        overlayLatexIndex_.reset();
        ShowTextOverlay(*controller_.EditingTextObject());
        return;
    }
    if (controller_.EditingLatexIndex().has_value() && controller_.EditingLatexObject() != nullptr) {
        if (overlayLatexIndex_ == controller_.EditingLatexIndex()
            && overlayPageIndex_ == controller_.EditingPageIndex()
            && latexEditor_ != nullptr && latexEditor_->isVisible()) {
            return;
        }
        overlayPageIndex_ = controller_.EditingPageIndex();
        overlayLatexIndex_ = controller_.EditingLatexIndex();
        overlayTextIndex_.reset();
        ShowLatexOverlay(*controller_.EditingLatexObject());
        return;
    }
    HideOverlays();
}

void CanvasWidget::CommitActiveEdit() {
    const bool committingText = textEditor_ != nullptr && controller_.EditingTextIndex().has_value();
    const bool committingLatex = !committingText && latexEditor_ != nullptr && controller_.EditingLatexIndex().has_value();
    PageText edited {};
    QString latexSource;
    if (committingText) {
        edited = controller_.EditingTextObject() != nullptr ? *controller_.EditingTextObject() : PageText {};
        edited.content = ToUtf16(textEditor_->toPlainText());
        edited.styleSpans = text::CapturePageTextFormatting(*textEditor_->document(), edited, textEditScale_);
    } else if (committingLatex) {
        latexSource = latexEditor_->toPlainText();
    }
    // Drop the overlay state before notifying the controller: EditStateChanged is
    // delivered synchronously, and a consumer that gates on whether an editor owns
    // the keyboard must never observe an overlay that is already being closed.
    HideOverlays();
    if (committingText) {
        controller_.CommitTextEdit(std::move(edited));
    } else if (committingLatex) {
        controller_.CommitLatexEdit(latexSource);
    }
    update();
}

void CanvasWidget::CancelActiveEdit() {
    const bool cancellingText = controller_.EditingTextIndex().has_value();
    const bool cancellingLatex = controller_.EditingLatexIndex().has_value();
    // Same ordering as CommitActiveEdit: the overlay goes away before the
    // controller notifies its EditStateChanged consumers.
    HideOverlays();
    if (cancellingText) {
        controller_.CancelTextEdit();
    }
    if (cancellingLatex) {
        controller_.CancelLatexEdit();
    }
    update();
}

void CanvasWidget::EnsureTextEditor() {
    if (textEditor_ != nullptr) {
        return;
    }
    textEditor_ = new QTextEdit(this);
    textEditor_->setAcceptRichText(false);
    // The editor IS the live preview: frameless and transparent so the styled
    // text sits directly on the page (the committed object is excluded from
    // the cached surface while it is being edited).
    textEditor_->setFrameShape(QFrame::NoFrame);
    textEditor_->setStyleSheet(QStringLiteral("QTextEdit { background: transparent; border: none; }"));
    textEditor_->setAutoFillBackground(false);
    textEditor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    textEditor_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    textEditor_->installEventFilter(this);
    textEditor_->hide();
    connect(textEditor_, &QTextEdit::textChanged, this, [this]() {
        // Qt resets the current char format when the document empties, which
        // would drop the seeded object colour; re-seed it so the next typed
        // character still matches the committed style.
        if (textEditor_->document()->isEmpty() && controller_.EditingTextObject() != nullptr) {
            const PageText& object = *controller_.EditingTextObject();
            textEditor_->setCurrentCharFormat(text::CharFormatForStyle(
                { object.fontSize, object.color, object.fontFamily, object.bold, object.italic, object.underline },
                textEditScale_));
        }
        PositionOverlays();
    });
}

void CanvasWidget::EnsureLatexEditor() {
    if (latexEditor_ == nullptr) {
        latexEditor_ = new QPlainTextEdit(this);
        latexEditor_->setFrameShape(QFrame::Box);
        latexEditor_->installEventFilter(this);
        latexEditor_->hide();
        connect(latexEditor_, &QPlainTextEdit::textChanged, this, [this]() {
            RefreshLatexPopup();
            update();
        });
        connect(latexEditor_, &QPlainTextEdit::cursorPositionChanged, this, [this]() { RefreshLatexPopup(); });
    }
    if (latexPopup_ == nullptr) {
        latexPopup_ = new QListView(this);
        latexPopupModel_ = new QStringListModel(latexPopup_);
        latexPopup_->setModel(latexPopupModel_);
        latexPopup_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        latexPopup_->setUniformItemSizes(true);
        latexPopup_->hide();
    }
}

void CanvasWidget::ShowTextOverlay(const PageText& text) {
    EnsureTextEditor();
    const CanvasPageView view = overlayPageIndex_.has_value()
        ? CalculatePageView(*overlayPageIndex_)
        : CanvasPageView {};
    textEditScale_ = view.valid ? view.scale : 1.0f;
    const text::TextOffsetMap map = text::BuildTextOffsetMap(text.content);
    textEditor_->document()->setDocumentMargin(0.0);
    // Match the canvas renderer's text option (TextLayoutQt BuildDocument):
    // same wrap mode and paragraph alignment so the live preview wraps and
    // aligns exactly like the committed object.
    QTextOption option;
    option.setWrapMode(text.width > 0.0f
        ? QTextOption::WrapAtWordBoundaryOrAnywhere
        : QTextOption::NoWrap);
    option.setAlignment(TextAlignmentFor(text.alignment));
    textEditor_->document()->setDefaultTextOption(option);
    textEditor_->setLineWrapMode(text.width > 0.0f
        ? QTextEdit::FixedPixelWidth
        : QTextEdit::NoWrap);
    {
        QSignalBlocker blocker(textEditor_);
        textEditor_->setPlainText(FromUtf16(map.normalized));
        text::ApplyPageTextFormatting(*textEditor_->document(), text, textEditScale_);
        // Newly typed text takes the cursor's current format, not the document
        // default: seed it with the object's style so fresh input matches the
        // committed colour (and the toolbar's format readout).
        textEditor_->setCurrentCharFormat(text::CharFormatForStyle(
            { text.fontSize, text.color, text.fontFamily, text.bold, text.italic, text.underline },
            textEditScale_));
    }
    textEditor_->show();
    textEditor_->raise();
    textEditor_->setFocus(Qt::OtherFocusReason);
    PositionOverlays();
    update();
}

void CanvasWidget::ShowLatexOverlay(const PageLatex& object) {
    EnsureLatexEditor();
    {
        QSignalBlocker blocker(latexEditor_);
        latexEditor_->setPlainText(FromUtf16(object.content));
        QTextCursor cursor = latexEditor_->textCursor();
        cursor.setPosition(static_cast<int>(object.content.size()));
        latexEditor_->setTextCursor(cursor);
    }
    latexEditor_->show();
    latexEditor_->raise();
    latexEditor_->setFocus(Qt::OtherFocusReason);
    RefreshLatexPopup();
    PositionOverlays();
    update();
}

void CanvasWidget::HideOverlays() {
    overlayPageIndex_.reset();
    overlayTextIndex_.reset();
    overlayLatexIndex_.reset();
    if (latexPopup_ != nullptr) {
        latexPopup_->hide();
    }
    if (textEditor_ != nullptr) {
        textEditor_->hide();
    }
    if (latexEditor_ != nullptr) {
        latexEditor_->hide();
    }
    setFocus(Qt::OtherFocusReason);
}

void CanvasWidget::PositionOverlays() {
    if (!overlayPageIndex_.has_value()) {
        return;
    }
    const Document& document = controller_.DocumentRef();
    if (*overlayPageIndex_ >= document.PageCount()) {
        return;
    }
    const CanvasPageView view = CalculatePageView(*overlayPageIndex_);
    if (!view.valid || view.scale <= 0.0f) {
        return;
    }

    if (textEditor_ != nullptr && textEditor_->isVisible() && controller_.EditingTextObject() != nullptr) {
        const PageText& object = *controller_.EditingTextObject();
        // The editor is the live preview: fonts are formatted at the view
        // scale, so a zoom change mid-edit re-applies formatting at the new
        // scale (capture/re-apply round-trips the user's current content).
        if (std::abs(view.scale - textEditScale_) > 0.0001f) {
            PageText live = object;
            live.content = ToUtf16(textEditor_->toPlainText());
            live.styleSpans = text::CapturePageTextFormatting(*textEditor_->document(), live, textEditScale_);
            const int cursorPosition = textEditor_->textCursor().position();
            {
                QSignalBlocker blocker(textEditor_);
                text::ApplyPageTextFormatting(*textEditor_->document(), live, view.scale);
            }
            QTextCursor cursor = textEditor_->textCursor();
            cursor.setPosition(std::min(cursorPosition, static_cast<int>(textEditor_->document()->characterCount() - 1)));
            textEditor_->setTextCursor(cursor);
            textEditScale_ = view.scale;
        }
        const qreal left = view.rect.left() + (static_cast<qreal>(object.x) * view.scale);
        const qreal top = view.rect.top() + (static_cast<qreal>(object.y) * view.scale);
        const qreal width = std::max<qreal>(40.0,
            object.width > 0.0f
                ? static_cast<qreal>(object.width) * view.scale
                : textEditor_->document()->idealWidth() + 4.0);
        const qreal height = std::max<qreal>(8.0, textEditor_->document()->size().height() + 4.0);
        const QRect geometry(QPoint(static_cast<int>(std::lround(left)), static_cast<int>(std::lround(top))),
                             QSize(static_cast<int>(std::lround(width)), static_cast<int>(std::lround(height))));
        if (textEditor_->geometry() != geometry) {
            textEditor_->setGeometry(geometry);
        }
        // Committed text is clipped to the page rect; mask the editor the same
        // way so the live preview cannot draw past the page edge.
        const QRect clip = geometry.intersected(view.rect.toAlignedRect()).translated(-geometry.topLeft());
        textEditor_->setMask(QRegion(clip));
    }

    if (latexEditor_ != nullptr && latexEditor_->isVisible() && controller_.EditingLatexObject() != nullptr) {
        const PageLatex& object = *controller_.EditingLatexObject();
        const latex::FormulaMetrics metrics = latex::Measure(latex::Parse(object));
        const qreal objectLeft = view.rect.left() + (static_cast<qreal>(object.x) * view.scale);
        const qreal objectTop = view.rect.top() + (static_cast<qreal>(object.y) * view.scale);
        const qreal objectHeight = std::max<qreal>(8.0, static_cast<qreal>(metrics.height) * view.scale);
        const qreal width = std::max<qreal>(kTextEditorMinWidth, static_cast<qreal>(metrics.width) * view.scale);

        // Source box below the equation, or above it when there is no room.
        const int belowTop = static_cast<int>(std::lround(objectTop + objectHeight + kLatexSourceBoxGap));
        const bool placeBelow = belowTop + kLatexSourceBoxHeight <= height();
        const int top = placeBelow
            ? belowTop
            : static_cast<int>(std::lround(objectTop)) - kLatexSourceBoxHeight - kLatexSourceBoxGap;
        const QRect geometry(QPoint(static_cast<int>(std::lround(objectLeft)), top),
                             QSize(static_cast<int>(std::lround(width)), kLatexSourceBoxHeight));
        if (latexEditor_->geometry() != geometry) {
            latexEditor_->setGeometry(geometry);
        }

        if (latexPopup_ != nullptr && latexPopup_->isVisible()) {
            const int popupTop = placeBelow ? geometry.bottom() + 2 : geometry.top() - kLatexPopupHeight - 2;
            latexPopup_->setGeometry(QRect(geometry.left(), popupTop, kLatexPopupWidth, kLatexPopupHeight));
            latexPopup_->raise();
        }
    }
}

void CanvasWidget::ApplyTextFormatToSelection(const PageTextStyle& style) {
    if (textEditor_ == nullptr) {
        return;
    }
    QTextCursor cursor = textEditor_->textCursor();
    if (!cursor.hasSelection()) {
        return;
    }
    const QTextCharFormat format = text::CharFormatForStyle(style, textEditScale_);
    cursor.mergeCharFormat(format);
    textEditor_->mergeCurrentCharFormat(format);
    PositionOverlays();
}

PageText CanvasWidget::CaptureEditedText() const {
    PageText edited = controller_.EditingTextObject() != nullptr ? *controller_.EditingTextObject() : PageText {};
    if (textEditor_ != nullptr) {
        edited.content = ToUtf16(textEditor_->toPlainText());
        edited.styleSpans = text::CapturePageTextFormatting(*textEditor_->document(), edited, textEditScale_);
    }
    return edited;
}

QString CanvasWidget::CaptureEditedLatexSource() const {
    return latexEditor_ != nullptr ? latexEditor_->toPlainText() : QString();
}

void CanvasWidget::SetLatexSource(const QString& source, int cursorPosition) {
    if (latexEditor_ == nullptr) {
        return;
    }
    QSignalBlocker blocker(latexEditor_);
    latexEditor_->setPlainText(source);
    QTextCursor cursor = latexEditor_->textCursor();
    cursor.setPosition(std::min(cursorPosition, static_cast<int>(source.size())));
    latexEditor_->setTextCursor(cursor);
    RefreshLatexPopup();
    update();
}

QString CanvasWidget::LatexSource() const {
    return latexEditor_ != nullptr ? latexEditor_->toPlainText() : QString();
}

bool CanvasWidget::LatexPopupVisible() const {
    return latexPopup_ != nullptr && latexPopup_->isVisible();
}

QStringList CanvasWidget::LatexPopupItems() const {
    return latexPopupModel_ != nullptr ? latexPopupModel_->stringList() : QStringList();
}

int CanvasWidget::LatexPopupRow() const {
    return latexPopup_ != nullptr ? latexPopup_->currentIndex().row() : -1;
}

void CanvasWidget::SetLatexPopupRow(int row) {
    if (latexPopup_ == nullptr || latexPopupModel_ == nullptr || latexPopupModel_->rowCount() == 0) {
        return;
    }
    const int wrapped = ((row % latexPopupModel_->rowCount()) + latexPopupModel_->rowCount()) % latexPopupModel_->rowCount();
    const QModelIndex index = latexPopupModel_->index(wrapped, 0);
    latexPopup_->setCurrentIndex(index);
    latexPopup_->scrollTo(index);
}

void CanvasWidget::RefreshLatexPopup() {
    if (latexEditor_ == nullptr || latexPopup_ == nullptr || !latexEditor_->isVisible()) {
        return;
    }

    const std::u16string content = ToUtf16(latexEditor_->toPlainText());
    const std::size_t cursor = static_cast<std::size_t>(latexEditor_->textCursor().position());
    const text::LatexCompletionDecision decision = text::LatexCompletionFor(content, cursor);
    latexSuggestions_ = decision.suggestions;

    if (!decision.show || latexSuggestions_.empty()) {
        latexPopup_->hide();
        return;
    }

    QStringList items;
    items.reserve(static_cast<qsizetype>(latexSuggestions_.size()));
    for (const latex::CompletionItem& item : latexSuggestions_) {
        items.append(FromUtf16(item.command));
    }
    latexPopupModel_->setStringList(items);
    SetLatexPopupRow(0);
    latexPopup_->show();
    PositionOverlays();
    latexPopup_->raise();
}

bool CanvasWidget::AcceptLatexSuggestion() {
    if (!LatexPopupVisible() || latexEditor_ == nullptr) {
        return false;
    }
    const int row = LatexPopupRow();
    if (row < 0 || static_cast<std::size_t>(row) >= latexSuggestions_.size()) {
        return false;
    }

    const std::u16string content = ToUtf16(latexEditor_->toPlainText());
    const std::size_t cursor = static_cast<std::size_t>(latexEditor_->textCursor().position());
    const std::optional<text::LatexEditResult> result =
        text::ApplyLatexCompletion(content, cursor, latexSuggestions_[static_cast<std::size_t>(row)]);
    if (!result.has_value()) {
        return false;
    }

    SetLatexSource(FromUtf16(result->content), static_cast<int>(result->cursor));
    RefreshLatexPopup();
    return true;
}

// --- editor events ---------------------------------------------------------

bool CanvasWidget::HandleLatexPairKey(QPlainTextEdit& editor, const QString& text) {
    if (text.size() != 1) {
        return false;
    }
    const char16_t typed = static_cast<char16_t>(text.at(0).unicode());
    if (typed > 0x7FU) {
        return false;
    }

    const std::u16string content = ToUtf16(editor.toPlainText());
    const QTextCursor cursor = editor.textCursor();
    const std::optional<text::LatexEditResult> result = text::ApplyLatexAutoPair(
        content,
        static_cast<std::uint32_t>(cursor.selectionStart()),
        static_cast<std::uint32_t>(cursor.selectionEnd()),
        typed);
    if (!result.has_value()) {
        return false;
    }

    SetLatexSource(FromUtf16(result->content), static_cast<int>(result->cursor));
    return true;
}

bool CanvasWidget::HandleEditorKey(QObject* watched, QKeyEvent* event) {
    if (watched != textEditor_ && watched != latexEditor_) {
        return false;
    }
    const bool latex = watched == latexEditor_;

    // Pair characters arrive with Shift on most layouts ("{", "(", "$"), so
    // only the modifiers that mean "not a plain insertion" are excluded.
    const Qt::KeyboardModifiers chord = event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (latex && latexEditor_ != nullptr
        && !event->text().isEmpty()
        && chord == Qt::NoModifier) {
        // Pair characters take precedence over the popup: "{" still wraps.
        if (HandleLatexPairKey(*latexEditor_, event->text())) {
            return true;
        }
    }

    if (latex && LatexPopupVisible()) {
        switch (text::LatexSuggestionActionForKey(static_cast<Qt::Key>(event->key()))) {
        case text::LatexSuggestionKeyAction::Accept:
            if (AcceptLatexSuggestion()) {
                return true;
            }
            break;
        case text::LatexSuggestionKeyAction::SelectPrevious:
            SetLatexPopupRow(LatexPopupRow() - 1);
            return true;
        case text::LatexSuggestionKeyAction::SelectNext:
            SetLatexPopupRow(LatexPopupRow() + 1);
            return true;
        case text::LatexSuggestionKeyAction::PageUp:
            SetLatexPopupRow(LatexPopupRow() - kLatexPopupPageStep);
            return true;
        case text::LatexSuggestionKeyAction::PageDown:
            SetLatexPopupRow(LatexPopupRow() + kLatexPopupPageStep);
            return true;
        case text::LatexSuggestionKeyAction::Dismiss:
            latexPopup_->hide();
            return true;
        case text::LatexSuggestionKeyAction::None:
            break;
        }
    }

    const bool commitChord = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && event->modifiers().testFlag(Qt::ControlModifier);
    if (commitChord) {
        CommitActiveEdit();
        return true;
    }
    if (event->key() == Qt::Key_Escape) {
        CancelActiveEdit();
        return true;
    }

    if (event->key() == Qt::Key_Backspace && event->modifiers().testFlag(Qt::ControlModifier)) {
        auto* editor = latex ? static_cast<QPlainTextEdit*>(latexEditor_) : static_cast<QPlainTextEdit*>(nullptr);
        QTextCursor cursor = latex ? editor->textCursor() : textEditor_->textCursor();
        const std::u16string content = latex
            ? ToUtf16(editor->toPlainText())
            : ToUtf16(textEditor_->toPlainText());
        const text::TextRange range = text::WordDeletionRange(content, static_cast<std::uint32_t>(cursor.position()));
        if (range.end > range.start) {
            cursor.setPosition(static_cast<int>(range.start));
            cursor.setPosition(static_cast<int>(range.end), QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            return true;
        }
    }
    return false;
}

bool CanvasWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == textEditor_ || watched == latexEditor_) {
        if (event->type() == QEvent::KeyPress) {
            if (HandleEditorKey(watched, static_cast<QKeyEvent*>(event))) {
                return true;
            }
        }
        if (event->type() == QEvent::Wheel) {
            // Ctrl-wheel belongs to the editor (font zoom) and must not reach the
            // canvas as a page zoom (plan step 5.3).
            auto* wheel = static_cast<QWheelEvent*>(event);
            if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
                wheel->accept();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

PointerSample CanvasWidget::SampleFromEvent(const QMouseEvent& event) {
    PointerSample sample;
    sample.position = event.position();
    sample.button = event.button();
    sample.buttons = event.buttons();
    sample.modifiers = event.modifiers();
    sample.timestamp = static_cast<quint64>(event.timestamp());
    return sample;
}

void CanvasWidget::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    // No chrome bands live inside this widget (the toolbar/sidebar are separate
    // widgets), so the event goes straight to the router (MessageLoop.cpp:189-211
    // offers bands first in the legacy window; here the widget boundary does).
    lastPointerPosition_ = event->position();
    HandlePointerPress(SampleFromEvent(*event));
    event->accept();
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    lastPointerPosition_ = event->position();
    HandlePointerMove(SampleFromEvent(*event));
    event->accept();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    lastPointerPosition_ = event->position();
    HandlePointerRelease(SampleFromEvent(*event));
    event->accept();
}

void CanvasWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        CancelActiveEdit();
        HandlePointerCancel(CancelReason::Escape);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete) {
        // Deleting the selection is a document mutation, so it goes through the
        // controller like the cut path (QtMainWindow::CutSelection).
        if (controller_.RemoveSelection()) {
            event->accept();
            return;
        }
    }
    InteractionModifiers modifiers = router_->Modifiers();
    if (event->key() == Qt::Key_A) {
        modifiers.straightLine = true;
    } else if (event->key() == Qt::Key_Space) {
        modifiers.dotted = true;
    }
    modifiers.snapLine = event->modifiers().testFlag(Qt::ShiftModifier);
    modifiers.oval = event->modifiers().testFlag(Qt::ControlModifier);
    SetInteractionModifiers(modifiers);
    QWidget::keyPressEvent(event);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* event) {
    InteractionModifiers modifiers = router_->Modifiers();
    if (event->key() == Qt::Key_A) {
        modifiers.straightLine = false;
    } else if (event->key() == Qt::Key_Space) {
        modifiers.dotted = false;
    }
    modifiers.snapLine = event->modifiers().testFlag(Qt::ShiftModifier);
    modifiers.oval = event->modifiers().testFlag(Qt::ControlModifier);
    SetInteractionModifiers(modifiers);
    QWidget::keyReleaseEvent(event);
}

void CanvasWidget::focusOutEvent(QFocusEvent* event) {
    // Legacy focus-loss/lost-grab finishing (Session.cpp:95): commit the partial
    // interaction once and release capture. Idempotent in the router.
    HandlePointerCancel(CancelReason::FocusLost);
    QWidget::focusOutEvent(event);
}

void CanvasWidget::wheelEvent(QWheelEvent* event) {
    WheelSample sample;
    sample.position = event->position();
    sample.angleDeltaY = event->angleDelta().y();
    sample.angleDeltaX = event->angleDelta().x();
    sample.pixelDelta = event->pixelDelta();
    sample.modifiers = event->modifiers();
    if (router_->HandleWheel(sample)) {
        event->accept();
        return;
    }
    event->accept();
}

// --- pointer entry points and InteractionHost -------------------------------


void CanvasWidget::HandlePointerPress(const PointerSample& sample) {
    // Pressing outside the editor overlay commits the active edit (the overlay
    // owns clicks inside itself because it is a child widget).
    if (EditOverlayActive() && !EditOverlayOwnsPoint(sample.position)) {
        CommitActiveEdit();
    }
    router_->HandlePointerPress(sample);
    update();
}

void CanvasWidget::HandlePointerMove(const PointerSample& sample) {
    router_->HandlePointerMove(sample);
}

void CanvasWidget::HandlePointerRelease(const PointerSample& sample) {
    router_->HandlePointerRelease(sample);
    update();
}

void CanvasWidget::HandlePointerCancel(CancelReason reason) {
    // Idempotent: the router commits the partial *pointer* interaction exactly
    // once and releases capture (legacy FinishDocumentInteraction).  Active
    // text/LaTeX edits are not cancelled here: they own their own focus, and a
    // canvas focus change must not discard typed content (Escape cancels).
    static_cast<void>(reason);
    router_->FinishInteraction();
    update();
}

std::optional<PageHit> CanvasWidget::HitTestPage(const QPointF& logical) const {
    const std::optional<CanvasPageView> view = HitTestPageView(logical);
    if (!view.has_value()) {
        return std::nullopt;
    }
    return PageHit { view->pageIndex, view->scale };
}

Vec2 CanvasWidget::ScreenToPage(const QPointF& logical, bool clampToPage) const {
    const std::optional<CanvasPageView> view = HitTestPageView(logical);
    if (!view.has_value()) {
        return {};
    }
    return ScreenToPage(*view, logical, clampToPage);
}

bool CanvasWidget::IsPointInCanvas(const QPointF& logical) const {
    return VisibleCanvasRect().contains(logical);
}

bool CanvasWidget::HasSelection() const {
    return controller_.HasSelection();
}

bool CanvasWidget::CursorInteractsWithSelection(const QPointF& logical) const {
    const std::optional<CanvasPageView> view = HitTestPageView(logical);
    if (!view.has_value()) {
        return false;
    }
    const std::optional<SelectionBounds> bounds = SelectionBoundsForPage(view->pageIndex);
    if (!bounds.has_value()) {
        return false;
    }
    // The rotate handle sits outside the frame, and a Select-tool press here
    // must reach the transform probe instead of clearing the selection; the
    // router only calls BeginMoveSelection when this returns true.
    if (HitTestSelectionHandle(logical).has_value()) {
        return true;
    }

    // Legacy App::CursorHitsSelectionMoveGrip (Selection.cpp:711): the grip is
    // the frame's centre square; selected objects also count (Selection.cpp:823).
    const Vec2 pagePoint = ScreenToPage(*view, logical, false);
    const float gripRadius = SelectionHandleHitRadiusPixels(96) / std::max(0.1f, view->scale);
    const Vec2 center {
        (bounds->left + bounds->right) * 0.5f,
        (bounds->top + bounds->bottom) * 0.5f,
    };
    if (pagePoint.x >= center.x - gripRadius && pagePoint.x <= center.x + gripRadius
        && pagePoint.y >= center.y - gripRadius && pagePoint.y <= center.y + gripRadius) {
        return true;
    }
    if (pagePoint.x >= bounds->left - gripRadius && pagePoint.x <= bounds->right + gripRadius
        && pagePoint.y >= bounds->top - gripRadius && pagePoint.y <= bounds->bottom + gripRadius) {
        return true;
    }
    return false;
}

std::optional<ObjectRef> CanvasWidget::TopmostObjectAt(const QPointF& logical, bool includeStrokes) const {
    const std::optional<CanvasPageView> view = HitTestPageView(logical);
    if (!view.has_value()) {
        return std::nullopt;
    }
    const Document& document = controller_.DocumentRef();
    const Page& page = document.PageAt(view->pageIndex);
    const Vec2 pagePoint = ScreenToPage(*view, logical, false);

    // Topmost first: LaTeX, text, images, strokes (reference layer order).
    for (std::size_t index = page.latexes.size(); index-- > 0U;) {
        if (LatexHitsPoint(page.latexes[index], page.width, pagePoint)) {
            return ObjectRef { view->pageIndex, ObjectKind::Latex, index };
        }
    }
    for (std::size_t index = page.texts.size(); index-- > 0U;) {
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(page.texts[index], page.width);
        if (pagePoint.x >= bounds.left && pagePoint.x <= bounds.right
            && pagePoint.y >= bounds.top && pagePoint.y <= bounds.bottom) {
            return ObjectRef { view->pageIndex, ObjectKind::Text, index };
        }
    }
    for (std::size_t index = page.images.size(); index-- > 0U;) {
        if (ImageHitsPoint(page.images[index], pagePoint)) {
            return ObjectRef { view->pageIndex, ObjectKind::Image, index };
        }
    }
    if (includeStrokes) {
        const float radius = 6.0f / std::max(0.1f, view->scale);
        for (std::size_t index = page.strokes.size(); index-- > 0U;) {
            if (StrokeHitsPoint(page.strokes[index], pagePoint, radius, controller_.InputSettings().strokeSmoothing)) {
                return ObjectRef { view->pageIndex, ObjectKind::Stroke, index };
            }
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>> CanvasWidget::HitTestText(const QPointF& logical) const {
    const std::optional<ObjectRef> object = TopmostObjectAt(logical, false);
    if (!object.has_value() || object->kind != ObjectKind::Text) {
        return std::nullopt;
    }
    return std::make_pair(object->pageIndex, object->objectIndex);
}

std::optional<std::pair<std::size_t, std::size_t>> CanvasWidget::HitTestLatex(const QPointF& logical) const {
    const std::optional<ObjectRef> object = TopmostObjectAt(logical, false);
    if (!object.has_value() || object->kind != ObjectKind::Latex) {
        return std::nullopt;
    }
    return std::make_pair(object->pageIndex, object->objectIndex);
}

std::optional<std::size_t> CanvasWidget::PageWithPdfTextAt(const QPointF& logical) const {
    // PDF text selection is not ported in this slice (plan step 5.2); returning
    // nullopt keeps ordinary tool routing without a fake hit.
    static_cast<void>(logical);
    return std::nullopt;
}

std::optional<SelectionHit> CanvasWidget::HitTestSelectionObject(const QPointF& logical, std::size_t pageIndex) const {
    const std::optional<CanvasPageView> view = HitTestPageView(logical);
    if (!view.has_value() || view->pageIndex != pageIndex) {
        return std::nullopt;
    }
    const std::optional<ObjectRef> object = TopmostObjectAt(logical, true);
    if (!object.has_value()) {
        return std::nullopt;
    }

    const PageSelection& selection = controller_.SelectionFor(pageIndex);
    bool selected = false;
    switch (object->kind) {
    case ObjectKind::Stroke:
        selected = std::find(selection.strokes.begin(), selection.strokes.end(), object->objectIndex) != selection.strokes.end();
        break;
    case ObjectKind::Image:
        selected = std::find(selection.images.begin(), selection.images.end(), object->objectIndex) != selection.images.end();
        break;
    case ObjectKind::Text:
        selected = std::find(selection.texts.begin(), selection.texts.end(), object->objectIndex) != selection.texts.end();
        break;
    case ObjectKind::Latex:
        selected = std::find(selection.latexes.begin(), selection.latexes.end(), object->objectIndex) != selection.latexes.end();
        break;
    }

    SelectionHit hit;
    hit.kind = object->kind;
    hit.objectIndex = object->objectIndex;
    hit.selected = selected;
    hit.moveGrip = CursorInteractsWithSelection(logical);
    return hit;
}

std::optional<TransformRequest> CanvasWidget::HitTestSelectionHandle(const QPointF& logical) const {
    // The press point is cached for BeginTransform, which the router calls
    // immediately after this probe and which needs the interaction's origin to
    // measure a rotation from.
    lastHandleProbeLogical_ = logical;

    const std::optional<CanvasPageView> view = HitTestPageView(logical);
    if (!view.has_value() || !view->valid) {
        return std::nullopt;
    }
    const std::optional<SelectionBounds> bounds = SelectionBoundsForPage(view->pageIndex);
    if (!bounds.has_value()) {
        return std::nullopt;
    }

    const QRectF frame(
        view->rect.left() + (static_cast<qreal>(bounds->left) * view->scale),
        view->rect.top() + (static_cast<qreal>(bounds->top) * view->scale),
        static_cast<qreal>(bounds->right - bounds->left) * view->scale,
        static_cast<qreal>(bounds->bottom - bounds->top) * view->scale);

    const qreal hitRadius = static_cast<qreal>(SelectionHandleHitRadiusPixels(96));
    const qreal hitRadiusSquared = hitRadius * hitRadius;
    const auto hits = [&logical, hitRadiusSquared](const QPointF& position) {
        const qreal dx = logical.x() - position.x();
        const qreal dy = logical.y() - position.y();
        return ((dx * dx) + (dy * dy)) <= hitRadiusSquared;
    };

    // The transform spans the whole selection on the page, so `target` and
    // `objectIndex` keep their Stroke/0 defaults and are ignored by the canvas.
    TransformRequest request;
    request.pageIndex = view->pageIndex;

    std::printf("[dbg] frame %.1f,%.1f %.1fx%.1f logical %.1f,%.1f radius %.1f\n",
        frame.left(), frame.top(), frame.width(), frame.height(), logical.x(), logical.y(), hitRadius);
    // The rotate handle's radius overlaps the top edge handle's, so it is
    // checked first and wins a tie.
    if (hits(SelectionRotateHandlePosition(frame))) {
        request.handle = TransformHandle::Rotate;
        return request;
    }

    const std::array<QPointF, 8> positions = SelectionHandlePositions(frame);
    constexpr std::array<TransformHandle, 8> kHandles {{
        TransformHandle::TopLeft,
        TransformHandle::TopRight,
        TransformHandle::BottomRight,
        TransformHandle::BottomLeft,
        TransformHandle::Top,
        TransformHandle::Bottom,
        TransformHandle::Left,
        TransformHandle::Right,
    }};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        if (hits(positions[index])) {
            request.handle = kHandles[index];
            return request;
        }
    }
    return std::nullopt;
}

std::vector<ObjectRef> CanvasWidget::EraseTargetsAt(const QPointF& logical) const {
    std::vector<ObjectRef> targets;
    const std::optional<CanvasPageView> view = HitTestPageView(logical);
    if (!view.has_value()) {
        return targets;
    }
    const Document& document = controller_.DocumentRef();
    const Page& page = document.PageAt(view->pageIndex);
    const Vec2 pagePoint = ScreenToPage(*view, logical, false);

    // Reference App::EraseAt: the eraser radius is max(3.5, eraseBrushSize * 4)
    // logical pixels (App::EraserRadiusPixels, App.cpp:7426-7428) scaled into
    // page units (App.cpp:5736).
    const float radiusPage = std::max(kEraserMinRadiusPixels, eraseBrushSize_ * 4.0f) / std::max(0.1f, view->scale);
    const Vec2 a { pagePoint.x - radiusPage, pagePoint.y - radiusPage };
    const Vec2 b { pagePoint.x + radiusPage, pagePoint.y + radiusPage };

    for (std::size_t index = 0; index < page.images.size(); ++index) {
        if (ImageIntersectsRectSelection(page.images[index], a, b)) {
            targets.push_back(ObjectRef { view->pageIndex, ObjectKind::Image, index });
        }
    }
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(page.texts[index], page.width);
        if (!(bounds.right < a.x || bounds.left > b.x || bounds.bottom < a.y || bounds.top > b.y)) {
            targets.push_back(ObjectRef { view->pageIndex, ObjectKind::Text, index });
        }
    }
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        if (LatexIntersectsRectSelection(page.latexes[index], page.width, a, b)) {
            targets.push_back(ObjectRef { view->pageIndex, ObjectKind::Latex, index });
        }
    }
    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        if (StrokeHitsPoint(page.strokes[index], pagePoint, radiusPage, controller_.InputSettings().strokeSmoothing)) {
            targets.push_back(ObjectRef { view->pageIndex, ObjectKind::Stroke, index });
        }
    }
    return targets;
}

void CanvasWidget::ZoomByWheelSteps(int steps, const QPointF& anchor) {
    ZoomBySteps(steps, anchor);
}

void CanvasWidget::ScrollBy(const QPointF& logicalDelta) {
    // Reference 90 ms smooth scroll (plan step 5.3).
    StartSmoothScroll(logicalDelta);
}

void CanvasWidget::SetViewPage(std::size_t pageIndex) {
    controller_.SetCurrentPageIndex(pageIndex);
}

void CanvasWidget::SetCursor(CursorKind cursor) {
    cursorKind_ = cursor;
    ApplyCursor();
}

void CanvasWidget::SetTool(Tool tool) {
    router_->SetTool(tool);
    SetCursor(InteractionRouter::CursorForTool(tool));
}

void CanvasWidget::SetDrawCursorSettings(settings::DrawCursorKind kind, float logicalPixels, std::uint32_t argbColor) {
    // SettingsStoreQt already clamps the persisted value; clamp again so a
    // programmatic set cannot build a degenerate pixmap.
    const float clamped = std::clamp(logicalPixels, kMinDrawCursorDiameter, kMaxDrawCursorDiameter);
    if (kind == drawCursorKind_ && std::abs(clamped - drawCursorSize_) < 0.001f && argbColor == drawCursorColor_) {
        return;
    }
    drawCursorKind_ = kind;
    drawCursorSize_ = clamped;
    drawCursorColor_ = argbColor;
    // The cached pixmap encodes the old appearance; the next apply rebuilds it.
    drawCursor_.reset();
    drawCursorDevicePixelRatio_ = 0.0;
    if (cursorKind_ == CursorKind::Draw) {
        ApplyCursor();
    }
}

void CanvasWidget::SetEraseBrushSize(float brushSize) {
    // No clamp: App::EraserRadiusPixels floors the value at the minimum radius
    // (App.cpp:7426-7428), so a degenerate brush size cannot build a degenerate
    // pixmap.
    if (std::abs(brushSize - eraseBrushSize_) < 0.001f) {
        return;
    }
    eraseBrushSize_ = brushSize;
    // The cached pixmap encodes the old diameter; the next apply rebuilds it.
    eraserCursor_.reset();
    eraserCursorDevicePixelRatio_ = 0.0;
    if (cursorKind_ == CursorKind::Eraser) {
        ApplyCursor();
    }
}

QCursor CanvasWidget::BuildDrawCursor() const {
    // Reference App::CreateBrushCursor (App.cpp:468-577): a disc of the active
    // palette colour - App::DrawCursorForCurrentSize (App.cpp:7471-7489) passes
    // the palette slot, or opaque black for an empty palette - whose outer edge
    // is the configured diameter with an inset black ring and the hotspot at its
    // centre.  The black ring keeps the disc visible whatever colour the palette
    // slot carries.  The image must be painted with an alpha channel: a default
    // QPixmap is opaque, and an opaque cursor would show the drawing tool as a
    // black square.
    constexpr int kMarginPixels = 1;          // antialiasing bleed room, device px
    const qreal ratio = devicePixelRatioF();
    const qreal deviceDiameter = std::max<qreal>(1.0, static_cast<qreal>(drawCursorSize_) * ratio);
    const int extent = static_cast<int>(std::lround(deviceDiameter)) + (kMarginPixels * 2);

    QImage image(extent, extent, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal penWidth = std::max<qreal>(1.0, kEraserOutlinePixels * ratio);
        const QRectF outer(
            static_cast<qreal>(kMarginPixels),
            static_cast<qreal>(kMarginPixels),
            deviceDiameter,
            deviceDiameter);
        painter.setBrush(QColor::fromRgba(drawCursorColor_));
        painter.setPen(QPen(Qt::black, penWidth));
        // The pen is centred on the path, so insetting the path by half the pen
        // width puts the stroke's outer edge exactly on the configured diameter.
        painter.drawEllipse(outer.adjusted(penWidth * 0.5, penWidth * 0.5, -penWidth * 0.5, -penWidth * 0.5));
        painter.end();
    }

    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(ratio);

    // QCursor's hotspot is in DEVICE-INDEPENDENT pixels: Qt multiplies it by the
    // pixmap's device pixel ratio when it builds the native cursor.  The centre
    // of an `extent`-device-pixel pixmap is therefore extent / (2 * ratio), not
    // extent / 2 (which would place the hotspot beyond the bottom-right corner).
    const int hotspot = static_cast<int>(std::lround(extent / (2.0 * ratio)));
    return QCursor(pixmap, hotspot, hotspot);
}

QCursor CanvasWidget::BuildEraserCursor() const {
    // Reference App::EraserCursorForCurrentSize (App.cpp:7459-7468):
    // CreateSquareBrushCursor(diameter, RGB(255,255,255)) (App.cpp:579-691) is a
    // white-filled square with an inset black kEraserOutlinePixels outline and a
    // centred hotspot, where the diameter is App::EraserDiameterPixels
    // (App.cpp:7430-7432): max(3.5, eraseBrushSize * 4) * 2 logical pixels.
    constexpr int kMarginPixels = 1;          // antialiasing bleed room, device px
    const qreal ratio = devicePixelRatioF();
    const qreal logicalSide =
        std::max<qreal>(kEraserMinRadiusPixels, static_cast<qreal>(eraseBrushSize_) * 4.0) * 2.0;
    const qreal deviceSide = std::max<qreal>(1.0, logicalSide * ratio);
    const int extent = static_cast<int>(std::lround(deviceSide)) + (kMarginPixels * 2);

    QImage image(extent, extent, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal penWidth = std::max<qreal>(1.0, kEraserOutlinePixels * ratio);
        const QRectF outer(
            static_cast<qreal>(kMarginPixels),
            static_cast<qreal>(kMarginPixels),
            deviceSide,
            deviceSide);
        painter.setBrush(Qt::white);
        painter.setPen(QPen(Qt::black, penWidth));
        // Same half-pen inset as the brush disc: the outline's outer edge lands
        // exactly on the eraser diameter.
        painter.drawRect(outer.adjusted(penWidth * 0.5, penWidth * 0.5, -penWidth * 0.5, -penWidth * 0.5));
        painter.end();
    }

    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(ratio);

    // Device-independent hotspot, same rule as BuildDrawCursor.
    const int hotspot = static_cast<int>(std::lround(extent / (2.0 * ratio)));
    return QCursor(pixmap, hotspot, hotspot);
}

void CanvasWidget::ApplyCursor() {
    switch (cursorKind_) {
    case CursorKind::Arrow:
        // The configured default: the plain system arrow, never a custom pixmap.
        setCursor(Qt::ArrowCursor);
        return;
    case CursorKind::Cross:
        setCursor(Qt::CrossCursor);
        return;
    case CursorKind::OpenHand:
        setCursor(Qt::OpenHandCursor);
        return;
    case CursorKind::ClosedHand:
        setCursor(Qt::ClosedHandCursor);
        return;
    case CursorKind::Text:
        setCursor(Qt::IBeamCursor);
        return;
    case CursorKind::Move:
        setCursor(Qt::SizeAllCursor);
        return;
    case CursorKind::Eraser: {
        // Reference App::EraserCursorForCurrentSize (App.cpp:7459-7468): the
        // erase tool shows the brush-sized white square rather than a system
        // cursor, so the user can see what the erase radius will hit.
        const qreal ratio = devicePixelRatioF();
        if (!eraserCursor_.has_value() || eraserCursorDevicePixelRatio_ != ratio) {
            eraserCursor_ = BuildEraserCursor();
            eraserCursorDevicePixelRatio_ = ratio;
        }
        setCursor(eraserCursor_.has_value() ? *eraserCursor_ : QCursor(Qt::CrossCursor));
        return;
    }
    case CursorKind::Draw:
        break;
    }

    // Reference App::DrawCursorForCurrentSize (App.cpp:7465-7485): the precision
    // crosshair is the system cross, otherwise the configured brush disc.
    if (drawCursorKind_ == settings::DrawCursorKind::PrecisionCrosshair) {
        setCursor(Qt::CrossCursor);
        return;
    }
    const qreal ratio = devicePixelRatioF();
    if (!drawCursor_.has_value() || drawCursorDevicePixelRatio_ != ratio) {
        drawCursor_ = BuildDrawCursor();
        drawCursorDevicePixelRatio_ = ratio;
    }
    setCursor(drawCursor_.has_value() ? *drawCursor_ : QCursor(Qt::CrossCursor));
}

void CanvasWidget::enterEvent(QEnterEvent* event) {
    ApplyCursor();
    QWidget::enterEvent(event);
}

void CanvasWidget::leaveEvent(QEvent* event) {
    ApplyCursor();
    QWidget::leaveEvent(event);
}

void CanvasWidget::InvalidateCanvas() {
    // Repaint request, not a cache drop. The page rasters are keyed and
    // signature-checked by the renderer, so a content change re-renders on its
    // own; the interaction router calls this for every pointer sample, and
    // clearing the rasters here re-rendered a dense page (~1 s) per sample.
    update();
}

void CanvasWidget::SetPointerCapture(bool captured) {
    if (captured == pointerCaptured_) {
        return;
    }
    pointerCaptured_ = captured;
    if (!isVisible()) {
        return;
    }
    if (captured) {
        grabMouse();
    } else {
        releaseMouse();
    }
}

void CanvasWidget::BeginStroke(std::size_t pageIndex, Vec2 pagePoint, bool dotted) {
    const CanvasPageView view = CalculatePageView(pageIndex);
    controller_.BeginStroke(pageIndex, pagePoint, view.scale);
    controller_.SetPendingStrokeDotted(dotted);
}

void CanvasWidget::SetStrokeDotted(bool dotted) {
    controller_.SetPendingStrokeDotted(dotted);
    update();
}

void CanvasWidget::AppendStrokePoint(Vec2 pagePoint, bool dotted) {
    const std::size_t pageIndex = controller_.PendingStrokePage();
    const CanvasPageView view = CalculatePageView(pageIndex);
    controller_.ExtendStroke(pagePoint, view.scale);
    controller_.SetPendingStrokeDotted(dotted);
    update();
}

void CanvasWidget::ReplaceStrokePoints(const std::vector<Vec2>& points, bool dotted) {
    const std::size_t pageIndex = controller_.PendingStrokePage();
    controller_.ReplacePendingStrokePoints(points, dotted);
    const CanvasPageView view = CalculatePageView(pageIndex);
    if (view.valid) {
        update(view.rect.toAlignedRect());
    }
}

void CanvasWidget::CommitStroke() {
    const std::size_t pageIndex = controller_.PendingStrokePage();
    const Stroke* pending = controller_.PendingStroke();
    const Document& document = controller_.DocumentRef();
    const Page* pendingPage = pageIndex < document.PageCount() ? &document.PageAt(pageIndex) : nullptr;
    const bool hasPending = pending != nullptr && pendingPage != nullptr;
    Stroke committed;
    if (hasPending) {
        committed = *pending;
    }
    controller_.EndStroke();

    const CanvasPageView view = CalculatePageView(pageIndex);
    const float devicePixelRatio = cachedDevicePixelRatio_ > 0.0f ? cachedDevicePixelRatio_ : 1.0f;
    render::PageSurfaceCache* cache = MutableSurface(pageIndex, view.scale, devicePixelRatio);
    // Same raster-resolution arithmetic as render::RenderPageSurface
    // (deviceScale = params.scale * params.devicePixelRatio, then page width *
    // deviceScale); a different float association rounds to 1124 vs 1123 on
    // a 612 pt page at zoom 2 / dpr 1.75 and dropped this fast path.
    const float deviceScale = view.scale * devicePixelRatio;

    // Composite the finished stroke into the cached page raster instead of
    // dropping it: a dense page costs about a second to re-render from scratch,
    // while the reference keeps drawing at refresh rate on the same fixture
    // (its incremental stroke commit).
    //
    // The cached raster is keyed by (logical scale, devicePixelRatio) and the
    // QImage carries that device pixel ratio, so QPainter maps the painter's
    // logical coordinates to device pixels itself.  The stroke must therefore
    // be drawn with the *logical* render params, exactly like the transient
    // preview on the widget; passing the device-space scale here applied the
    // DPR a second time and displaced/scaled the committed stroke (reported:
    // committed stroke lands away from the live preview at non-1 DPR, growing
    // with zoom).
    if (hasPending && view.valid && cache != nullptr && !cache->image.isNull()
        && cache->image.size() == QSize(
            std::max(1, static_cast<int>(std::lround(pendingPage->width * deviceScale))),
            std::max(1, static_cast<int>(std::lround(pendingPage->height * deviceScale))))) {
        render::PageRenderParams params = RenderParamsFor(view);
        QPainter painter(&cache->image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        render::PaintStrokePreview(painter, committed, params, 1.0f);
        painter.end();
        cache->signature = render::PageSurfaceSignature(*pendingPage, RenderParamsFor(view));
        update(view.rect.toAlignedRect());
        NotifyViewportChanged();
        return;
    }

    const auto page = surfaces_.find(pageIndex);
    if (page != surfaces_.end()) {
        surfaces_.erase(page);
    }
    update(view.valid ? view.rect.toAlignedRect() : rect());
}

void CanvasWidget::DiscardStroke() {
    const std::size_t pageIndex = controller_.PendingStrokePage();
    controller_.CancelStroke();
    const CanvasPageView view = CalculatePageView(pageIndex);
    update(view.valid ? view.rect.toAlignedRect() : rect());
}

void CanvasWidget::ClearSelection() {
    controller_.ClearSelection();
    update();
}

void CanvasWidget::UpdateSelectionOverlay(
    const QPointF& anchor, const QPointF& current, bool lasso, const std::vector<QPointF>& lassoPoints) {
    selectionOverlayActive_ = true;
    selectionAnchor_ = anchor;
    selectionCurrent_ = current;
    selectionOverlayLasso_ = lasso;
    lassoPoints_ = lassoPoints;
    update();
}

void CanvasWidget::CommitSelection(
    const QPointF& anchor, const QPointF& current, bool lasso, bool boxOverride, bool click) {
    selectionOverlayActive_ = false;

    const std::optional<CanvasPageView> view = HitTestPageView(anchor);
    if (!view.has_value()) {
        controller_.ClearSelection();
        update();
            return;
    }

    const Document& document = controller_.DocumentRef();
    const Page& page = document.PageAt(view->pageIndex);
    const Vec2 anchorPage = ScreenToPage(*view, anchor, false);
    const Vec2 currentPage = ScreenToPage(*view, current, false);

    PageSelection selection;
    if (click) {
        // 18 logical px click radius (Selection.cpp:1269).
        constexpr float kSelectionClickRadiusPixels = 18.0f;
        const float radiusPage = kSelectionClickRadiusPixels / std::max(0.1f, view->scale);
        const Vec2 a { anchorPage.x - radiusPage, anchorPage.y - radiusPage };
        const Vec2 b { anchorPage.x + radiusPage, anchorPage.y + radiusPage };
        for (std::size_t index = 0; index < page.strokes.size(); ++index) {
            if (StrokeIntersectsRectSelection(page.strokes[index], a, b, controller_.InputSettings().strokeSmoothing)) {
                selection.strokes.push_back(index);
            }
        }
        for (std::size_t index = 0; index < page.images.size(); ++index) {
            if (ImageIntersectsRectSelection(page.images[index], a, b)) {
                selection.images.push_back(index);
            }
        }
        for (std::size_t index = 0; index < page.texts.size(); ++index) {
            const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(page.texts[index], page.width);
            if (!(bounds.right < a.x || bounds.left > b.x || bounds.bottom < a.y || bounds.top > b.y)) {
                selection.texts.push_back(index);
            }
        }
        for (std::size_t index = 0; index < page.latexes.size(); ++index) {
            if (LatexIntersectsRectSelection(page.latexes[index], page.width, a, b)) {
                selection.latexes.push_back(index);
            }
        }
    } else if (lasso && !boxOverride) {
        std::vector<Vec2> polygon;
        polygon.reserve(lassoPoints_.size());
        for (const QPointF& point : lassoPoints_) {
            polygon.push_back(ScreenToPage(*view, point, false));
        }
        if (polygon.size() >= 3U) {
            for (std::size_t index = 0; index < page.strokes.size(); ++index) {
                if (StrokeIntersectsPolygonSelection(page.strokes[index], polygon, controller_.InputSettings().strokeSmoothing)) {
                    selection.strokes.push_back(index);
                }
            }
            for (std::size_t index = 0; index < page.images.size(); ++index) {
                if (ImageIntersectsPolygonSelection(page.images[index], polygon)) {
                    selection.images.push_back(index);
                }
            }
            for (std::size_t index = 0; index < page.texts.size(); ++index) {
                const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(page.texts[index], page.width);
                const std::array<Vec2, 4> corners {{
                    { bounds.left, bounds.top }, { bounds.right, bounds.top },
                    { bounds.right, bounds.bottom }, { bounds.left, bounds.bottom },
                }};
                bool hit = false;
                for (const Vec2& corner : corners) {
                    hit = hit || PointInPolygon(corner, polygon);
                }
                if (hit) {
                    selection.texts.push_back(index);
                }
            }
            for (std::size_t index = 0; index < page.latexes.size(); ++index) {
                if (LatexIntersectsPolygonSelection(page.latexes[index], page.width, polygon)) {
                    selection.latexes.push_back(index);
                }
            }
        }
    } else {
        const float left = std::min(anchorPage.x, currentPage.x);
        const float top = std::min(anchorPage.y, currentPage.y);
        const float right = std::max(anchorPage.x, currentPage.x);
        const float bottom = std::max(anchorPage.y, currentPage.y);
        const Vec2 a { left, top };
        const Vec2 b { right, bottom };
        for (std::size_t index = 0; index < page.strokes.size(); ++index) {
            if (StrokeIntersectsRectSelection(page.strokes[index], a, b, controller_.InputSettings().strokeSmoothing)) {
                selection.strokes.push_back(index);
            }
        }
        for (std::size_t index = 0; index < page.images.size(); ++index) {
            if (ImageIntersectsRectSelection(page.images[index], a, b)) {
                selection.images.push_back(index);
            }
        }
        for (std::size_t index = 0; index < page.texts.size(); ++index) {
            const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(page.texts[index], page.width);
            if (!(bounds.right < left || bounds.left > right || bounds.bottom < top || bounds.top > bottom)) {
                selection.texts.push_back(index);
            }
        }
        for (std::size_t index = 0; index < page.latexes.size(); ++index) {
            if (LatexIntersectsRectSelection(page.latexes[index], page.width, a, b)) {
                selection.latexes.push_back(index);
            }
        }
    }

    controller_.SelectObjects(view->pageIndex, std::move(selection));
    update();
}

void CanvasWidget::AddObjectToSelection(ObjectRef object) {
    controller_.AddObjectToSelection(object);
    update();
}

void CanvasWidget::BeginMoveSelection(std::size_t pageIndex) {
    controller_.BeginPageGesture(pageIndex);
}

void CanvasWidget::UpdateMoveSelection(const QPointF& logicalDelta) {
    const std::optional<std::size_t> pageIndex = controller_.GesturePageIndex();
    if (!pageIndex.has_value()) {
        return;
    }
    const CanvasPageView view = CalculatePageView(*pageIndex);
    if (!view.valid || view.scale <= 0.0f) {
        return;
    }
    controller_.MoveSelectionBy(
        *pageIndex,
        Vec2 { static_cast<float>(logicalDelta.x()) / view.scale, static_cast<float>(logicalDelta.y()) / view.scale });
    if (const auto page = surfaces_.find(*pageIndex); page != surfaces_.end()) {
        surfaces_.erase(page);
    }
    update();
}

void CanvasWidget::CommitMoveSelection() {
    controller_.CommitPageGesture();
    update();
}

void CanvasWidget::BeginTransform(const TransformRequest& request) {
    const std::optional<SelectionBounds> from = SelectionBoundsForPage(request.pageIndex);
    if (!from.has_value()) {
        return;
    }
    transformFrom_ = *from;
    transformHandle_ = request.handle;
    transformPressLogical_ = lastHandleProbeLogical_;
    // Same one-transaction-per-gesture machinery as the selection move: the
    // controller snapshots the page and CommitTransform records one
    // ReplacePages action.
    controller_.BeginPageGesture(request.pageIndex);
}

void CanvasWidget::UpdateTransform(const QPointF& logical, Qt::KeyboardModifiers modifiers) {
    const std::optional<std::size_t> pageIndex = controller_.GesturePageIndex();
    if (!pageIndex.has_value() || !transformHandle_.has_value()) {
        return;
    }
    const CanvasPageView view = CalculatePageView(*pageIndex);
    if (!view.valid || view.scale <= 0.0f) {
        return;
    }
    const TransformHandle handle = *transformHandle_;
    if (handle == TransformHandle::Move) {
        // Moving is its own interaction (BeginMoveSelection).
        return;
    }

    const Vec2 pagePoint = ScreenToPage(view, logical, false);
    SelectionTransform transform;
    transform.from = transformFrom_;
    transform.to = transformFrom_;

    const bool shiftPressed = modifiers.testFlag(Qt::ShiftModifier);
    transform.scaleTextFont = shiftPressed;
    if (handle == TransformHandle::Rotate) {
        const Vec2 center {
            (transformFrom_.left + transformFrom_.right) * 0.5f,
            (transformFrom_.top + transformFrom_.bottom) * 0.5f,
        };
        const Vec2 press = ScreenToPage(view, transformPressLogical_, false);
        const float pointerAngle = std::atan2(pagePoint.y - center.y, pagePoint.x - center.x);
        const float pressAngle = std::atan2(press.y - center.y, press.x - center.x);
        transform.rotateDegrees = (pointerAngle - pressAngle) * (180.0f / kPi);
        if (shiftPressed) {
            // Reference App.cpp:8997-8999: Shift snaps rotation to 15 degrees.
            transform.rotateDegrees = std::round(transform.rotateDegrees / 15.0f) * 15.0f;
        }
    } else {
        // Resize: each dragged side follows the pointer while the opposite side
        // stays fixed, so the selection scales about the opposite edge/corner.
        const bool movesLeft = handle == TransformHandle::Left
            || handle == TransformHandle::TopLeft || handle == TransformHandle::BottomLeft;
        const bool movesRight = handle == TransformHandle::Right
            || handle == TransformHandle::TopRight || handle == TransformHandle::BottomRight;
        const bool movesTop = handle == TransformHandle::Top
            || handle == TransformHandle::TopLeft || handle == TransformHandle::TopRight;
        const bool movesBottom = handle == TransformHandle::Bottom
            || handle == TransformHandle::BottomLeft || handle == TransformHandle::BottomRight;
        if (movesLeft) {
            transform.to.left = pagePoint.x;
        }
        if (movesRight) {
            transform.to.right = pagePoint.x;
        }
        if (movesTop) {
            transform.to.top = pagePoint.y;
        }
        if (movesBottom) {
            transform.to.bottom = pagePoint.y;
        }

        // Keep both extents positive (never mirrored), clamped to the fixed
        // opposite side.
        constexpr float kMinSelectionExtent = 1.0f;
        if (movesLeft && (transform.to.right - transform.to.left) < kMinSelectionExtent) {
            transform.to.left = transform.to.right - kMinSelectionExtent;
        }
        if (movesRight && (transform.to.right - transform.to.left) < kMinSelectionExtent) {
            transform.to.right = transform.to.left + kMinSelectionExtent;
        }
        if (movesTop && (transform.to.bottom - transform.to.top) < kMinSelectionExtent) {
            transform.to.top = transform.to.bottom - kMinSelectionExtent;
        }
        if (movesBottom && (transform.to.bottom - transform.to.top) < kMinSelectionExtent) {
            transform.to.bottom = transform.to.top + kMinSelectionExtent;
        }

        if (shiftPressed) {
            // Reference App.cpp:9044-9095: Shift locks the aspect ratio.
            // Corner handles scale uniformly about the opposite corner; edge
            // handles derive the perpendicular extent from the dragged one,
            // centered on the original frame.
            const float initialWidth = std::max(kMinSelectionExtent,
                transformFrom_.right - transformFrom_.left);
            const float initialHeight = std::max(kMinSelectionExtent,
                transformFrom_.bottom - transformFrom_.top);
            const float aspect = initialWidth / initialHeight;
            const bool corner = (movesLeft || movesRight) && (movesTop || movesBottom);
            if (corner) {
                const float widthScale = (transform.to.right - transform.to.left) / initialWidth;
                const float heightScale = (transform.to.bottom - transform.to.top) / initialHeight;
                const float scale = std::max(widthScale, heightScale);
                const float newWidth = std::max(kMinSelectionExtent, initialWidth * scale);
                const float newHeight = std::max(kMinSelectionExtent, initialHeight * scale);
                if (movesLeft) {
                    transform.to.left = transform.to.right - newWidth;
                } else {
                    transform.to.right = transform.to.left + newWidth;
                }
                if (movesTop) {
                    transform.to.top = transform.to.bottom - newHeight;
                } else {
                    transform.to.bottom = transform.to.top + newHeight;
                }
            } else if (movesLeft || movesRight) {
                const float newWidth = std::max(kMinSelectionExtent,
                    transform.to.right - transform.to.left);
                const float newHeight = std::max(kMinSelectionExtent, newWidth / aspect);
                if (movesLeft) {
                    transform.to.left = transform.to.right - newWidth;
                } else {
                    transform.to.right = transform.to.left + newWidth;
                }
                const float centerY = (transformFrom_.top + transformFrom_.bottom) * 0.5f;
                transform.to.top = centerY - (newHeight * 0.5f);
                transform.to.bottom = centerY + (newHeight * 0.5f);
            } else {
                const float newHeight = std::max(kMinSelectionExtent,
                    transform.to.bottom - transform.to.top);
                const float newWidth = std::max(kMinSelectionExtent, newHeight * aspect);
                if (movesTop) {
                    transform.to.top = transform.to.bottom - newHeight;
                } else {
                    transform.to.bottom = transform.to.top + newHeight;
                }
                const float centerX = (transformFrom_.left + transformFrom_.right) * 0.5f;
                transform.to.left = centerX - (newWidth * 0.5f);
                transform.to.right = centerX + (newWidth * 0.5f);
            }
        }
    }

    controller_.TransformSelection(*pageIndex, transform);
    if (const auto page = surfaces_.find(*pageIndex); page != surfaces_.end()) {
        surfaces_.erase(page);
    }
    update();
}

void CanvasWidget::CommitTransform() {
    controller_.CommitPageGesture();
    transformHandle_.reset();
    transformFrom_ = SelectionBounds {};
    transformPressLogical_ = QPointF();
    update();
}

void CanvasWidget::UpdatePendingErase(const std::vector<ObjectRef>& pending) {
    pendingErase_ = pending;
    update();
}

void CanvasWidget::CommitErase(const std::vector<ObjectRef>& pending) {
    std::map<std::size_t, std::vector<ObjectRef>> perPage;
    for (const ObjectRef& object : pending) {
        perPage[object.pageIndex].push_back(object);
    }
    for (const auto& entry : perPage) {
        controller_.CommitErase(entry.first, entry.second);
        if (const auto page = surfaces_.find(entry.first); page != surfaces_.end()) {
            surfaces_.erase(page);
        }
    }
    pendingErase_.clear();
    update();
}

void CanvasWidget::BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) {
    if (controller_.BeginTextEdit(pageIndex, textIndex)) {
        SyncEditOverlay();
    }
}

void CanvasWidget::CreateTextAt(std::size_t pageIndex, Vec2 pagePoint) {
    controller_.AddTextObject(pageIndex, pagePoint, 24.0f);
    SyncEditOverlay();
}

void CanvasWidget::BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex) {
    if (controller_.BeginLatexEdit(pageIndex, latexIndex)) {
        SyncEditOverlay();
    }
}

void CanvasWidget::CreateLatexAt(std::size_t pageIndex, Vec2 pagePoint) {
    controller_.AddLatexObject(pageIndex, pagePoint, 28.0f);
    SyncEditOverlay();
}

// --- ui::PrecisionInputSink -------------------------------------------------

PrecisionInputRoute CanvasWidget::RoutePrecisionInput(const QPoint& globalPosition) const {
    const QPointF logical = mapFromGlobal(globalPosition);
    return VisibleCanvasRect().contains(logical) ? PrecisionInputRoute::Canvas : PrecisionInputRoute::PassThrough;
}

void CanvasWidget::BeginPrecisionGesture() {
    precisionStartZoom_ = zoom_;
}


void CanvasWidget::ZoomAtAnchor(qreal cumulativeScale, const QPoint& globalAnchor) {
    const QPointF logical = mapFromGlobal(globalAnchor);
    SetZoom(static_cast<float>(precisionStartZoom_ * cumulativeScale), logical);
}

void CanvasWidget::PanBy(qreal deltaX, qreal deltaY) {
    PanBy(QPointF(deltaX, deltaY));
}

void CanvasWidget::EndPrecisionGesture() {
}

// --- smooth scroll, selection helpers and transient painting ----------------

void CanvasWidget::StartSmoothScroll(const QPointF& delta) {
    scrollStart_ = QPointF(panX_, panY_);
    scrollTarget_ = scrollStart_ + delta;
    scrollClock_.start();
    scrolling_ = true;
    if (scrollTimer_ == nullptr) {
        scrollTimer_ = new QTimer(this);
        scrollTimer_->setTimerType(Qt::PreciseTimer);
        scrollTimer_->setInterval(8);
        connect(scrollTimer_, &QTimer::timeout, this, [this]() { UpdateSmoothScroll(); });
    }
    scrollTimer_->start();
}

void CanvasWidget::StopSmoothScroll() {
    scrolling_ = false;
    if (scrollTimer_ != nullptr) {
        scrollTimer_->stop();
    }
}

void CanvasWidget::UpdateSmoothScroll() {
    if (!scrolling_) {
        return;
    }
    constexpr double kSmoothScrollDurationMs = 90.0;
    const double progress = std::min(1.0, static_cast<double>(scrollClock_.elapsed()) / kSmoothScrollDurationMs);
    // Reference Viewport.cpp:53 interpolation: start + (target - start) * (1 - (1-t)^3).
    const double remaining = 1.0 - progress;
    const double eased = 1.0 - (remaining * remaining * remaining);
    panX_ = static_cast<float>(scrollStart_.x() + ((scrollTarget_.x() - scrollStart_.x()) * eased));
    panY_ = static_cast<float>(scrollStart_.y() + ((scrollTarget_.y() - scrollStart_.y()) * eased));
    ClampViewportPan();
    NotifyViewportChanged();
    if (progress >= 1.0) {
        StopSmoothScroll();
    }
}

std::optional<SelectionBounds> CanvasWidget::SelectionBoundsForPage(std::size_t pageIndex) const {
    const Document& document = controller_.DocumentRef();
    if (pageIndex >= document.PageCount()) {
        return std::nullopt;
    }
    const Page& page = document.PageAt(pageIndex);
    const PageSelection& selection = controller_.SelectionFor(pageIndex);
    if (selection.Empty()) {
        return std::nullopt;
    }

    SelectionBounds bounds;
    const auto include = [&bounds](float left, float top, float right, float bottom) {
        if (!bounds.valid) {
            bounds = SelectionBounds { left, top, right, bottom, true };
            return;
        }
        bounds.left = std::min(bounds.left, left);
        bounds.top = std::min(bounds.top, top);
        bounds.right = std::max(bounds.right, right);
        bounds.bottom = std::max(bounds.bottom, bottom);
    };

    for (const std::size_t index : selection.strokes) {
        if (index >= page.strokes.size()) {
            continue;
        }
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        if (StrokeBounds(page.strokes[index], &minX, &minY, &maxX, &maxY)) {
            include(minX, minY, maxX, maxY);
        }
    }
    for (const std::size_t index : selection.images) {
        if (index >= page.images.size()) {
            continue;
        }
        for (const Vec2& corner : ImageCorners(page.images[index])) {
            include(corner.x, corner.y, corner.x, corner.y);
        }
    }
    for (const std::size_t index : selection.texts) {
        if (index >= page.texts.size()) {
            continue;
        }
        const text::QtTextBounds textBounds = text::MeasurePageTextBoundsQt(page.texts[index], page.width);
        include(textBounds.left, textBounds.top, textBounds.right, textBounds.bottom);
    }
    for (const std::size_t index : selection.latexes) {
        if (index >= page.latexes.size()) {
            continue;
        }
        const LatexBounds latexBounds = MeasurePageLatexBounds(page.latexes[index], page.width);
        include(latexBounds.left, latexBounds.top, latexBounds.right, latexBounds.bottom);
    }
    return bounds.valid ? std::optional<SelectionBounds>(bounds) : std::nullopt;
}

std::array<QPointF, 8> CanvasWidget::SelectionHandlePositions(const QRectF& frame) {
    return {{
        frame.topLeft(),
        frame.topRight(),
        frame.bottomRight(),
        frame.bottomLeft(),
        QPointF(frame.center().x(), frame.top()),
        QPointF(frame.center().x(), frame.bottom()),
        QPointF(frame.left(), frame.center().y()),
        QPointF(frame.right(), frame.center().y()),
    }};
}

QPointF CanvasWidget::SelectionRotateHandlePosition(const QRectF& frame) {
    return QPointF(frame.center().x(), frame.top() - static_cast<qreal>(kSelectionRotateOffsetDip));
}

void CanvasWidget::PaintSelectionOverlay(QPainter& painter) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Committed selection: dashed frame plus handles (transient, never cached).
    for (const CanvasPageView& view : CalculatePageViews()) {
        if (!view.valid) {
            continue;
        }
        const std::optional<SelectionBounds> bounds = SelectionBoundsForPage(view.pageIndex);
        if (!bounds.has_value()) {
            continue;
        }
        const QRectF frame(
            view.rect.left() + (static_cast<qreal>(bounds->left) * view.scale),
            view.rect.top() + (static_cast<qreal>(bounds->top) * view.scale),
            static_cast<qreal>(bounds->right - bounds->left) * view.scale,
            static_cast<qreal>(bounds->bottom - bounds->top) * view.scale);

        QPen pen(QColor(59, 130, 246), 1.0, Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(frame);

        const qreal handleHalf = 4.0;
        painter.setPen(QPen(QColor(59, 130, 246), 1.0));
        painter.setBrush(QColor(255, 255, 255));
        for (const QPointF& handle : SelectionHandlePositions(frame)) {
            painter.drawRect(QRectF(handle.x() - handleHalf, handle.y() - handleHalf, handleHalf * 2.0, handleHalf * 2.0));
        }

        // Rotate handle: a stem from the top edge midpoint to a circular knob
        // (the same colours as the square handles).
        const QPointF rotate = SelectionRotateHandlePosition(frame);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(frame.center().x(), frame.top()), rotate);
        painter.setBrush(QColor(255, 255, 255));
        painter.drawEllipse(rotate, 4.0, 4.0);

        // Move grip at the frame centre (reference App::CursorHitsSelectionMoveGrip).
        painter.setBrush(QColor(59, 130, 246));
        painter.drawEllipse(frame.center(), 3.0, 3.0);
    }

    // Live marquee / lasso.
    if (selectionOverlayActive_) {
        painter.setPen(QPen(QColor(59, 130, 246), 1.0, selectionOverlayLasso_ ? Qt::SolidLine : Qt::DashLine));
        painter.setBrush(QColor(59, 130, 246, 40));
        if (selectionOverlayLasso_ && lassoPoints_.size() >= 2U) {
            QPainterPath path;
            path.moveTo(lassoPoints_.front());
            for (std::size_t index = 1; index < lassoPoints_.size(); ++index) {
                path.lineTo(lassoPoints_[index]);
            }
            path.closeSubpath();
            painter.drawPath(path);
        } else if (!selectionOverlayLasso_) {
            painter.drawRect(QRectF(selectionAnchor_, selectionCurrent_).normalized());
        }
    }
    painter.restore();
}

render::PendingEraseIndices CanvasWidget::PendingEraseFor(std::size_t pageIndex) const {
    // The router's ObjectRef list becomes the per-kind index sets the renderer
    // fades (App.cpp:5572-5628 pendingErase*Indices_).
    render::PendingEraseIndices indices;
    for (const ObjectRef& object : pendingErase_) {
        if (object.pageIndex != pageIndex) {
            continue;
        }
        switch (object.kind) {
        case ObjectKind::Stroke:
            indices.strokes.push_back(object.objectIndex);
            break;
        case ObjectKind::Image:
            indices.images.push_back(object.objectIndex);
            break;
        case ObjectKind::Text:
            indices.texts.push_back(object.objectIndex);
            break;
        case ObjectKind::Latex:
            indices.latexes.push_back(object.objectIndex);
            break;
        }
    }
    return indices;
}

}  // namespace cnote::ui
