#include "ui/CanvasWidget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <QColor>
#include <QImage>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QTimer>
#include <QWheelEvent>

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

qreal RoundToPixels(float value) {
    return static_cast<qreal>(std::lround(value));
}

}  // namespace

CanvasWidget::CanvasWidget(CanvasController& controller, QWidget* parent)
    : QWidget(parent),
      controller_(controller) {
    // The whole widget is repainted by us; Qt need not clear it first.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::StrongFocus);

    zoomSettleTimer_ = new QTimer(this);
    zoomSettleTimer_->setSingleShot(true);
    zoomSettleTimer_->setInterval(kZoomSessionDelayMs);
    connect(zoomSettleTimer_, &QTimer::timeout, this, [this]() { EndZoomSession(); });
}

CanvasWidget::~CanvasWidget() = default;

// --- view state ------------------------------------------------------------

void CanvasWidget::SetZoom(float zoom, std::optional<QPointF> anchor) {
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

void CanvasWidget::SetZoomAt(const QPointF& anchor, float zoom) {
    SetZoom(zoom, anchor);
}

void CanvasWidget::FitToWindow() {
    SetZoom(1.0f);
}

void CanvasWidget::ResetView() {
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
    if (const std::optional<CanvasPageView> hit = HitTestPage(anchor); hit.has_value()) {
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

    const QRectF canvasRect = VisibleCanvasRect();
    const qreal pageCenterY = view.rect.top() + (view.rect.height() / 2.0);
    const qreal desiredCenterY = canvasRect.top() + (canvasRect.height() / 2.0);
    panY_ += static_cast<float>(desiredCenterY - pageCenterY);
    ClampViewportPan();
    NotifyViewportChanged();
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

    const float fitScale = (canvasWidth - static_cast<float>(kZoomReferenceInset)) / widestPage;
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

    const float fitScale = (canvasWidth - static_cast<float>(kZoomReferenceInset)) / widestPage;
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

std::optional<CanvasPageView> CanvasWidget::HitTestPage(const QPointF& logical) const {
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
            render::EnsurePageSurface(entry.cache, page, RenderParamsFor(view));
            return entry.cache;
        }
    }

    perPage.entries.insert(perPage.entries.begin(), SurfaceEntry { view.scale, devicePixelRatio, {} });
    if (perPage.entries.size() > kScalesPerPage) {
        perPage.entries.resize(kScalesPerPage);
    }
    render::EnsurePageSurface(perPage.entries.front().cache, page, RenderParamsFor(view));
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
    std::vector<std::size_t> visiblePages;
    std::size_t paintedPages = 0;

    for (const CanvasPageView& view : CalculatePageViews()) {
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
        const bool liveStrokeOnPage = drawing_ && strokePageIndex_.has_value()
            && *strokePageIndex_ == view.pageIndex;
        const render::PageSurfaceCache* surface = nullptr;

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

        painter.save();
        painter.setClipRect(view.rect);
        painter.drawImage(view.rect, surface->image);

        if (liveStrokeOnPage) {
            if (const Stroke* pending = controller_.PendingStroke(); pending != nullptr) {
                painter.save();
                painter.translate(view.rect.topLeft());
                render::PaintStrokePreview(painter, *pending, RenderParamsFor(view), 1.0f);
                painter.restore();
            }
        }
        painter.restore();

        // Page border, drawn on top of the page edge like the reference.
        painter.save();
        painter.setPen(QPen(QColor::fromRgba(kPageBorderColor), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(view.rect.adjusted(0.5, 0.5, -0.5, -0.5));
        painter.restore();

        visiblePages.push_back(view.pageIndex);
        ++paintedPages;
    }

    lastPaintPageCount_ = paintedPages;
    PruneSurfaces(visiblePages);
}

void CanvasWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // The fit scale depends on the canvas width, so the page layout changes with
    // the widget; the reference frontend re-derives it on every paint as well.
    update();
}

// --- interaction -----------------------------------------------------------

void CanvasWidget::BeginStrokeAt(const QPointF& logical) {
    const std::optional<CanvasPageView> hit = HitTestPage(logical);
    if (!hit.has_value() || hit->scale <= 0.0f) {
        return;
    }

    strokePageIndex_ = hit->pageIndex;
    drawing_ = true;
    strokeLastPosition_ = logical;
    controller_.BeginStroke(hit->pageIndex, ScreenToPage(*hit, logical, true), hit->scale);

    const qreal margin = kStrokeDirtyMarginPixels;
    update(QRectF(logical.x() - margin, logical.y() - margin, margin * 2.0, margin * 2.0).toAlignedRect());
}

bool CanvasWidget::ExtendStrokeTo(const QPointF& logical) {
    if (!drawing_ || !strokePageIndex_.has_value()) {
        return false;
    }

    const Document& document = controller_.DocumentRef();
    if (*strokePageIndex_ >= document.PageCount()) {
        CancelStroke();
        return false;
    }

    const CanvasPageView view = CalculatePageView(*strokePageIndex_);
    if (!view.valid || view.scale <= 0.0f) {
        return false;
    }

    // Reference App::ApplyStrokePoint: crossing into another page ends the
    // current stroke and starts a new segment on the page under the pointer.
    if (const std::optional<CanvasPageView> hit = HitTestPage(logical);
        hit.has_value() && hit->pageIndex != *strokePageIndex_) {
        const QRectF previousRect = view.rect;
        EndStroke();
        controller_.BeginStroke(hit->pageIndex, ScreenToPage(*hit, logical, true), hit->scale);
        strokePageIndex_ = hit->pageIndex;
        drawing_ = true;
        strokeLastPosition_ = logical;
        update(previousRect.toAlignedRect().united(hit->rect.toAlignedRect()));
        return true;
    }

    controller_.ExtendStroke(ScreenToPage(view, logical, true), view.scale);

    // Dirty-region repaint for the new segment only; the page raster cache is
    // untouched, so the rest of the canvas is not redrawn from the document.
    const qreal margin = kStrokeDirtyMarginPixels;
    const QRectF segment = QRectF(strokeLastPosition_, logical).normalized()
        .adjusted(-margin, -margin, margin, margin);
    strokeLastPosition_ = logical;
    update(segment.toAlignedRect());
    return true;
}

void CanvasWidget::EndStroke() {
    if (!drawing_) {
        return;
    }

    drawing_ = false;
    const std::optional<std::size_t> pageIndex = strokePageIndex_;
    strokePageIndex_.reset();
    controller_.EndStroke();

    if (pageIndex.has_value()) {
        // The committed stroke lives in the page raster cache now; drop that
        // page's surfaces so the next paint re-renders it exactly once.
        const auto page = surfaces_.find(*pageIndex);
        if (page != surfaces_.end()) {
            surfaces_.erase(page);
        }
        const CanvasPageView view = CalculatePageView(*pageIndex);
        update(view.valid ? view.rect.toAlignedRect() : rect());
    } else {
        update();
    }
}

void CanvasWidget::CancelStroke() {
    if (!drawing_) {
        return;
    }

    drawing_ = false;
    const std::optional<std::size_t> pageIndex = strokePageIndex_;
    strokePageIndex_.reset();
    controller_.CancelStroke();

    if (pageIndex.has_value()) {
        if (const auto page = surfaces_.find(*pageIndex); page != surfaces_.end()) {
            surfaces_.erase(page);
        }
    }
    update();
}

bool CanvasWidget::HasActiveStroke() const {
    return drawing_;
}

void CanvasWidget::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);

    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        panLastPosition_ = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        BeginStrokeAt(event->position());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        const QPointF delta = event->position() - panLastPosition_;
        panLastPosition_ = event->position();
        PanBy(delta);
        event->accept();
        return;
    }
    if (drawing_ && (event->buttons() & Qt::LeftButton) != 0) {
        ExtendStrokeTo(event->position());
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton && panning_) {
        panning_ = false;
        unsetCursor();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && drawing_) {
        EndStroke();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void CanvasWidget::wheelEvent(QWheelEvent* event) {
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        // Cursor-anchored zoom: one 1.1/0.9 step per wheel notch (reference
        // App::ZoomAt), anchored at the pointer.
        const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
        if (delta != 0) {
            ZoomBySteps(delta > 0 ? 1 : -1, event->position());
        }
        event->accept();
        return;
    }

    // Ordinary wheels use angleDelta (3 lines * 18 logical px per notch, the
    // reference WheelScrollPixels); precision devices deliver pixelDelta, which
    // is already in pixels and is applied without additional smoothing.
    QPointF pixels;
    if (!event->pixelDelta().isNull()) {
        pixels = QPointF(event->pixelDelta());
    } else {
        constexpr qreal kPixelsPerNotch = static_cast<qreal>(kWheelScrollLines * kWheelScrollUnitPixels);
        const QPoint angle = event->angleDelta();
        pixels = QPointF(
            (static_cast<qreal>(angle.x()) / 120.0) * kPixelsPerNotch,
            (static_cast<qreal>(angle.y()) / 120.0) * kPixelsPerNotch);
    }
    if (!pixels.isNull()) {
        // Reference: vertical wheel pans along +y, horizontal wheel pans -x.
        PanBy(QPointF(-pixels.x(), pixels.y()));
    }
    event->accept();
}

}  // namespace cnote::ui
