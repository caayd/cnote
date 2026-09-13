#include "ui/CanvasWidget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <QColor>
#include <QImage>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
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

        visiblePages.push_back(view.pageIndex);
        ++paintedPages;
    }

    lastPaintPageCount_ = paintedPages;
    PruneSurfaces(visiblePages);
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

// --- editing overlay -------------------------------------------------------

bool CanvasWidget::EditOverlayActive() const {
    return overlayTextIndex_.has_value() || overlayLatexIndex_.has_value();
}

bool CanvasWidget::BeginEditAt(const QPointF& logical) {
    const std::optional<CanvasPageView> hit = HitTestPage(logical);
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
    if (textEditor_ != nullptr && controller_.EditingTextIndex().has_value()) {
        PageText edited = controller_.EditingTextObject() != nullptr ? *controller_.EditingTextObject() : PageText {};
        edited.content = ToUtf16(textEditor_->toPlainText());
        edited.styleSpans = text::CapturePageTextFormatting(*textEditor_->document(), edited);
        controller_.CommitTextEdit(std::move(edited));
    } else if (latexEditor_ != nullptr && controller_.EditingLatexIndex().has_value()) {
        controller_.CommitLatexEdit(latexEditor_->toPlainText());
    }
    HideOverlays();
    update();
}

void CanvasWidget::CancelActiveEdit() {
    if (controller_.EditingTextIndex().has_value()) {
        controller_.CancelTextEdit();
    }
    if (controller_.EditingLatexIndex().has_value()) {
        controller_.CancelLatexEdit();
    }
    HideOverlays();
    update();
}

void CanvasWidget::EnsureTextEditor() {
    if (textEditor_ != nullptr) {
        return;
    }
    textEditor_ = new QTextEdit(this);
    textEditor_->setAcceptRichText(false);
    textEditor_->setFrameShape(QFrame::Box);
    textEditor_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    textEditor_->installEventFilter(this);
    textEditor_->hide();
    connect(textEditor_, &QTextEdit::textChanged, this, [this]() { PositionOverlays(); });
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
    const text::TextOffsetMap map = text::BuildTextOffsetMap(text.content);
    textEditor_->document()->setDocumentMargin(0.0);
    {
        QSignalBlocker blocker(textEditor_);
        textEditor_->setPlainText(FromUtf16(map.normalized));
        text::ApplyPageTextFormatting(*textEditor_->document(), text);
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
    const Page& page = document.PageAt(*overlayPageIndex_);

    if (textEditor_ != nullptr && textEditor_->isVisible() && controller_.EditingTextObject() != nullptr) {
        const PageText& object = *controller_.EditingTextObject();
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(object, page.width);
        const qreal left = view.rect.left() + (static_cast<qreal>(std::max(bounds.left, object.x)) * view.scale);
        const qreal top = view.rect.top() + (static_cast<qreal>(object.y) * view.scale);
        const qreal width = std::max<qreal>(kTextEditorMinWidth, static_cast<qreal>(std::max(bounds.right - bounds.left, object.width)) * view.scale);
        const qreal height = std::max<qreal>(
            kTextEditorMinHeight,
            textEditor_->document()->size().height() + 8.0);
        const QRect geometry(QPoint(static_cast<int>(std::lround(left)), static_cast<int>(std::lround(top))),
                             QSize(static_cast<int>(std::lround(width)), static_cast<int>(std::lround(height))));
        if (textEditor_->geometry() != geometry) {
            textEditor_->setGeometry(geometry);
        }
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
    const QTextCharFormat format = text::CharFormatForStyle(style);
    cursor.mergeCharFormat(format);
    textEditor_->mergeCurrentCharFormat(format);
    PositionOverlays();
}

PageText CanvasWidget::CaptureEditedText() const {
    PageText edited = controller_.EditingTextObject() != nullptr ? *controller_.EditingTextObject() : PageText {};
    if (textEditor_ != nullptr) {
        edited.content = ToUtf16(textEditor_->toPlainText());
        edited.styleSpans = text::CapturePageTextFormatting(*textEditor_->document(), edited);
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
        // Clicking another object commits the active edit first; clicking the
        // edited object keeps it open.
        if (EditOverlayActive()) {
            const std::optional<CanvasPageView> hit = HitTestPage(event->position());
            if (!hit.has_value()) {
                CommitActiveEdit();
                event->accept();
                return;
            }
        }
        if (BeginEditAt(event->position())) {
            event->accept();
            return;
        }
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
