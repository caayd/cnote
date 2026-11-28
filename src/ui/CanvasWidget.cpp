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
#include <QModelIndex>
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
#include <QTabletEvent>
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

// Eraser trail look (Excalidraw's EraserTrail): 20% black, repainted every
// frame (~60 Hz) while it decays; arcs flattened to a quarter logical pixel.
constexpr int kEraserTrailAlpha = 51;
constexpr int kEraserTrailFrameMs = 16;
constexpr float kEraserTrailTolerance = 0.25f;

// Hold-to-snap: how long the pointer must rest mid-stroke, and how far (in
// logical pixels) it may drift while resting, to snap the stroke to a shape.
constexpr int kShapeHoldMs = 500;
constexpr qreal kShapeHoldTolerancePixels = 3.0;

// The snapped stroke's freehand original: its opacity under the shape while
// the pointer is down, and how long it takes to fade out after the release.
constexpr float kSnapGhostOpacity = 0.3f;
constexpr int kSnapGhostFadeMs = 250;
constexpr int kSnapGhostFrameMs = 16;

// LaTeX source box and completion popup metrics (logical pixels).
constexpr int kLatexSourceBoxHeight = 120;
constexpr int kLatexSourceBoxGap = 6;
constexpr int kLatexPopupWidth = 260;
constexpr int kLatexPopupHeight = 140;
constexpr int kLatexPopupPageStep = 8;

// Selection chrome after Excalidraw's interactive scene (renderSelectionBorder,
// renderTransformHandles, getTransformHandlesFromCoords), in logical pixels:
// a 1 px frame 4 px outside the selection, 8 px round-cornered corner handles
// 2 px past the frame corners, and a round rotate handle 18 px above the frame.
// Sides have no visible handle; a band along each frame edge resizes instead.
constexpr QRgb kSelectionColor = 0xFF6965DBU;
constexpr qreal kSelectionFramePadding = 4.0;
constexpr qreal kSelectionHandleSize = 8.0;
constexpr qreal kSelectionHandleCornerRadius = 2.0;
constexpr qreal kSelectionHandleOutset = 2.0;
constexpr qreal kSelectionRotateGap = 18.0;
constexpr qreal kSelectionEdgeGrip = 5.0;

// Logical-pixel outline of one selected object, padded outward along its own
// (possibly rotated) axes like Excalidraw's per-element selection border.
QPolygonF PaddedObjectOutline(const CanvasPageView& view, const std::array<Vec2, 4>& corners, qreal padding) {
    std::array<QPointF, 4> points;
    for (std::size_t index = 0; index < corners.size(); ++index) {
        points[index] = QPointF(
            view.rect.left() + (static_cast<qreal>(corners[index].x) * view.scale),
            view.rect.top() + (static_cast<qreal>(corners[index].y) * view.scale));
    }
    // A straight stroke has a zero-length side; pad it along the other side's
    // normal so the outline still has an area.
    QPointF across = points[1] - points[0];
    const qreal acrossLength = std::hypot(across.x(), across.y());
    across = acrossLength > 1e-3 ? across / acrossLength : QPointF(1.0, 0.0);
    QPointF down = points[3] - points[0];
    const qreal downLength = std::hypot(down.x(), down.y());
    down = downLength > 1e-3 ? down / downLength : QPointF(-across.y(), across.x());
    across *= padding;
    down *= padding;
    return QPolygonF({
        points[0] - across - down,
        points[1] + across - down,
        points[2] + across + down,
        points[3] - across + down,
    });
}

// Axis-aligned page-space union of object outlines; nullopt when empty.
std::optional<SelectionBounds> OutlinesBounds(const std::vector<std::array<Vec2, 4>>& outlines) {
    if (outlines.empty()) {
        return std::nullopt;
    }
    const Vec2 first = outlines.front().front();
    SelectionBounds bounds { first.x, first.y, first.x, first.y, true };
    for (const std::array<Vec2, 4>& outline : outlines) {
        for (const Vec2& corner : outline) {
            bounds.left = std::min(bounds.left, corner.x);
            bounds.top = std::min(bounds.top, corner.y);
            bounds.right = std::max(bounds.right, corner.x);
            bounds.bottom = std::max(bounds.bottom, corner.y);
        }
    }
    return bounds;
}

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

    eraserTrailClock_.start();
    eraserTrailTimer_ = new QTimer(this);
    eraserTrailTimer_->setInterval(kEraserTrailFrameMs);
    connect(eraserTrailTimer_, &QTimer::timeout, this, [this]() { RefreshEraserTrail(); });

    shapeHoldTimer_ = new QTimer(this);
    shapeHoldTimer_->setSingleShot(true);
    shapeHoldTimer_->setInterval(kShapeHoldMs);
    connect(shapeHoldTimer_, &QTimer::timeout, this, [this]() { SnapHeldStroke(); });

    snapGhostClock_.start();
    snapGhostTimer_ = new QTimer(this);
    snapGhostTimer_->setInterval(kSnapGhostFrameMs);
    connect(snapGhostTimer_, &QTimer::timeout, this, [this]() { RefreshSnapGhost(); });
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

    if (zoomSession_.has_value() && zoomSession_->anchorValid) {
        const QPointF anchored = PageToScreen(zoomSession_->anchorPageIndex, zoomSession_->anchorPage);
        panX_ += static_cast<float>(zoomSession_->anchorCursor.x() - anchored.x());
        panY_ += static_cast<float>(zoomSession_->anchorCursor.y() - anchored.y());
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
    zoomSession_.reset();
    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
    NotifyViewportChanged();
}

void CanvasWidget::BeginZoomSession(const QPointF& anchor) {
    zoomSession_ = ZoomSession {};
    // Reference App::ResolveZoomAnchorPage (Viewport.cpp:437-458): the anchor
    // page is the *nearest* page, not only a direct hit, so a cursor in the
    // margin or an open wheel's centre still anchors the zoom.
    std::optional<CanvasPageView> best;
    float bestDistance = 0.0f;
    for (const CanvasPageView& view : CalculatePageViews()) {
        if (!view.valid) {
            continue;
        }
        const qreal dx = std::max({ view.rect.left() - anchor.x(), 0.0, anchor.x() - view.rect.right() });
        const qreal dy = std::max({ view.rect.top() - anchor.y(), 0.0, anchor.y() - view.rect.bottom() });
        const float distance = static_cast<float>((dx * dx) + (dy * dy));
        if (!best.has_value() || distance < bestDistance) {
            best = view;
            bestDistance = distance;
            if (distance <= 0.0f) {
                break;
            }
        }
    }
    if (best.has_value()) {
        zoomSession_->anchorValid = true;
        zoomSession_->anchorPageIndex = best->pageIndex;
        zoomSession_->anchorPage = ScreenToPage(*best, anchor, false);
        zoomSession_->anchorCursor = anchor;
    }
    zoomSettleTimer_->start();
}

void CanvasWidget::EndZoomSession() {
    if (!zoomSession_.has_value()) {
        return;
    }
    zoomSession_.reset();
    update();
}

// --- panning ---------------------------------------------------------------

void CanvasWidget::PanBy(const QPointF& delta) {
    // Reference App::PanBy (Viewport.cpp:116-119) cancels an in-flight smooth
    // scroll before applying the delta: a precision pan (native touchpad frame
    // or a pixelDelta event) must land 1:1 and must not fight the notch chase
    // of an earlier wheel notch for the same viewport.
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
    SyncFocusedPage();
    PositionOverlays();
    update();
    emit ViewportChanged();
}

void CanvasWidget::SyncFocusedPage() {
    const QRectF canvasRect = VisibleCanvasRect();
    const qreal canvasCenterY = canvasRect.center().y();
    std::optional<std::size_t> best;
    qreal bestArea = 0.0;
    qreal bestDistance = 0.0;
    for (const CanvasPageView& view : CalculatePageViews()) {
        if (view.rect.top() >= canvasRect.bottom()) {
            break;  // pages stack top to bottom: the rest are below the canvas
        }
        const QRectF visible = view.rect.intersected(canvasRect);
        const qreal area = visible.width() * visible.height();
        if (!view.valid || area <= 0.0) {
            continue;
        }
        const qreal distance = std::abs(view.rect.center().y() - canvasCenterY);
        if (!best.has_value() || area > bestArea || (area == bestArea && distance < bestDistance)) {
            best = view.pageIndex;
            bestArea = area;
            bestDistance = distance;
        }
    }
    if (best.has_value()) {
        controller_.SetCurrentPageIndex(*best);
    }
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
    // Committed text lays out at the logical zoom and paints 1:1 in logical
    // pixels, exactly like the live editor, so committing does not shift glyphs.
    params.textPixelRatio = 1.0f;
    // Per-page content epoch: a commit on one page invalidates only its own
    // cached surface.  The old session-global revision re-rendered every
    // visible page on the first paint after each stroke commit (a dense
    // neighbour ~1 s on the GUI thread), which compressed queued pointer
    // samples into sharp jumps at the next stroke's start.
    params.contentRevision = controller_.DocumentRef().PageEpoch(view.pageIndex);
    params.strokeSmoothing = controller_.InputSettings().strokeSmoothing;
    params.dashLength = controller_.InputSettings().dashLength;
    params.dashGap = controller_.InputSettings().dashGap;
    // Lazy PDF page backgrounds for pages that have a PDF source and no stored
    // raster; PageRenderer asks the provider at the current raster scale.
    params.pdfBackground = pdfBackground_;
    // Objects being edited stay out of the committed raster cache.
    if (controller_.EditingPageIndex() == view.pageIndex) {
        if (const std::optional<std::size_t> index = controller_.EditingTextIndex(); index.has_value()) {
            params.excluded.texts.push_back(*index);
        }
        if (const std::optional<std::size_t> index = controller_.EditingLatexIndex(); index.has_value()) {
            params.excluded.latexes.push_back(*index);
        }
    }
    return params;
}

CanvasWidget::SurfaceEntry* CanvasWidget::FindSurfaceEntry(
    std::size_t pageIndex, float scale, float devicePixelRatio) {
    const auto page = surfaces_.find(pageIndex);
    if (page == surfaces_.end()) {
        return nullptr;
    }
    const std::uint64_t epoch = controller_.DocumentRef().PageEpoch(pageIndex);
    for (SurfaceEntry& entry : page->second.entries) {
        if (entry.pageEpoch == epoch
            && entry.scale == scale && entry.devicePixelRatio == devicePixelRatio
            && (entry.cache.valid || entry.tiled)) {
            return &entry;
        }
    }
    return nullptr;
}

CanvasWidget::SurfaceEntry* CanvasWidget::NewestFullSurfaceEntry(std::size_t pageIndex) {
    const auto page = surfaces_.find(pageIndex);
    if (page == surfaces_.end()) {
        return nullptr;
    }
    const std::uint64_t epoch = controller_.DocumentRef().PageEpoch(pageIndex);
    for (SurfaceEntry& entry : page->second.entries) {
        if (entry.pageEpoch == epoch && !entry.cache.image.isNull()) {
            return &entry;
        }
    }
    return nullptr;
}

bool CanvasWidget::PageSurfaceFits(const CanvasPageView& view, const Page& page) const {
    const QSize size = render::PageSurfacePixelSize(page, RenderParamsFor(view));
    return render::RasterFitsBudget(size.width(), size.height());
}

std::optional<QRect> CanvasWidget::VisiblePageDeviceRect(
    const CanvasPageView& view, const Page& page, const QRect& clip) const {
    if (!view.valid || view.rect.isEmpty()) {
        return std::nullopt;
    }
    const QRectF visible = view.rect.intersected(QRectF(clip));
    if (visible.isEmpty()) {
        return std::nullopt;
    }
    // Full-page device size and view size can differ by one pixel because each
    // rounds independently; scale the logical offsets by their ratio so the
    // visible region lands on the same pixels the full surface covers.
    const QSize pageSize = render::PageSurfacePixelSize(page, RenderParamsFor(view));
    const double scaleX = static_cast<double>(pageSize.width()) / std::max(1.0, view.rect.width());
    const double scaleY = static_cast<double>(pageSize.height()) / std::max(1.0, view.rect.height());
    const QRect device(
        static_cast<int>(std::floor((visible.left() - view.rect.left()) * scaleX)),
        static_cast<int>(std::floor((visible.top() - view.rect.top()) * scaleY)),
        static_cast<int>(std::ceil((visible.right() - visible.left()) * scaleX)),
        static_cast<int>(std::ceil((visible.bottom() - visible.top()) * scaleY)));
    const QRect clamped = device.intersected(QRect(QPoint(0, 0), pageSize));
    if (clamped.isEmpty()) {
        return std::nullopt;
    }
    return clamped;
}

int CanvasWidget::SurfaceTileSize(const CanvasPageView& view) const {
    return render::PageTileSize(RenderParamsFor(view));
}

void CanvasWidget::EnsureVisibleTiles(SurfaceEntry& entry, const CanvasPageView& view, const Page& page,
    const QRect& visibleDeviceRect, std::uint64_t signature, std::size_t& renderBudget, bool& deferred) {
    if (entry.tileSignature != signature) {
        // A committed change (or any signature input) invalidates every tile of
        // the page; they are excerpts of the same content.
        entry.tiles.clear();
        entry.tileSignature = signature;
    }
    const QSize pageSize = render::PageSurfacePixelSize(page, RenderParamsFor(view));
    const QRect visible = visibleDeviceRect.intersected(QRect(QPoint(0, 0), pageSize));
    if (visible.isEmpty()) {
        return;
    }
    render::PageRenderParams params = RenderParamsFor(view);
    if (params.pdfBackground != nullptr) {
        // One PDFium render of the visible page region feeds every tile, so a
        // glyph split by a tile boundary is never re-rasterized with a
        // different subpixel phase elsewhere.
        params.pdfBackgroundRegion = visible;
    }
    const int tileSize = SurfaceTileSize(view);
    const int firstColumn = visible.left() / tileSize;
    const int lastColumn = visible.right() / tileSize;
    const int firstRow = visible.top() / tileSize;
    const int lastRow = visible.bottom() / tileSize;
    for (int row = firstRow; row <= lastRow; ++row) {
        for (int column = firstColumn; column <= lastColumn; ++column) {
            const QRect tileRect(
                column * tileSize, row * tileSize,
                std::min(tileSize, pageSize.width() - (column * tileSize)),
                std::min(tileSize, pageSize.height() - (row * tileSize)));
            if (tileRect.isEmpty()) {
                continue;
            }
            const auto existing = std::find_if(entry.tiles.begin(), entry.tiles.end(),
                [&tileRect](const SurfaceEntry::Tile& tile) { return tile.deviceRect == tileRect; });
            if (existing != entry.tiles.end()) {
                // Present tiles are current; a null image records a refused or
                // failed render so it is not retried every paint.
                continue;
            }
            const std::size_t bytes = render::RasterBytes(tileRect.width(), tileRect.height());
            if (bytes > renderBudget) {
                deferred = true;
                continue;
            }
            renderBudget -= bytes;
            lastPaintSurfaceBytes_ += bytes;
            QImage image = render::RenderPageSurfaceRegionPixels(page, params, tileRect);
            ++surfaceRenders_;
            entry.tiles.push_back(SurfaceEntry::Tile { tileRect, std::move(image) });
        }
    }
    TrimEntryTiles(entry, visible);
}

void CanvasWidget::TrimEntryTiles(SurfaceEntry& entry, const QRect& visibleDeviceRect) {
    // Keep the visible tiles (the viewport must stay covered) and only drop
    // excerpts the user has panned away from when this page alone exceeds its
    // share of the canvas budget.
    std::size_t bytes = 0;
    for (const SurfaceEntry::Tile& tile : entry.tiles) {
        if (!tile.image.isNull()) {
            bytes += static_cast<std::size_t>(tile.image.sizeInBytes());
        }
    }
    const std::size_t pageTileBudget = render::kCanvasSurfaceCacheBytes * 3U / 4U;
    while (bytes > pageTileBudget) {
        auto farthest = entry.tiles.end();
        double farthestDistance = -1.0;
        for (auto tile = entry.tiles.begin(); tile != entry.tiles.end(); ++tile) {
            if (tile->image.isNull() || tile->deviceRect.intersects(visibleDeviceRect)) {
                continue;
            }
            const QPoint tileCenter = tile->deviceRect.center();
            const QPoint visibleCenter = visibleDeviceRect.center();
            const double dx = tileCenter.x() - visibleCenter.x();
            const double dy = tileCenter.y() - visibleCenter.y();
            const double distance = (dx * dx) + (dy * dy);
            if (distance > farthestDistance) {
                farthestDistance = distance;
                farthest = tile;
            }
        }
        if (farthest == entry.tiles.end()) {
            break;
        }
        bytes -= static_cast<std::size_t>(farthest->image.sizeInBytes());
        entry.tiles.erase(farthest);
    }
}

// Blit rules (cached page rasters are 1:1 device-pixel copies, never resamples):
//  - A full-page surface is sized scale * DPR device pixels and tagged with the
//    DPR. When its tagged size equals the view's, it is drawn with the point
//    overload at view.rect.topLeft() and SmoothPixmapTransform off, so the
//    painter copies pixels 1:1. Drawing it into the target rect instead would
//    resample the whole (up to ~8 MP) image every frame.
//  - When the raster is not at this view's scale (a zoom-session snapshot, or a
//    deferred replacement) it is drawn into view.rect and resampled: the DPR
//    alone does not prove the raster's scale.
//  - A tiled surface holds raw device-pixel excerpts whose origin is an integer
//    pixel of the full-page raster. Each tile is drawn with an explicit logical
//    rect of device size / DPR, keeping the copy 1:1; the tile origin adds to
//    the same device-pixel fraction the full surface would have. If some tiles
//    are still pending (deferred by the render budget), the newest full-scale
//    snapshot is drawn underneath so no hole shows while they fill in.
bool CanvasWidget::BlitSurface(QPainter& painter, const CanvasPageView& view, const SurfaceEntry& entry,
    const QRect& clip) {
    const qreal dpr = cachedDevicePixelRatio_ > 0.0 ? cachedDevicePixelRatio_ : 1.0;
    if (!entry.cache.image.isNull()) {
        const qreal rasterDpr = entry.cache.image.devicePixelRatio() > 0.0
            ? entry.cache.image.devicePixelRatio()
            : 1.0;
        const bool rasterMatchesView = std::abs(rasterDpr - dpr) < 0.001
            && std::abs((entry.cache.image.width() / rasterDpr) - view.rect.width()) < 1.0
            && std::abs((entry.cache.image.height() / rasterDpr) - view.rect.height()) < 1.0;
        if (rasterMatchesView) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(view.rect.topLeft(), entry.cache.image);
        } else {
            painter.drawImage(view.rect, entry.cache.image);
        }
        return true;
    }
    if (entry.tiles.empty()) {
        return false;
    }

    // Pending-tile backdrop: the newest full-scale snapshot, scaled into place.
    const Page& blitPage = controller_.DocumentRef().PageAt(view.pageIndex);
    const QSize pageSize = render::PageSurfacePixelSize(blitPage, RenderParamsFor(view));
    const QRect visible = VisiblePageDeviceRect(view, blitPage, clip).value_or(QRect());
    const int tileSize = SurfaceTileSize(view);
    bool complete = !visible.isEmpty();
    if (complete) {
        const int firstColumn = visible.left() / tileSize;
        const int lastColumn = visible.right() / tileSize;
        const int firstRow = visible.top() / tileSize;
        const int lastRow = visible.bottom() / tileSize;
        for (int row = firstRow; row <= lastRow && complete; ++row) {
            for (int column = firstColumn; column <= lastColumn && complete; ++column) {
                const QRect tileRect(
                    column * tileSize, row * tileSize,
                    std::min(tileSize, pageSize.width() - (column * tileSize)),
                    std::min(tileSize, pageSize.height() - (row * tileSize)));
                const auto tile = std::find_if(entry.tiles.begin(), entry.tiles.end(),
                    [&tileRect](const SurfaceEntry::Tile& candidate) { return candidate.deviceRect == tileRect; });
                complete = tile != entry.tiles.end() && !tile->image.isNull();
            }
        }
    }
    if (!complete) {
        if (SurfaceEntry* snapshot = NewestFullSurfaceEntry(view.pageIndex); snapshot != nullptr) {
            painter.drawImage(view.rect, snapshot->cache.image);
        }
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    for (const SurfaceEntry::Tile& tile : entry.tiles) {
        if (tile.image.isNull()) {
            continue;
        }
        painter.drawImage(
            QRectF(
                view.rect.left() + (tile.deviceRect.x() / dpr),
                view.rect.top() + (tile.deviceRect.y() / dpr),
                tile.image.width() / dpr,
                tile.image.height() / dpr),
            tile.image);
    }
    return true;
}

void CanvasWidget::EnsureGestureFallback(const CanvasPageView& view, const Page& page, const QRect& clip) {
    const std::optional<QRect> visible = VisiblePageDeviceRect(view, page, clip);
    if (!visible.has_value()) {
        return;
    }
    const float dpr = cachedDevicePixelRatio_ > 0.0f ? cachedDevicePixelRatio_ : 1.0f;
    if (!gesture_.fallback.image.isNull()
        && gesture_.fallbackScale == view.scale
        && gesture_.fallbackDevicePixelRatio == dpr
        && gesture_.fallbackRegion == *visible) {
        return;
    }
    render::PageRenderParams params = RenderParamsFor(view);
    params.excluded = gesture_.excluded;
    gesture_.fallbackRegion = *visible;
    gesture_.fallbackScale = view.scale;
    gesture_.fallbackDevicePixelRatio = dpr;
    gesture_.fallback.image = render::RenderPageSurfaceRegionPixels(page, params, *visible);
    gesture_.fallback.valid = !gesture_.fallback.image.isNull();
    gesture_.fallback.signature = render::PageSurfaceSignature(page, params);
    if (!gesture_.fallback.image.isNull()) {
        // Raw device pixels: the paint path blits it in a logical rect of
        // device size / DPR, like a tile.
        gesture_.fallback.image.setDevicePixelRatio(1.0);
    }
    ++surfaceRenders_;
}

CanvasWidget::SurfaceEntry* CanvasWidget::EnsureSurfaceEntry(const CanvasPageView& view, const Page& page,
    const QRect& clip, std::size_t& renderBudget, bool& deferred) {
    PageSurfaces& perPage = surfaces_[view.pageIndex];
    const render::PageRenderParams params = RenderParamsFor(view);
    const float devicePixelRatio = params.devicePixelRatio;
    const std::uint64_t epoch = controller_.DocumentRef().PageEpoch(view.pageIndex);
    const bool fits = PageSurfaceFits(view, page);
    const std::uint64_t signature = render::PageSurfaceSignature(page, params);

    const auto prepareTiles = [&](SurfaceEntry& entry) {
        if (!entry.tiled) {
            // The page outgrew (or never fit) the full-surface budget: drop a
            // stale full raster and tile instead.
            entry.cache = render::PageSurfaceCache {};
            entry.tiled = true;
            entry.tiles.clear();
            entry.tileSignature = 0;
        }
        const std::optional<QRect> visible = VisiblePageDeviceRect(view, page, clip);
        if (!visible.has_value()) {
            return;
        }
        EnsureVisibleTiles(entry, view, page, *visible, signature, renderBudget, deferred);
    };

    for (SurfaceEntry& entry : perPage.entries) {
        // Entries keyed to a different page epoch came from before the page's
        // last committed mutation (or, after an insert/remove shifted
        // indices, from the page that previously sat at this index): they are
        // dead weight and never match, so drop them here.
        if (entry.pageEpoch != epoch) {
            entry.cache = render::PageSurfaceCache {};
            entry.tiles.clear();
            entry.tiled = false;
            entry.tileSignature = 0;
            entry.pageEpoch = epoch;
        }
        if (entry.scale != view.scale || entry.devicePixelRatio != devicePixelRatio) {
            continue;
        }
        if (!fits) {
            prepareTiles(entry);
            return &entry;
        }
        if (!entry.tiled && entry.cache.valid && entry.cache.signature == signature
            && !entry.cache.image.isNull()) {
            return &entry;
        }
        if (entry.cache.failed && entry.cache.failedSignature == signature) {
            // Same inputs, known-impossible allocation: keep the usable prior
            // raster (if any) instead of trying again on every paint.
            return &entry;
        }
        const QSize size = render::PageSurfacePixelSize(page, params);
        const std::size_t bytes = render::RasterBytes(size.width(), size.height());
        if (bytes > renderBudget) {
            deferred = true;
            return &entry;
        }
        renderBudget -= bytes;
        lastPaintSurfaceBytes_ += bytes;
        if (entry.tiled) {
            entry.tiled = false;
            entry.tiles.clear();
        }
        const std::uint64_t before = entry.cache.signature;
        render::EnsurePageSurface(entry.cache, page, params);
        if (entry.cache.signature != before) {
            ++surfaceRenders_;
        }
        // The surface is rendered at scale * DPR pixels; stamping the DPR
        // lets the paint path blit it 1:1 in device pixels.
        if (!entry.cache.image.isNull()) {
            entry.cache.image.setDevicePixelRatio(devicePixelRatio);
        }
        return &entry;
    }

    // New scale/DPR entry.  Above the per-surface budget: tile the visible
    // region.  A tiled entry needs at least one tile of render budget before it
    // is worth creating.
    if (!fits) {
        const std::optional<QRect> visible = VisiblePageDeviceRect(view, page, clip);
        if (!visible.has_value()) {
            return nullptr;
        }
        const int tileSize = SurfaceTileSize(view);
        const std::size_t tileBytes = render::RasterBytes(
            std::min(tileSize, visible->width()),
            std::min(tileSize, visible->height()));
        if (tileBytes > renderBudget) {
            deferred = true;
            return nullptr;
        }
    } else {
        const QSize size = render::PageSurfacePixelSize(page, params);
        if (render::RasterBytes(size.width(), size.height()) > renderBudget) {
            deferred = true;
            return nullptr;
        }
    }

    ++surfaceScaleMisses_;
    perPage.entries.insert(perPage.entries.begin(),
        SurfaceEntry { epoch, view.scale, devicePixelRatio, {}, 0, false, {} });
    if (perPage.entries.size() > kScalesPerPage) {
        perPage.entries.resize(kScalesPerPage);
    }
    SurfaceEntry& entry = perPage.entries.front();
    if (!fits) {
        prepareTiles(entry);
        return &entry;
    }
    const QSize size = render::PageSurfacePixelSize(page, params);
    const std::size_t bytes = std::min(renderBudget, render::RasterBytes(size.width(), size.height()));
    renderBudget -= bytes;
    lastPaintSurfaceBytes_ += bytes;
    render::EnsurePageSurface(entry.cache, page, params);
    ++surfaceRenders_;
    if (!entry.cache.image.isNull()) {
        entry.cache.image.setDevicePixelRatio(devicePixelRatio);
    }
    // If the allocation was refused (or failed) and no image exists, report
    // nothing so the caller paints live instead of blitting an empty page.
    if (entry.cache.image.isNull()) {
        return nullptr;
    }
    return &entry;
}

void CanvasWidget::ResetStrokeLayer() {
    strokeLayer_ = LiveStrokeLayer {};
}

bool CanvasWidget::SyncStrokeLayer(const Stroke& stroke, const InkPath& path, const CanvasPageView& view) {
    const float devicePixelRatio = cachedDevicePixelRatio_ > 0.0f ? cachedDevicePixelRatio_ : 1.0f;
    const float smoothing = StrokeRenderSmoothing(stroke, controller_.InputSettings().strokeSmoothing);
    const std::vector<Vec2>& points = path.points;
    const std::size_t count = points.size();
    // Pressure ink composites filled outlines instead of pen segments.
    const bool pressure = StrokeHasPressure(stroke) && path.pressures.size() == count;
    const auto pressureAt = [&path, pressure](std::size_t index) {
        return pressure ? path.pressures[index] : 0.0f;
    };
    LiveStrokeLayer& layer = strokeLayer_;

    // The layer continues only for a pure append: same page, view scale,
    // style and a point list whose already-consumed prefix is unchanged.
    // A replaced list (line/oval modes rewrite all points) rebuilds it.
    const bool appendContinues = layer.valid
        && layer.pageIndex == view.pageIndex
        && std::abs(layer.scale - view.scale) < 0.0001f
        && std::abs(layer.devicePixelRatio - devicePixelRatio) < 0.0001f
        && layer.style.color == stroke.style.color
        && layer.style.dotted == stroke.style.dotted
        && std::abs(layer.style.width - stroke.style.width) < 0.0001f
        && std::abs(layer.smoothing - smoothing) < 0.0001f
        && layer.pressure == pressure
        && count >= layer.consumedRaw
        && Vec2Equal(points.front(), layer.firstPoint)
        && Vec2Equal(points[layer.consumedRaw - 1U], layer.consumedLast);
    if (!appendContinues) {
        // Dotted strokes keep the whole-stroke preview: the dash pattern phase
        // is path-global, so compositing per segment would restart the pattern
        // at every seam.
        layer = LiveStrokeLayer {};
        if (count == 0U || stroke.style.dotted) {
            return false;
        }
        layer.valid = true;
        layer.pageIndex = view.pageIndex;
        layer.scale = view.scale;
        layer.devicePixelRatio = devicePixelRatio;
        layer.style = stroke.style;
        layer.smoothing = smoothing;
        layer.pressure = pressure;
        layer.smoother.Reset(smoothing);
        layer.smoothed.push_back(points.front());
        layer.smoothedPressures.push_back(pressureAt(0));
        layer.firstPoint = points.front();
        layer.rawLast = points.front();
        layer.lastEnd = points.front();
        layer.lastEndPressure = pressureAt(0);
        layer.consumedRaw = 1;
        layer.consumedLast = points.front();
        Vec2 ignored;
        static_cast<void>(layer.smoother.Feed(points.front(), pressureAt(0), &ignored, nullptr));
    } else if (stroke.style.dotted) {
        return false;
    }

    const float deviceScale = layer.scale * layer.devicePixelRatio;
    const float penWidth = render::StrokeRenderPenWidth(stroke, layer.scale) * layer.devicePixelRatio;
    const auto inkRadius = [&stroke, &layer](float value) {
        return 0.5f * render::StrokeRenderInkDiameter(stroke, layer.scale, value) * layer.devicePixelRatio;
    };
    const float margin = layer.style.width * 0.5f + 2.0f;    // page units; covers 1.6x pressure ink too
    const QColor color = QColor::fromRgba(layer.style.color);

    // Ensures the image covers `rect` (page units, snapped to device pixels).
    // Snapping keeps the paint-time blit at integer device coordinates, so it
    // is a 1:1 copy like the page surface rather than a resample.
    const auto ensureCover = [&layer, deviceScale](const QRectF& needed) {
        if (!layer.image.isNull() && layer.pageRect.contains(needed)) {
            return;
        }
        // Grow with slack on every side.  A wandering stroke otherwise
        // re-allocates and re-copies the whole raster nearly every sample
        // (each segment tips the union's edge a few pixels further), which
        // is O(points) copies over the gesture.  Padding the grown rect by
        // a fixed device-pixel band makes re-growth amortized over ~band
        // px of travel instead.
        constexpr int kGrowthSlackPixels = 96;
        const float slack = static_cast<float>(kGrowthSlackPixels) / deviceScale;
        const QRectF grown = (layer.image.isNull()
                ? needed
                : layer.pageRect.united(needed))
            .adjusted(-slack, -slack, slack, slack);
        const int left = static_cast<int>(std::floor(grown.left() * deviceScale));
        const int top = static_cast<int>(std::floor(grown.top() * deviceScale));
        const int right = static_cast<int>(std::ceil(grown.right() * deviceScale));
        const int bottom = static_cast<int>(std::ceil(grown.bottom() * deviceScale));
        QImage image(std::max(1, right - left), std::max(1, bottom - top),
            QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) {
            return;
        }
        image.fill(Qt::transparent);
        if (!layer.image.isNull()) {
            QPainter copy(&image);
            const int offsetX = static_cast<int>(std::lround(layer.pageRect.x() * deviceScale)) - left;
            const int offsetY = static_cast<int>(std::lround(layer.pageRect.y() * deviceScale)) - top;
            copy.drawImage(QRect(offsetX, offsetY, layer.image.width(), layer.image.height()),
                layer.image);
            copy.end();
        }
        layer.pageRect = QRectF(
            static_cast<qreal>(left) / deviceScale,
            static_cast<qreal>(top) / deviceScale,
            static_cast<qreal>(right - left) / deviceScale,
            static_cast<qreal>(bottom - top) / deviceScale);
        // The layer stays untagged (devicePixelRatio 1): QPainter on a dpr-
        // tagged QImage scales every coordinate by the dpr, which would push
        // composited segments x dpr off their device-pixel targets.  The
        // canvas blit passes an explicit logical-size rect instead.
        layer.image = std::move(image);
    };

    const auto toDevice = [&layer, deviceScale](Vec2 point) {
        return QPointF(
            static_cast<qreal>((point.x - layer.pageRect.x()) * deviceScale),
            static_cast<qreal>((point.y - layer.pageRect.y()) * deviceScale));
    };

    // Newly stable smoothed points composite as the quad chain
    // quadTo(s_{k-1}, mid(s_{k-1}, s_k)) that PaintStrokePreview's whole-path
    // build emits.  Batching them into one path keeps the joins interior
    // (round joins, no caps; one nonzero-filled outline for pressure ink)
    // instead of drawing a seam per point.
    std::vector<Vec2> pendingControls;
    std::vector<Vec2> pendingEnds;
    std::vector<float> pendingControlPressures;
    std::vector<float> pendingEndPressures;
    Vec2 batchStart {};              // lastEnd when the first pending segment was queued
    float batchStartPressure = 0.0f;
    const auto flush = [&]() {
        if (pendingControls.empty() || layer.image.isNull()) {
            pendingControls.clear();
            pendingEnds.clear();
            pendingControlPressures.clear();
            pendingEndPressures.clear();
            return;
        }
        QPainter ink(&layer.image);
        ink.setRenderHint(QPainter::Antialiasing, true);
        if (layer.pressure) {
            constexpr float kDeviceInkTolerance = 0.25f;
            const auto device = [&toDevice](Vec2 point) {
                const QPointF mapped = toDevice(point);
                return Vec2 { static_cast<float>(mapped.x()), static_cast<float>(mapped.y()) };
            };
            InkCenterline centerline;
            Vec2 start = batchStart;
            float startPressure = batchStartPressure;
            for (std::size_t i = 0; i < pendingControls.size(); ++i) {
                AppendInkQuadratic(centerline, device(start), inkRadius(startPressure),
                    device(pendingControls[i]), inkRadius(pendingControlPressures[i]),
                    device(pendingEnds[i]), inkRadius(pendingEndPressures[i]), kDeviceInkTolerance);
                start = pendingEnds[i];
                startPressure = pendingEndPressures[i];
            }
            InkOutline outline;
            AppendInkOutline(outline, centerline, kDeviceInkTolerance);
            ink.setPen(Qt::NoPen);
            ink.fillPath(render::InkOutlinePath(outline), color);
        } else {
            QPainterPath path;
            path.moveTo(toDevice(batchStart));
            for (std::size_t i = 0; i < pendingControls.size(); ++i) {
                path.quadTo(toDevice(pendingControls[i]), toDevice(pendingEnds[i]));
            }
            ink.setPen(QPen(color, static_cast<qreal>(penWidth), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            ink.setBrush(Qt::NoBrush);
            ink.drawPath(path);
        }
        ink.end();
        pendingControls.clear();
        pendingEnds.clear();
        pendingControlPressures.clear();
        pendingEndPressures.clear();
    };

    while (layer.consumedRaw < count) {
        const Vec2 raw = points[layer.consumedRaw];
        Vec2 candidate;
        float candidatePressure = 0.0f;
        if (layer.smoother.Feed(raw, pressureAt(layer.consumedRaw), &candidate, &candidatePressure)
            && DistanceSquared(candidate, layer.smoothed.back()) >= 0.05f) {
            const Vec2 control = layer.smoothed.back();
            const float controlPressure = layer.smoothedPressures.back();
            const Vec2 end {
                (control.x + candidate.x) * 0.5f,
                (control.y + candidate.y) * 0.5f,
            };
            const float endPressure = (controlPressure + candidatePressure) * 0.5f;
            const QRectF needed = QRectF(
                QPointF(std::min({ layer.lastEnd.x, control.x, candidate.x }) - margin,
                    std::min({ layer.lastEnd.y, control.y, candidate.y }) - margin),
                QPointF(std::max({ layer.lastEnd.x, control.x, candidate.x }) + margin,
                    std::max({ layer.lastEnd.y, control.y, candidate.y }) + margin));
            ensureCover(needed);
            if (layer.image.isNull()) {
                // Allocation failure: keep painting live instead of dropping ink.
                layer = LiveStrokeLayer {};
                return false;
            }
            if (pendingControls.empty()) {
                batchStart = layer.lastEnd;
                batchStartPressure = layer.lastEndPressure;
            }
            pendingControls.push_back(control);
            pendingEnds.push_back(end);
            pendingControlPressures.push_back(controlPressure);
            pendingEndPressures.push_back(endPressure);
            layer.smoothed.push_back(candidate);
            layer.smoothedPressures.push_back(candidatePressure);
            layer.lastEnd = end;
            layer.lastEndPressure = endPressure;
        }
        layer.rawLast = raw;
        layer.consumedLast = raw;
        ++layer.consumedRaw;
    }
    flush();
    return true;
}

void CanvasWidget::PaintStrokeTail(QPainter& painter, const Stroke& stroke, const InkPath& path,
    const CanvasPageView& view) {
    const LiveStrokeLayer& layer = strokeLayer_;
    const float scale = view.scale;
    const float penWidth = render::StrokeRenderPenWidth(stroke, scale);
    const QColor color = QColor::fromRgba(stroke.style.color);
    const std::vector<Vec2>& points = path.points;

    // The provisional tail mirrors PaintStrokePreview's path over
    // smoothed[m-1..] plus the raw tail point (emitted or not by the same
    // 0.01 squared-distance clamp BuildSmoothedPath uses).
    Vec2 tail {};
    const bool hasTail = !points.empty()
        && layer.smoother.TailPoint(points.back(), layer.smoothed.back(), &tail);
    const std::size_t k = layer.smoothed.size() + (hasTail ? 1U : 0U);
    const Vec2 end = hasTail ? tail : layer.smoothed.back();

    if (layer.pressure) {
        // The same pieces as a filled outline, radii from the smoothed
        // pressures and the raw tail's.
        constexpr float kInkTolerance = 0.2f;
        const auto at = [scale](Vec2 point) { return Vec2 { point.x * scale, point.y * scale }; };
        const auto radius = [&stroke, scale](float value) {
            return 0.5f * render::StrokeRenderInkDiameter(stroke, scale, value);
        };
        const float tailPressure = path.pressures.empty() ? 0.0f : path.pressures.back();
        const float endPressure = hasTail ? tailPressure : layer.smoothedPressures.back();
        InkCenterline centerline;
        if (k <= 1U) {
            centerline.Add(at(layer.smoothed.front()), radius(layer.smoothedPressures.front()));
        } else if (k == 2U || !hasTail) {
            centerline.Add(at(layer.lastEnd), radius(layer.lastEndPressure));
            centerline.Add(at(end), radius(endPressure));
        } else {
            const Vec2 control = layer.smoothed.back();
            const float controlPressure = layer.smoothedPressures.back();
            const Vec2 middle { (control.x + tail.x) * 0.5f, (control.y + tail.y) * 0.5f };
            AppendInkQuadratic(centerline, at(layer.lastEnd), radius(layer.lastEndPressure), at(control),
                radius(controlPressure), at(middle), radius((controlPressure + tailPressure) * 0.5f), kInkTolerance);
            centerline.Add(at(tail), radius(tailPressure));
        }
        InkOutline outline;
        AppendInkOutline(outline, centerline, kInkTolerance);
        painter.setPen(Qt::NoPen);
        painter.fillPath(render::InkOutlinePath(outline), color);
        return;
    }

    painter.setBrush(Qt::NoBrush);
    if (k <= 1U) {
        // Single-point dot: same filled-ellipse rule as PaintStrokePreview.
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const qreal radius = static_cast<qreal>(penWidth * 0.5f);
        painter.drawEllipse(
            QPointF(static_cast<qreal>(layer.smoothed.front().x * scale),
                static_cast<qreal>(layer.smoothed.front().y * scale)),
            radius, radius);
        return;
    }

    painter.setPen(QPen(color, static_cast<qreal>(penWidth), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF from(static_cast<qreal>(layer.lastEnd.x * scale),
        static_cast<qreal>(layer.lastEnd.y * scale));
    const QPointF to(static_cast<qreal>(end.x * scale), static_cast<qreal>(end.y * scale));
    if (k == 2U) {
        painter.drawLine(from, to);
        return;
    }
    QPainterPath line;
    line.moveTo(from);
    if (hasTail) {
        const Vec2 control = layer.smoothed.back();
        line.quadTo(
            QPointF(static_cast<qreal>(control.x * scale), static_cast<qreal>(control.y * scale)),
            QPointF(static_cast<qreal>((control.x + tail.x) * 0.5f * scale),
                static_cast<qreal>((control.y + tail.y) * 0.5f * scale)));
    }
    line.lineTo(to);
    painter.drawPath(line);
}

void CanvasWidget::BeginGestureOverlay(std::size_t pageIndex, bool transform) {
    const PageSelection& selection = controller_.SelectionFor(pageIndex);
    gesture_ = GestureSurface {};
    gesture_.pageIndex = pageIndex;
    gesture_.transform = transform;
    gesture_.only.strokes = selection.strokes;
    gesture_.only.images = selection.images;
    gesture_.only.texts = selection.texts;
    gesture_.only.latexes = selection.latexes;
    const auto sorted = [](std::vector<std::size_t>& list) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    };
    sorted(gesture_.only.strokes);
    sorted(gesture_.only.images);
    sorted(gesture_.only.texts);
    sorted(gesture_.only.latexes);
    // The patch excludes the dragged selection plus whatever the edit overlays
    // already exclude (the editor paints that object itself).
    gesture_.excluded = gesture_.only;
    if (controller_.EditingPageIndex() == pageIndex) {
        if (const std::optional<std::size_t> index = controller_.EditingTextIndex(); index.has_value()) {
            gesture_.excluded.texts.push_back(*index);
            sorted(gesture_.excluded.texts);
        }
        if (const std::optional<std::size_t> index = controller_.EditingLatexIndex(); index.has_value()) {
            gesture_.excluded.latexes.push_back(*index);
            sorted(gesture_.excluded.latexes);
        }
    }
    // The vacated region is the selection bounds at gesture begin, inflated by
    // the widest selected stroke's half width plus a small margin so stroke
    // caps/joins cannot bleed past the patch.  It is clamped to the page so
    // the blit origin matches the renderer's clipped region.
    const std::optional<SelectionBounds> bounds = SelectionBoundsForPage(pageIndex);
    if (bounds.has_value()) {
        float margin = 2.0f;
        const Document& document = controller_.DocumentRef();
        if (pageIndex < document.PageCount()) {
            const Page& page = document.PageAt(pageIndex);
            for (const std::size_t index : gesture_.only.strokes) {
                if (index < page.strokes.size()) {
                    margin = std::max(margin, page.strokes[index].style.width * 0.5f + 2.0f);
                }
            }
            gesture_.patchRegion = QRectF(
                bounds->left - margin, bounds->top - margin,
                (bounds->right - bounds->left) + margin * 2.0,
                (bounds->bottom - bounds->top) + margin * 2.0)
                .intersected(QRectF(0.0, 0.0, page.width, page.height));

            // Move gestures blit one cached raster per object.  Only the kind
            // and index are captured here: the raster's bounds and track point
            // are measured when it is rendered (first gesture paint), so a
            // move that already landed before the first paint is not applied
            // twice.
            if (!transform) {
                const auto addItem = [this](ObjectKind kind, std::size_t index) {
                    GestureItem item;
                    item.kind = kind;
                    item.index = index;
                    gesture_.items.push_back(item);
                };
                for (const std::size_t index : gesture_.only.strokes) {
                    addItem(ObjectKind::Stroke, index);
                }
                for (const std::size_t index : gesture_.only.images) {
                    addItem(ObjectKind::Image, index);
                }
                for (const std::size_t index : gesture_.only.texts) {
                    addItem(ObjectKind::Text, index);
                }
                for (const std::size_t index : gesture_.only.latexes) {
                    addItem(ObjectKind::Latex, index);
                }
            }
        }
    }
}

void CanvasWidget::EndGestureOverlay() {
    gesture_ = GestureSurface {};
}

QRect CanvasWidget::SelectionDirtyRect(std::size_t pageIndex) const {
    const std::optional<SelectionBounds> bounds = SelectionBoundsForPage(pageIndex);
    if (!bounds.has_value()) {
        return rect();
    }
    const CanvasPageView view = CalculatePageView(pageIndex);
    if (!view.valid) {
        return rect();
    }
    // The padded frame's handles reach the rotate handle's top above it, and
    // less on the other sides; one margin covers them plus antialiasing.
    const qreal margin = kSelectionRotateGap + kSelectionHandleSize + 2.0;
    return SelectionFrameRect(view, *bounds)
        .adjusted(-margin, -margin, margin, margin)
        .toAlignedRect()
        .intersected(rect());
}

void CanvasWidget::TrimSurfaceCache(const std::vector<std::size_t>& visiblePages) {
    // A page that left the viewport stays cached until the aggregate raster
    // bytes pass the budget, so scrolling back to a nearby page blits instead
    // of re-rendering.  Visible pages and their current-scale entries are kept;
    // stale scale/DPR entries of visible pages only survive while a zoom
    // session still uses them as snapshots.
    if (!zoomSession_.has_value()) {
        for (const std::size_t pageIndex : visiblePages) {
            const auto page = surfaces_.find(pageIndex);
            if (page == surfaces_.end()) {
                continue;
            }
            const CanvasPageView view = CalculatePageView(pageIndex);
            std::vector<SurfaceEntry>& entries = page->second.entries;
            for (auto entry = entries.begin(); entry != entries.end();) {
                if (std::abs(entry->scale - view.scale) > 0.0001f
                    || std::abs(entry->devicePixelRatio - cachedDevicePixelRatio_) > 0.0001f) {
                    entry = entries.erase(entry);
                } else {
                    ++entry;
                }
            }
        }
    }

    const auto pageBytes = [](const PageSurfaces& page) {
        std::size_t bytes = 0;
        for (const SurfaceEntry& entry : page.entries) {
            if (!entry.cache.image.isNull()) {
                bytes += static_cast<std::size_t>(entry.cache.image.sizeInBytes());
            }
            for (const SurfaceEntry::Tile& tile : entry.tiles) {
                if (!tile.image.isNull()) {
                    bytes += static_cast<std::size_t>(tile.image.sizeInBytes());
                }
            }
        }
        return bytes;
    };
    const auto distanceToVisible = [&visiblePages](std::size_t pageIndex) {
        std::size_t distance = std::numeric_limits<std::size_t>::max();
        for (const std::size_t visible : visiblePages) {
            distance = std::min(distance, pageIndex > visible ? pageIndex - visible : visible - pageIndex);
        }
        return distance;
    };

    std::size_t totalBytes = 0;
    for (const auto& page : surfaces_) {
        totalBytes += pageBytes(page.second);
    }
    while (totalBytes > render::kCanvasSurfaceCacheBytes) {
        auto victim = surfaces_.end();
        std::size_t victimDistance = 0;
        for (auto page = surfaces_.begin(); page != surfaces_.end(); ++page) {
            if (std::find(visiblePages.begin(), visiblePages.end(), page->first) != visiblePages.end()) {
                continue;
            }
            const std::size_t distance = distanceToVisible(page->first);
            if (victim == surfaces_.end() || distance > victimDistance) {
                victim = page;
                victimDistance = distance;
            }
        }
        if (victim == surfaces_.end()) {
            break;  // only visible pages remain; they must stay rendered
        }
        totalBytes -= pageBytes(victim->second);
        surfaces_.erase(victim);
    }
    peakSurfaceBytes_ = std::max(peakSurfaceBytes_, CachedSurfaceBytes());
}

void CanvasWidget::NotifyDocumentChanged() {
    // Repaint request, not a cache drop: every committed change is covered by
    // the per-page signature (content hash) the renderer checks on the next
    // paint, and CommitStroke already composites its stroke into the cached
    // raster. Clearing here re-rendered a dense page (~1 s) on the paint after
    // each commit, which delayed the first frame of the next stroke by ~10
    // refresh intervals (measured on dense-1000x100 at a 0.8 s cadence).
    // Inserted, removed or resized pages move the layout under the viewport.
    SyncFocusedPage();
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

std::size_t CanvasWidget::CachedSurfaceBytes() const {
    std::size_t bytes = 0;
    for (const auto& page : surfaces_) {
        for (const SurfaceEntry& entry : page.second.entries) {
            if (!entry.cache.image.isNull()) {
                bytes += static_cast<std::size_t>(entry.cache.image.sizeInBytes());
            }
            for (const SurfaceEntry::Tile& tile : entry.tiles) {
                if (!tile.image.isNull()) {
                    bytes += static_cast<std::size_t>(tile.image.sizeInBytes());
                }
            }
        }
    }
    return bytes;
}

std::size_t CanvasWidget::CachedTileCount() const {
    std::size_t count = 0;
    for (const auto& page : surfaces_) {
        for (const SurfaceEntry& entry : page.second.entries) {
            count += entry.tiles.size();
        }
    }
    return count;
}

// --- painting --------------------------------------------------------------

void CanvasWidget::paintEvent(QPaintEvent* event) {
    lastPaintSurfaceBytes_ = 0;
    const float devicePixelRatio = static_cast<float>(devicePixelRatioF());
    if (cachedDevicePixelRatio_ != devicePixelRatio) {
        // Display/DPR change: raster caches are keyed by their effective scale.
        cachedDevicePixelRatio_ = devicePixelRatio;
        surfaces_.clear();
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (transparentBackdrop_) {
        // The window's backdrop (ui::ApplyWindowBackdrop) shows around the pages.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(event->rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    } else {
        painter.fillRect(event->rect(), QColor::fromRgba(kCanvasBackdrop));
    }

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

    // Prepare the page rasters this paint blits, visible pages first (the
    // current page, then the largest visible area) and inside one paint's
    // raster-byte budget.  A zoom settle therefore replaces the visible pages
    // across frames instead of re-rendering every one inside a single paint;
    // a deferred page paints from its newest snapshot (or vector-live) and its
    // rect is repainted on the next frame.
    struct PendingSurface {
        const CanvasPageView* view = nullptr;
        qreal visibleArea = 0.0;
    };
    std::vector<PendingSurface> pendingSurfaces;
    std::unordered_map<std::size_t, SurfaceEntry*> paintSurfaces;
    const std::size_t currentPage = document.CurrentPageIndex();
    for (const CanvasPageView& view : views) {
        if (!view.valid || view.pageIndex >= document.PageCount()) {
            continue;
        }
        const qreal padding = zoomSession_.has_value() ? 0.0 : 20.0;
        if (view.rect.bottom() < clip.top() - padding || view.rect.top() > clip.bottom() + padding) {
            continue;
        }
        if (!view.rect.intersects(clip)) {
            continue;
        }
        // A move/transform gesture mutates its selected objects in place
        // (translation/transform), which the committed-raster signature would
        // see as changes; the gesture path blits the pre-gesture raster and
        // paints the moved selection itself, so it must not re-render here.
        const bool gestureOnPage = controller_.GesturePageIndex() == view.pageIndex
            && gesture_.pageIndex == view.pageIndex
            && !gesture_.only.Empty();
        if (gestureOnPage) {
            continue;
        }
        // Zoom sessions keep the previous scale's snapshot: rendering a new
        // exact surface mid-gesture is what made a zoom step stall.
        if (zoomSession_.has_value()
            && controller_.PendingStrokePage() != view.pageIndex
            && NewestFullSurfaceEntry(view.pageIndex) != nullptr) {
            continue;
        }
        const QRectF visible = view.rect.intersected(clip);
        pendingSurfaces.push_back(PendingSurface { &view, visible.width() * visible.height() });
    }
    std::sort(pendingSurfaces.begin(), pendingSurfaces.end(),
        [currentPage](const PendingSurface& a, const PendingSurface& b) {
            const bool aCurrent = a.view->pageIndex == currentPage;
            const bool bCurrent = b.view->pageIndex == currentPage;
            if (aCurrent != bCurrent) {
                return aCurrent;
            }
            return a.visibleArea > b.visibleArea;
        });
    std::size_t renderBudget = render::kSurfaceRenderBytesPerPaint;
    std::vector<QRect> deferredSurfaceRects;
    for (const PendingSurface& pending : pendingSurfaces) {
        const Page& page = document.PageAt(pending.view->pageIndex);
        bool deferred = false;
        SurfaceEntry* entry = EnsureSurfaceEntry(*pending.view, page, event->rect(), renderBudget, deferred);
        if (entry != nullptr) {
            paintSurfaces[pending.view->pageIndex] = entry;
        }
        if (deferred) {
            deferredSurfaceRects.push_back(pending.view->rect.toAlignedRect().intersected(rect()));
        }
        peakSurfaceBytes_ = std::max(peakSurfaceBytes_, CachedSurfaceBytes());
    }

    for (const CanvasPageView& view : views) {
        if (!view.valid || view.pageIndex >= document.PageCount()) {
            continue;
        }

        // Reference culling: a page is considered when it is within the paint
        // bounds plus 20 px (0 while a zoom session previews a snapshot).
        const qreal padding = zoomSession_.has_value() ? 0.0 : 20.0;
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
        // A move/transform gesture on this page blits the committed raster,
        // covers the vacated region with a once-per-gesture patch and paints
        // the dragged selection live on top — no per-sample re-render.
        const bool gestureOnPage = controller_.GesturePageIndex() == view.pageIndex
            && gesture_.pageIndex == view.pageIndex
            && !gesture_.only.Empty();
        // The pre-pass prepared this page's raster (a full surface or visible
        // tiles); a page whose raster was deferred is repainted next frame.
        SurfaceEntry* entry = nullptr;
        if (const auto prepared = paintSurfaces.find(view.pageIndex); prepared != paintSurfaces.end()) {
            entry = prepared->second;
        }
        bool gestureFallback = false;
        if (gestureOnPage && !pageHasPendingErase) {
            if (entry == nullptr) {
                entry = FindSurfaceEntry(view.pageIndex, view.scale, cachedDevicePixelRatio_);
            }
            if (gesture_.patch.isNull()
                || gesture_.scale != view.scale
                || gesture_.devicePixelRatio != cachedDevicePixelRatio_) {
                render::PageRenderParams params = RenderParamsFor(view);
                params.excluded = gesture_.excluded;
                // Refused above the raster budget: the bounded fallback below
                // covers the vacated region instead.
                gesture_.patch = render::RenderPageSurfaceRegion(page, params, gesture_.patchRegion);
                gesture_.scale = view.scale;
                gesture_.devicePixelRatio = cachedDevicePixelRatio_;
                if (!gesture_.patch.isNull()) {
                    gesture_.patch.setDevicePixelRatio(cachedDevicePixelRatio_);
                }
                ++surfaceRenders_;
            }
            if (entry == nullptr || gesture_.patch.isNull()) {
                // No committed raster (or no patch inside the budget) to
                // reuse: render the visible region once with the selection
                // excluded so later moves cannot ghost.
                gestureFallback = true;
                gesture_.patch = QImage();
                EnsureGestureFallback(view, page, event->rect());
            }
        } else if (pageHasPendingErase) {
            // The committed raster is still valid during an erase drag (marks
            // only fade; nothing is deleted yet).  Blit it and overlay the
            // re-rendered patch that carries the faded objects, instead of
            // re-painting the whole page vector-live every sample.
            if (entry == nullptr) {
                entry = FindSurfaceEntry(view.pageIndex, view.scale, cachedDevicePixelRatio_);
            }
            if (entry == nullptr) {
                entry = NewestFullSurfaceEntry(view.pageIndex);
            }
        } else if (zoomSession_.has_value() && !liveStrokeOnPage) {
            // Zoom snapshot: reuse the newest raster (drawn scaled) until the
            // session settles, instead of re-rendering the page mid-gesture.
            if (entry == nullptr) {
                entry = FindSurfaceEntry(view.pageIndex, view.scale, cachedDevicePixelRatio_);
            }
        }

        painter.save();
        painter.setClipRect(view.rect);
        if (gestureOnPage && !pageHasPendingErase) {
            const qreal dpr = cachedDevicePixelRatio_ > 0.0 ? cachedDevicePixelRatio_ : 1.0;
            if (gestureFallback) {
                if (!gesture_.fallback.image.isNull()) {
                    // Raw device pixels: blit in a logical rect of device size /
                    // DPR (the tile rule above), 1:1.
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                    painter.drawImage(
                        QRectF(
                            view.rect.left() + (gesture_.fallbackRegion.x() / dpr),
                            view.rect.top() + (gesture_.fallbackRegion.y() / dpr),
                            gesture_.fallback.image.width() / dpr,
                            gesture_.fallback.image.height() / dpr),
                        gesture_.fallback.image);
                } else {
                    render::PageRenderParams params = RenderParamsFor(view);
                    params.excluded = gesture_.excluded;
                    render::PaintPage(painter, page, view.rect.topLeft(), params);
                }
            } else if (entry != nullptr && BlitSurface(painter, view, *entry, event->rect())) {
                if (!gesture_.patch.isNull()) {
                    // Cover the region the selection vacated: the committed
                    // raster still shows it at its original position.  The
                    // patch is rendered at scale * DPR in device pixels, so it
                    // blits 1:1 at its region's top-left.
                    painter.drawImage(
                        QPointF(view.rect.left() + gesture_.patchRegion.x() * view.scale,
                            view.rect.top() + gesture_.patchRegion.y() * view.scale),
                        gesture_.patch);
                }
            } else {
                render::PageRenderParams params = RenderParamsFor(view);
                params.excluded = gesture_.excluded;
                render::PaintPage(painter, page, view.rect.topLeft(), params);
            }
        } else if (pageHasPendingErase) {
            bool erasePatchReady = false;
            if (entry != nullptr && BlitSurface(painter, view, *entry, event->rect())) {
                // Overlay the faded objects: one re-rendered patch keyed on the
                // page signature (which covers the pending index lists), reused
                // for every sample where the pending set is unchanged.
                render::PageRenderParams params = RenderParamsFor(view);
                params.pendingErase = pendingErase;
                const std::uint64_t signature = render::PageSurfaceSignature(page, params);
                if (erasePatch_.isNull() || erasePatchSignature_ != signature
                    || erasePatchPage_ != view.pageIndex) {
                    // RenderPageSurfaceRegion clamps to the page internally but
                    // does not report the clamped rect, so clip here first or the
                    // patch would land offset from its true position.
                    erasePatchRegion_ = PendingErasePageRegion(view.pageIndex)
                        .intersected(QRectF(0.0, 0.0, page.width, page.height));
                    erasePatch_ = render::RenderPageSurfaceRegion(page, params, erasePatchRegion_);
                    if (!erasePatch_.isNull()) {
                        erasePatch_.setDevicePixelRatio(cachedDevicePixelRatio_);
                    }
                    erasePatchSignature_ = signature;
                    erasePatchPage_ = view.pageIndex;
                    ++surfaceRenders_;
                }
                erasePatchReady = !erasePatch_.isNull() && !erasePatchRegion_.isEmpty();
                if (erasePatchReady) {
                    // The patch is rendered at scale * DPR in device pixels, so it
                    // blits 1:1 at its region's top-left (same as gesture_.patch).
                    painter.drawImage(
                        QPointF(view.rect.left() + erasePatchRegion_.x() * view.scale,
                            view.rect.top() + erasePatchRegion_.y() * view.scale),
                        erasePatch_);
                }
            }
            if (!erasePatchReady) {
                // No reusable raster, or the marked region's patch is above the
                // raster budget: repaint the page vector-live with the fade.
                render::PageRenderParams params = RenderParamsFor(view);
                params.pendingErase = pendingErase;
                render::PaintPage(painter, page, view.rect.topLeft(), params);
            }
        } else if (entry != nullptr && BlitSurface(painter, view, *entry, event->rect())) {
            // Cached raster blitted 1:1 (full surface or visible tiles).
        } else if (SurfaceEntry* snapshot = NewestFullSurfaceEntry(view.pageIndex); snapshot != nullptr) {
            // No raster at this scale yet (deferred replacement or a zoom
            // session's first frame): reuse the newest snapshot, resampled.
            painter.drawImage(view.rect, snapshot->cache.image);
        } else {
            // Nothing cached at any scale: paint the page vector-live.  This is
            // bounded memory; the pre-pass's deferred render replaces it.
            render::PaintPage(painter, page, view.rect.topLeft(), RenderParamsFor(view));
        }

        if (gestureOnPage && !pageHasPendingErase) {
            if (!gesture_.transform && !gesture_.items.empty()) {
                // Move: blit each object's cached raster at its live offset.
                // The rasters are rendered once per gesture at scale * DPR.
                if (gesture_.itemsScale != view.scale
                    || gesture_.itemsDevicePixelRatio != cachedDevicePixelRatio_) {
                    const float deviceScale = view.scale * cachedDevicePixelRatio_;
                    for (GestureItem& item : gesture_.items) {
                        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
                        float margin = 2.0f;
                        render::ExcludedIndices one;
                        std::vector<std::size_t>* list = nullptr;
                        bool valid = true;
                        switch (item.kind) {
                        case ObjectKind::Stroke:
                            list = &one.strokes;
                            if (item.index >= page.strokes.size()
                                || !StrokeBounds(page.strokes[item.index], &minX, &minY, &maxX, &maxY)) {
                                valid = false;
                                break;
                            }
                            margin = page.strokes[item.index].style.width * 0.5f + 2.0f;
                            item.trackX = page.strokes[item.index].points.front().x;
                            item.trackY = page.strokes[item.index].points.front().y;
                            break;
                        case ObjectKind::Image:
                            list = &one.images;
                            if (item.index >= page.images.size()) {
                                valid = false;
                                break;
                            }
                            minX = page.width;
                            minY = page.height;
                            for (const Vec2& corner : ImageCorners(page.images[item.index])) {
                                minX = std::min(minX, corner.x);
                                minY = std::min(minY, corner.y);
                                maxX = std::max(maxX, corner.x);
                                maxY = std::max(maxY, corner.y);
                            }
                            item.trackX = page.images[item.index].x;
                            item.trackY = page.images[item.index].y;
                            break;
                        case ObjectKind::Text:
                            list = &one.texts;
                            if (item.index >= page.texts.size()) {
                                valid = false;
                                break;
                            }
                            {
                                const text::QtTextBounds textBounds =
                                    text::MeasurePageTextBoundsQt(page.texts[item.index], page.width);
                                minX = page.width;
                                minY = page.height;
                                maxX = 0.0f;
                                maxY = 0.0f;
                                for (const Vec2& corner : RotatedRectCorners(
                                        textBounds.left, textBounds.top, textBounds.right, textBounds.bottom,
                                        page.texts[item.index].rotationDegrees)) {
                                    minX = std::min(minX, corner.x);
                                    minY = std::min(minY, corner.y);
                                    maxX = std::max(maxX, corner.x);
                                    maxY = std::max(maxY, corner.y);
                                }
                            }
                            item.trackX = page.texts[item.index].x;
                            item.trackY = page.texts[item.index].y;
                            break;
                        case ObjectKind::Latex:
                            list = &one.latexes;
                            if (item.index >= page.latexes.size()) {
                                valid = false;
                                break;
                            }
                            {
                                const LatexBounds latexBounds =
                                    MeasurePageLatexBounds(page.latexes[item.index], page.width);
                                minX = page.width;
                                minY = page.height;
                                maxX = 0.0f;
                                maxY = 0.0f;
                                for (const Vec2& corner : RotatedRectCorners(
                                        latexBounds.left, latexBounds.top, latexBounds.right, latexBounds.bottom,
                                        page.latexes[item.index].rotationDegrees)) {
                                    minX = std::min(minX, corner.x);
                                    minY = std::min(minY, corner.y);
                                    maxX = std::max(maxX, corner.x);
                                    maxY = std::max(maxY, corner.y);
                                }
                            }
                            item.trackX = page.latexes[item.index].x;
                            item.trackY = page.latexes[item.index].y;
                            break;
                        }
                        if (!valid) {
                            item.image = QImage();
                            item.live = false;
                            continue;
                        }
                        item.originX = minX - margin;
                        item.originY = minY - margin;
                        item.width = (maxX - minX) + margin * 2.0f;
                        item.height = (maxY - minY) + margin * 2.0f;
                        list->push_back(item.index);
                        // Check the raster size before allocating: an object
                        // spanning the page at max zoom/DPR would otherwise ask
                        // for an impossible image every gesture.
                        const std::optional<QSize> rasterSize = render::CheckedRasterSize(
                            static_cast<double>(item.width) * deviceScale,
                            static_cast<double>(item.height) * deviceScale);
                        if (!rasterSize.has_value()) {
                            item.image = QImage();
                            item.live = true;
                            continue;
                        }
                        item.live = false;
                        render::PageRenderParams params = RenderParamsFor(view);
                        params.scale = deviceScale;
                        params.devicePixelRatio = 1.0f;
                        params.textPixelRatio = cachedDevicePixelRatio_;
                        item.image = QImage(*rasterSize, QImage::Format_ARGB32_Premultiplied);
                        item.image.fill(Qt::transparent);
                        QPainter itemPainter(&item.image);
                        render::PaintPageSubset(itemPainter, page,
                            QPointF(-item.originX * deviceScale, -item.originY * deviceScale),
                            params, one);
                        itemPainter.end();
                        item.image.setDevicePixelRatio(cachedDevicePixelRatio_);
                    }
                    gesture_.itemsScale = view.scale;
                    gesture_.itemsDevicePixelRatio = cachedDevicePixelRatio_;
                    ++surfaceRenders_;
                }
                for (const GestureItem& item : gesture_.items) {
                    if (item.live) {
                        // Above the raster budget: paint just this object live
                        // on top (bounded memory, exact vector output).
                        render::ExcludedIndices one;
                        switch (item.kind) {
                        case ObjectKind::Stroke:
                            one.strokes.push_back(item.index);
                            break;
                        case ObjectKind::Image:
                            one.images.push_back(item.index);
                            break;
                        case ObjectKind::Text:
                            one.texts.push_back(item.index);
                            break;
                        case ObjectKind::Latex:
                            one.latexes.push_back(item.index);
                            break;
                        }
                        render::PaintPageSubset(painter, page, view.rect.topLeft(), RenderParamsFor(view), one);
                        continue;
                    }
                    if (item.image.isNull()) {
                        continue;
                    }
                    float currentX = item.trackX;
                    float currentY = item.trackY;
                    switch (item.kind) {
                    case ObjectKind::Stroke:
                        if (item.index < page.strokes.size() && !page.strokes[item.index].points.empty()) {
                            currentX = page.strokes[item.index].points.front().x;
                            currentY = page.strokes[item.index].points.front().y;
                        }
                        break;
                    case ObjectKind::Image:
                        if (item.index < page.images.size()) {
                            currentX = page.images[item.index].x;
                            currentY = page.images[item.index].y;
                        }
                        break;
                    case ObjectKind::Text:
                        if (item.index < page.texts.size()) {
                            currentX = page.texts[item.index].x;
                            currentY = page.texts[item.index].y;
                        }
                        break;
                    case ObjectKind::Latex:
                        if (item.index < page.latexes.size()) {
                            currentX = page.latexes[item.index].x;
                            currentY = page.latexes[item.index].y;
                        }
                        break;
                    }
                    painter.drawImage(
                        QPointF(view.rect.left() + (item.originX + currentX - item.trackX) * view.scale,
                            view.rect.top() + (item.originY + currentY - item.trackY) * view.scale),
                        item.image);
                }
            } else {
                // Transform: the dragged selection paints live on top of the
                // patched raster (scaling/rotation change stroke widths and
                // text layout, so cached rasters do not apply).
                render::PaintPageSubset(painter, page, view.rect.topLeft(), RenderParamsFor(view), gesture_.only);
            }
        }

        // A snapped stroke's freehand original, faded under the shape.
        if (snapGhost_.has_value() && snapGhost_->pageIndex == view.pageIndex) {
            painter.save();
            painter.translate(view.rect.topLeft());
            render::PaintStrokePreview(painter, snapGhost_->stroke, RenderParamsFor(view), SnapGhostOpacity());
            painter.restore();
        }

        if (liveStrokeOnPage) {
            if (const Stroke* pending = controller_.PendingStroke(); pending != nullptr) {
                painter.save();
                painter.translate(view.rect.topLeft());
                if (SyncStrokeLayer(*pending, liveStroke_, view)) {
                    // Stabilized ink is composited in the layer raster; only
                    // the provisional tail (two segments at most) is drawn
                    // live, so a paint no longer costs O(stroke points).
                    // The image is untagged device pixels, so the blit passes
                    // an explicit logical-size rect (device px / dpr) at
                    // pageRect's device-snapped origin — the point overload
                    // would stretch the raster dpr-fold.
                    const float dpr = strokeLayer_.devicePixelRatio > 0.0f
                        ? strokeLayer_.devicePixelRatio : 1.0f;
                    painter.drawImage(QRectF(
                            static_cast<qreal>(strokeLayer_.pageRect.x() * view.scale),
                            static_cast<qreal>(strokeLayer_.pageRect.y() * view.scale),
                            static_cast<qreal>(strokeLayer_.image.width() / dpr),
                            static_cast<qreal>(strokeLayer_.image.height() / dpr)),
                        strokeLayer_.image);
                    PaintStrokeTail(painter, *pending, liveStroke_, view);
                } else {
                    render::PaintStrokePreview(painter, *pending, RenderParamsFor(view), 1.0f);
                }
                painter.restore();
            }
        }

        // Live LaTeX preview while the source box is open.
        if (latexEditor_ != nullptr && latexEditor_->isVisible()
            && editOverlay_.has_value() && editOverlay_->page == view.pageIndex
            && editOverlay_->kind == EditOverlayKey::Kind::Latex) {
            if (const PageLatex* object = controller_.EditingLatexObject(); object != nullptr) {
                const PageLatex preview = LiveLatexPreview(*object);
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
    peakPaintSurfaceBytes_ = std::max(peakPaintSurfaceBytes_, lastPaintSurfaceBytes_);
    // The accumulated transient region was just consumed by this paint; the
    // next gesture samples start a fresh region.
    transientDirty_ = QRect();
    TrimSurfaceCache(visiblePages);
    for (const QRect& deferred : deferredSurfaceRects) {
        // Spread zoom-settle (and above-budget tile) replacements across
        // frames: repaint only the pages that still owe a raster.
        update(deferred);
    }
    // The erase drag's fading trail floats above the pages like the
    // selection chrome (Excalidraw: rgba(0, 0, 0, 0.2) on the light theme).
    if (const std::int64_t now = eraserTrailClock_.elapsed(); eraserTrail_.Visible(now)) {
        InkOutline outline;
        eraserTrail_.AppendOutline(now, kEraserTrailTolerance, outline);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.fillPath(render::InkOutlinePath(outline), QColor(0, 0, 0, kEraserTrailAlpha));
        painter.restore();
    }
    PaintTextCreatePreview(painter);
    PaintSelectionOverlay(painter);
    PositionOverlays();
}

void CanvasWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // The fit scale depends on the canvas width, so the page layout changes with
    // the widget; the reference frontend re-derives it on every paint as well.
    NotifyViewportChanged();
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
    // The controller owns edit state; the cached key is only a placement hint,
    // so it must never decide whether an edit is live.
    return controller_.EditingTextIndex().has_value() || controller_.EditingLatexIndex().has_value();
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
                SyncEditOverlay();
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
                SyncEditOverlay();
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
        const EditOverlayKey key {
            EditOverlayKey::Kind::Text,
            controller_.EditingPageIndex(),
            *controller_.EditingTextIndex(),
        };
        if (editOverlay_.has_value() && *editOverlay_ == key
            && textEditor_ != nullptr && textEditor_->isVisible()) {
            return;
        }
        editOverlay_ = key;
        ShowTextOverlay(*controller_.EditingTextObject());
        return;
    }
    if (controller_.EditingLatexIndex().has_value() && controller_.EditingLatexObject() != nullptr) {
        const EditOverlayKey key {
            EditOverlayKey::Kind::Latex,
            controller_.EditingPageIndex(),
            *controller_.EditingLatexIndex(),
        };
        if (editOverlay_.has_value() && *editOverlay_ == key
            && latexEditor_ != nullptr && latexEditor_->isVisible()) {
            return;
        }
        editOverlay_ = key;
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
        edited = CaptureEditedText();
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

void CanvasWidget::CommitActiveEditFromKeyboard() {
    // Excalidraw selects a text submitted from the keyboard (Escape,
    // Ctrl+Enter), so it can be moved, resized or deleted next.  The object
    // is found again by id: the commit may reorder or (emptied) remove it.
    const std::size_t pageIndex = controller_.EditingPageIndex();
    std::optional<std::uint64_t> textId;
    std::optional<std::uint64_t> latexId;
    if (const PageText* text = controller_.EditingTextObject(); text != nullptr && controller_.EditingTextIndex().has_value()) {
        textId = text->id;
    } else if (const PageLatex* latex = controller_.EditingLatexObject();
               latex != nullptr && controller_.EditingLatexIndex().has_value()) {
        latexId = latex->id;
    }
    CommitActiveEdit();

    const Document& document = controller_.DocumentRef();
    if (pageIndex >= document.PageCount() || (!textId.has_value() && !latexId.has_value())) {
        return;
    }
    const Page& page = document.PageAt(pageIndex);
    PageSelection selection;
    for (std::size_t index = 0; textId.has_value() && index < page.texts.size(); ++index) {
        if (page.texts[index].id == *textId) {
            selection.texts.push_back(index);
        }
    }
    for (std::size_t index = 0; latexId.has_value() && index < page.latexes.size(); ++index) {
        if (page.latexes[index].id == *latexId) {
            selection.latexes.push_back(index);
        }
    }
    if (!selection.Empty()) {
        controller_.SelectObjects(pageIndex, std::move(selection));
        update();
        emit KeyboardCommitSelected();
    }
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
    textEditor_ = new InlineMathTextEdit(this);
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
    connect(textEditor_, &QTextEdit::textChanged, this, [this]() { FitTextEditor(); });
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
    const CanvasPageView view = editOverlay_.has_value()
        ? CalculatePageView(editOverlay_->page)
        : CanvasPageView {};
    textEditScale_ = view.valid ? view.scale : 1.0f;
    textEditor_->document()->setDocumentMargin(0.0);
    ApplyTextEditorWrap(text);
    {
        // Typing continues the style of the text before the caret (the end of
        // the object), like any editor; an empty object types in its own
        // style through the block char format LoadPageText applies.
        QSignalBlocker blocker(textEditor_);
        textEditor_->LoadPageText(text, textEditScale_);
    }
    textEditor_->show();
    textEditor_->raise();
    textEditor_->setFocus(Qt::OtherFocusReason);
    PositionOverlays();
    update();
}

void CanvasWidget::ApplyTextEditorWrap(const PageText& text) {
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
    // FixedPixelWidth wraps at lineWrapColumnOrWidth, which defaults to 0 and
    // stays 0 without this call - that wrapped every character onto its own
    // line the moment a resized (fixed-width) text object reopened. The wrap
    // width is the object's page width at the view scale, same as the
    // committed layout; PositionOverlays keeps it in sync on zoom.
    if (text.width > 0.0f) {
        textEditor_->setLineWrapColumnOrWidth(text::TextWrapWidthPx(text, textEditScale_));
    } else {
        // A non-wrapping QTextEdit aligns within its viewport only after one
        // of its own relayouts (a resize or a wrap-mode change) has seen
        // aligned content; an alignment change is neither, so give the
        // document the width it aligns in now.
        textEditor_->document()->setPageSize(QSizeF(textEditor_->viewport()->width(), -1.0));
    }
}

void CanvasWidget::FitTextEditor() {
    const PageText* object = controller_.EditingTextObject();
    if (textEditor_ == nullptr || object == nullptr || !editOverlay_.has_value()) {
        return;
    }
    const float widthBefore = object->width;
    controller_.FitEditingText(textEditor_->CapturePageText(), MaxAutoTextWidth(editOverlay_->page));
    // Excalidraw's autoResize flip: the text stopped growing and wraps from
    // here on, in the editor as on the page, and is scrolled into view.
    if (const PageText* fitted = controller_.EditingTextObject(); fitted != nullptr && fitted->width != widthBefore) {
        ApplyTextEditorWrap(*fitted);
        RevealEditedText(*fitted);
    }
    PositionOverlays();
}

void CanvasWidget::RevealEditedText(const PageText& text) {
    // scrollBoundsIntoView(tooLarge: "leave"): pans the box between the
    // padded canvas edges, on each axis only if it fits there.
    const CanvasPageView view = CalculatePageView(editOverlay_->page);
    if (!view.valid) {
        return;
    }
    const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, 0.0f);
    const qreal scale = static_cast<qreal>(view.scale);
    const QRectF box(
        view.rect.left() + (static_cast<qreal>(bounds.left) * scale),
        view.rect.top() + (static_cast<qreal>(bounds.top) * scale),
        static_cast<qreal>(bounds.right - bounds.left) * scale,
        static_cast<qreal>(bounds.bottom - bounds.top) * scale);
    const qreal padding = static_cast<qreal>(text::kTextViewportPaddingPx);
    const QRectF room = VisibleCanvasRect().adjusted(padding, padding, -padding, -padding);
    // A text that just started wrapping is exactly as wide as the room (both
    // come from the same view width), give or take float rounding.
    const auto shift = [](qreal low, qreal high, qreal roomLow, qreal roomHigh) -> qreal {
        if (high - low > roomHigh - roomLow + 1.0) {
            return 0.0;
        }
        if (low < roomLow) {
            return roomLow - low;
        }
        return high > roomHigh ? roomHigh - high : 0.0;
    };
    const QPointF delta(
        shift(box.left(), box.right(), room.left(), room.right()),
        shift(box.top(), box.bottom(), room.top(), room.bottom()));
    if (!delta.isNull()) {
        PanBy(delta);
    }
}

float CanvasWidget::MaxAutoTextWidth(std::size_t pageIndex) const {
    const CanvasPageView view = CalculatePageView(pageIndex);
    return text::MaxAutoTextWidth(static_cast<float>(VisibleCanvasRect().width()), view.valid ? view.scale : 1.0f);
}

void CanvasWidget::SetTextEditAlignment(TextAlignment alignment) {
    controller_.SetEditingTextAlignment(alignment);
    const PageText* object = controller_.EditingTextObject();
    if (textEditor_ == nullptr || object == nullptr) {
        return;
    }
    // The editor aligns every paragraph through its document's default
    // option, the same one the committed layout uses.
    ApplyTextEditorWrap(*object);
    PositionOverlays();
    update();
}

void CanvasWidget::ShowLatexOverlay(const PageLatex& object) {
    EnsureLatexEditor();
    latexPreviewMetrics_ = latex::Measure(latex::Parse(object));
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
    editOverlay_.reset();
    textEditScale_ = 1.0f;
    latexPreviewMetrics_ = latex::FormulaMetrics {};
    latexSuggestions_.clear();
    if (latexPopupModel_ != nullptr) {
        latexPopupModel_->setStringList({});
    }
    if (latexPopup_ != nullptr) {
        latexPopup_->setCurrentIndex(QModelIndex());
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
    if (!editOverlay_.has_value()) {
        return;
    }
    const Document& document = controller_.DocumentRef();
    if (editOverlay_->page >= document.PageCount()) {
        return;
    }
    const CanvasPageView view = CalculatePageView(editOverlay_->page);
    if (!view.valid || view.scale <= 0.0f) {
        return;
    }

    if (textEditor_ != nullptr && textEditor_->isVisible() && controller_.EditingTextObject() != nullptr) {
        const PageText& object = *controller_.EditingTextObject();
        // The editor is the live preview: fonts are formatted at the view
        // scale, so a zoom change mid-edit re-formats at the new scale (the
        // editor round-trips content, caret and undo history).
        if (std::abs(view.scale - textEditScale_) > 0.0001f) {
            textEditScale_ = view.scale;
            QSignalBlocker blocker(textEditor_);
            textEditor_->SetDisplayScale(view.scale);
        }
        // The editor sits on the box the committed text lays out in.  A
        // growing text's editor does not wrap, and QTextEdit then aligns its
        // lines within the whole viewport: the caret room is added on the side
        // the alignment leaves free, so the lines land where the committed
        // render puts them.  A wrapping editor aligns within its wrap width.
        // Trailing blanks count in the editor but not in the box; the editor
        // still makes room for them so it never scrolls its own content.
        constexpr qreal kCaretRoom = 4.0;
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(object, 0.0f);
        const bool grows = object.width <= 0.0f;
        const qreal alignmentShare = !grows ? 0.0
            : (object.alignment == TextAlignment::Center ? 0.5 : (object.alignment == TextAlignment::Right ? 1.0 : 0.0));
        const qreal boxWidth = static_cast<qreal>(bounds.right - bounds.left) * view.scale;
        const qreal left = view.rect.left() + (static_cast<qreal>(object.x) * view.scale) - (kCaretRoom * alignmentShare);
        const qreal top = view.rect.top() + (static_cast<qreal>(object.y) * view.scale);
        const qreal width = (grows ? std::max(boxWidth, textEditor_->document()->idealWidth()) : boxWidth) + kCaretRoom;
        const qreal height = std::max<qreal>(8.0, textEditor_->document()->size().height() + 4.0);
        const QRect geometry(QPoint(static_cast<int>(std::lround(left)), static_cast<int>(std::lround(top))),
                             QSize(static_cast<int>(std::lround(width)), static_cast<int>(std::lround(height))));
        if (textEditor_->geometry() != geometry) {
            textEditor_->setGeometry(geometry);
        }
        // Committed text is clipped to the page rect; mask the editor the same
        // way so the live preview cannot draw past the page edge.
        const QRect clip = geometry.intersected(view.rect.toAlignedRect()).translated(-geometry.topLeft());
        if (object.width > 0.0f) {
            const int wrapWidth = text::TextWrapWidthPx(object, view.scale);
            if (textEditor_->lineWrapColumnOrWidth() != wrapWidth) {
                textEditor_->setLineWrapColumnOrWidth(wrapWidth);
            }
        }
        textEditor_->setMask(QRegion(clip));
    }
    if (latexEditor_ != nullptr && latexEditor_->isVisible() && controller_.EditingLatexObject() != nullptr) {
        // Measure what the preview draws (the typed source), not the object
        // as it was when the edit began, or a growing formula slides under
        // the box.
        const PageLatex object = LiveLatexPreview(*controller_.EditingLatexObject());
        if (const latex::FormulaHandle formula = latex::Parse(object); formula != nullptr) {
            latexPreviewMetrics_ = latex::Measure(formula);
        }
        const latex::FormulaMetrics& metrics = latexPreviewMetrics_;
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

PageLatex CanvasWidget::LiveLatexPreview(const PageLatex& object) const {
    PageLatex preview = object;
    if (latexEditor_ != nullptr) {
        preview.content = ToUtf16(latexEditor_->toPlainText());
    }
    return preview;
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
        const PageText captured = textEditor_->CapturePageText();
        edited.content = captured.content;
        edited.styleSpans = captured.styleSpans;
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

    // Ctrl+Enter and Escape both finish the edit and keep what was typed
    // (Excalidraw's textWysiwyg submits on Escape); an emptied text is
    // removed by the commit itself.
    const bool commitChord = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && event->modifiers().testFlag(Qt::ControlModifier);
    if (commitChord || event->key() == Qt::Key_Escape) {
        CommitActiveEditFromKeyboard();
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
        // Nothing before the caret to delete: swallow the chord.  Passing it
        // on ran QTextEdit's own word deletion, which on an emptied editor
        // reset the typing format to an unstyled one (invisible new text).
        if (!cursor.hasSelection()) {
            return true;
        }
    }
    return false;
}

bool CanvasWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == textEditor_ || watched == latexEditor_) {
        // Ctrl+B inside an editor never reaches the window (its sidebar
        // binding): claiming the ShortcutOverride keeps the key with the
        // editor.  The text editor's press is its bold command; the LaTeX
        // source box gets the press itself (QPlainTextEdit leaves it inert),
        // like the reference's edit control.
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_B && key->modifiers() == Qt::ControlModifier) {
                if (event->type() == QEvent::ShortcutOverride) {
                    event->accept();
                    return true;
                }
                if (watched == textEditor_) {
                    emit TextBoldShortcutPressed();
                    return true;
                }
                return false;
            }
        }
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
    // One cursor source: the live interaction/tool plus the PDF-text hover at
    // the position just sampled (MessageLoop.cpp:665-680).
    RefreshCursor();
    event->accept();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    lastPointerPosition_ = event->position();
    HandlePointerRelease(SampleFromEvent(*event));
    event->accept();
}

void CanvasWidget::tabletEvent(QTabletEvent* event) {
    PointerSample sample;
    sample.position = event->position();
    sample.button = event->button();
    sample.buttons = event->buttons();
    sample.modifiers = event->modifiers();
    sample.timestamp = static_cast<quint64>(event->timestamp());
    sample.pressure = static_cast<float>(event->pressure());
    switch (event->type()) {
    case QEvent::TabletPress:
        setFocus(Qt::MouseFocusReason);
        lastPointerPosition_ = event->position();
        HandlePointerPress(sample);
        break;
    case QEvent::TabletMove:
        lastPointerPosition_ = event->position();
        HandlePointerMove(sample);
        RefreshCursor();
        break;
    case QEvent::TabletRelease:
        lastPointerPosition_ = event->position();
        HandlePointerRelease(sample);
        break;
    default:
        // Proximity and other tablet events fall through to Qt's defaults.
        event->ignore();
        return;
    }
    event->accept();
}

void CanvasWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        CommitActiveEditFromKeyboard();
        HandlePointerCancel(CancelReason::Escape);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete) {
        // Deleting the selection is a document mutation, so it goes through the
        // controller like the cut path (QtMainWindow::CutSelection).  A
        // pdf-text-only clear emits SelectionChanged, not SessionChanged, so
        // the repaint cannot rely on the session broadcast.
        if (controller_.RemoveSelection()) {
            update();
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
    TrackEraserTrail(sample.position);
    TrackShapeHold(sample.position);
    // A handle press turns the hover cursor into the drag's (the rotate hand
    // closes).
    RefreshCursor();
    update();
}

void CanvasWidget::HandlePointerMove(const PointerSample& sample) {
    router_->HandlePointerMove(sample);
    TrackEraserTrail(sample.position);
    TrackShapeHold(sample.position);
}

void CanvasWidget::HandlePointerRelease(const PointerSample& sample) {
    router_->HandlePointerRelease(sample);
    TrackEraserTrail(sample.position);
    TrackShapeHold(std::nullopt);
    EndSnapGhost(true);
    // The drag's cursor gives way to whatever the pointer now hovers.
    RefreshCursor();
    update();
}

void CanvasWidget::HandlePointerCancel(CancelReason reason) {
    // Idempotent: the router abandons the live *pointer* interaction without
    // committing it (Escape / lost grab) and releases capture.  Active
    // text/LaTeX edits are not cancelled here: they own their own focus, and a
    // canvas focus change must not discard typed content (Escape commits).
    static_cast<void>(reason);
    router_->CancelInteraction();
    TrackEraserTrail(std::nullopt);
    TrackShapeHold(std::nullopt);
    EndSnapGhost(false);
    RefreshCursor();
    update();
}

void CanvasWidget::TrackShapeHold(std::optional<QPointF> position) {
    if (!position.has_value() || router_->ActiveInteraction() != InteractionKind::Stroke) {
        shapeHoldTimer_->stop();
        return;
    }
    const QPointF drift = *position - shapeHoldAnchor_;
    if (!shapeHoldTimer_->isActive() || std::hypot(drift.x(), drift.y()) > kShapeHoldTolerancePixels) {
        shapeHoldAnchor_ = *position;
        shapeHoldTimer_->start();
    }
}

void CanvasWidget::SnapHeldStroke() {
    const Stroke* pending = controller_.PendingStroke();
    if (pending == nullptr) {
        return;
    }
    SnapGhost ghost { controller_.PendingStrokePage(), *pending, std::nullopt };
    if (!router_->SnapStrokeToShape()) {
        return;
    }
    // A previous ghost still fading gives way to the new one.
    RepaintSnapGhost();
    snapGhostTimer_->stop();
    snapGhost_ = std::move(ghost);
    RepaintSnapGhost();
}

float CanvasWidget::SnapGhostOpacity() const {
    if (!snapGhost_.has_value()) {
        return 0.0f;
    }
    if (!snapGhost_->releasedAt.has_value()) {
        return kSnapGhostOpacity;
    }
    const auto faded = static_cast<float>(snapGhostClock_.elapsed() - *snapGhost_->releasedAt);
    return kSnapGhostOpacity * std::max(0.0f, 1.0f - (faded / static_cast<float>(kSnapGhostFadeMs)));
}

void CanvasWidget::RepaintSnapGhost() {
    if (!snapGhost_.has_value()) {
        return;
    }
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    if (!StrokeBounds(snapGhost_->stroke, &minX, &minY, &maxX, &maxY)) {
        return;
    }
    const float margin = snapGhost_->stroke.style.width + kStrokeDirtyMarginPixels;
    update(PageRegionDirtyRect(snapGhost_->pageIndex, QRectF(
        static_cast<qreal>(minX - margin), static_cast<qreal>(minY - margin),
        static_cast<qreal>((maxX - minX) + margin * 2.0f), static_cast<qreal>((maxY - minY) + margin * 2.0f))));
}

void CanvasWidget::EndSnapGhost(bool fade) {
    if (!snapGhost_.has_value() || snapGhost_->releasedAt.has_value()) {
        return;
    }
    RepaintSnapGhost();
    if (!fade) {
        snapGhost_.reset();
        return;
    }
    snapGhost_->releasedAt = snapGhostClock_.elapsed();
    snapGhostTimer_->start();
}

void CanvasWidget::RefreshSnapGhost() {
    RepaintSnapGhost();
    if (SnapGhostOpacity() <= 0.0f) {
        snapGhost_.reset();
        snapGhostTimer_->stop();
    }
}

void CanvasWidget::TrackEraserTrail(std::optional<QPointF> position) {
    const bool erasing = router_->ActiveInteraction() == InteractionKind::Erase && position.has_value();
    if (!erasing && !eraserTrail_.Active()) {
        return;
    }
    if (erasing) {
        const std::int64_t now = eraserTrailClock_.elapsed();
        const Vec2 point { static_cast<float>(position->x()), static_cast<float>(position->y()) };
        if (eraserTrail_.Active()) {
            eraserTrail_.Add(point, now);
        } else {
            // The swept width is the eraser's own hit radius (EraseTargetsAt).
            eraserTrail_.Begin(point, now, std::max(kEraserMinRadiusPixels, eraseBrushSize_ * 4.0f));
        }
    } else {
        eraserTrail_.End();
    }
    RefreshEraserTrail();
}

void CanvasWidget::RefreshEraserTrail() {
    const std::int64_t now = eraserTrailClock_.elapsed();
    QRect next;
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    if (eraserTrail_.Bounds(now, &left, &top, &right, &bottom)) {
        next = QRectF(QPointF(left, top), QPointF(right, bottom)).toAlignedRect().adjusted(-2, -2, 2, 2);
    }
    // QRect::united treats an empty rect as a point at the origin.
    const QRect dirty = eraserTrailRect_.isEmpty() ? next : (next.isEmpty() ? eraserTrailRect_ : eraserTrailRect_.united(next));
    if (!dirty.isEmpty()) {
        update(dirty);
    }
    eraserTrailRect_ = next;
    if (!eraserTrail_.Visible(now)) {
        eraserTrailTimer_->stop();
    } else if (!eraserTrailTimer_->isActive()) {
        eraserTrailTimer_->start();
    }
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

Vec2 CanvasWidget::ScreenToPageOnPage(std::size_t pageIndex, const QPointF& logical, bool clampToPage) const {
    const CanvasPageView view = CalculatePageView(pageIndex);
    if (!view.valid) {
        return {};
    }
    return ScreenToPage(view, logical, clampToPage);
}

bool CanvasWidget::IsPointInCanvas(const QPointF& logical) const {
    return VisibleCanvasRect().contains(logical);
}

bool CanvasWidget::HasSelection() const {
    return controller_.HasSelection();
}

bool CanvasWidget::CursorInteractsWithSelection(const QPointF& logical) const {
    // The handles sit outside the frame, possibly off the page, and a
    // Select-tool press there must reach the transform probe instead of
    // clearing the selection; the router only calls BeginMoveSelection when
    // this returns true.
    if (HitTestSelectionHandle(logical).has_value()) {
        return true;
    }
    const std::optional<CanvasPageView> view = HitTestPageView(logical);
    if (!view.has_value()) {
        return false;
    }
    const std::optional<SelectionBounds> bounds = SelectionBoundsForPage(view->pageIndex);
    if (!bounds.has_value()) {
        return false;
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
        const PageText& text = page.texts[index];
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page.width);
        if (PointInRotatedRect(pagePoint, bounds.left, bounds.top, bounds.right, bounds.bottom, text.rotationDegrees)) {
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
    // App::CursorHitsPdfText (Selection.cpp:680-710): the page under the cursor
    // counts only when one of its PDF character boxes contains the point.
    const std::optional<CanvasPageView> view = HitTestPageView(logical);
    if (!view.has_value() || !view->valid) {
        return std::nullopt;
    }
    const std::vector<PdfTextCharBox>* boxes = controller_.PdfTextCharBoxesFor(view->pageIndex);
    if (boxes == nullptr) {
        return std::nullopt;
    }
    const Vec2 pagePoint = ScreenToPage(*view, logical, false);
    for (const PdfTextCharBox& box : *boxes) {
        if (pagePoint.x >= box.left
            && pagePoint.x <= box.right
            && pagePoint.y >= box.top
            && pagePoint.y <= box.bottom) {
            return view->pageIndex;
        }
    }
    return std::nullopt;
}

namespace {

// Selection.cpp:1399-1424 nearestBoxIndex: the box containing the point, or the
// one whose centre is closest. Assumes `boxes` is non-empty.
std::size_t NearestPdfTextIndex(const std::vector<PdfTextCharBox>& boxes, Vec2 point) {
    std::size_t nearest = 0;
    float nearestDistance = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < boxes.size(); ++index) {
        const PdfTextCharBox& box = boxes[index];
        if (point.x >= box.left && point.x <= box.right
            && point.y >= box.top && point.y <= box.bottom) {
            return index;
        }
        const float cx = (box.left + box.right) * 0.5f;
        const float cy = (box.top + box.bottom) * 0.5f;
        const float dx = cx - point.x;
        const float dy = cy - point.y;
        const float distance = (dx * dx) + (dy * dy);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = index;
        }
    }
    return nearest;
}

}  // namespace

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
    // Every visible page's selection, not only the page under the pointer:
    // the handles hang outside the selection bounds, so they can lie off the
    // page when the selection runs along a page edge.
    for (const CanvasPageView& view : CalculatePageViews()) {
        if (!view.valid) {
            continue;
        }
        const std::optional<SelectionBounds> bounds = SelectionBoundsForPage(view.pageIndex);
        if (!bounds.has_value()) {
            continue;
        }
        if (const std::optional<TransformHandle> handle = SelectionHandleAt(SelectionFrameRect(view, *bounds), logical);
            handle.has_value()) {
            // The transform spans the whole selection on the page, so `target`
            // and `objectIndex` keep their Stroke/0 defaults and are ignored.
            TransformRequest request;
            request.pageIndex = view.pageIndex;
            request.handle = *handle;
            return request;
        }
    }
    return std::nullopt;
}

std::optional<TransformHandle> CanvasWidget::SelectionHandleAt(const QRectF& frame, const QPointF& logical) {
    const qreal hitRadius = static_cast<qreal>(SelectionHandleHitRadiusPixels(96));
    const qreal hitRadiusSquared = hitRadius * hitRadius;
    const auto hits = [&logical, hitRadiusSquared](const QPointF& position) {
        const qreal dx = logical.x() - position.x();
        const qreal dy = logical.y() - position.y();
        return ((dx * dx) + (dy * dy)) <= hitRadiusSquared;
    };

    // On a short selection the rotate handle's radius overlaps the top
    // corners' and the top edge, so it is checked first and wins a tie.
    if (hits(SelectionRotateHandlePosition(frame))) {
        return TransformHandle::Rotate;
    }

    const std::array<QPointF, 4> corners = SelectionHandlePositions(frame);
    constexpr std::array<TransformHandle, 4> kCornerHandles {{
        TransformHandle::TopLeft,
        TransformHandle::TopRight,
        TransformHandle::BottomRight,
        TransformHandle::BottomLeft,
    }};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        if (hits(corners[index])) {
            return kCornerHandles[index];
        }
    }

    // Side resizing grips the frame edges themselves (no visible handle): a
    // band from just outside the frame in to the selection bounds, so a press
    // on an object lying along the bounds still reaches the object.
    const qreal reach = kSelectionEdgeGrip;
    const qreal inset = kSelectionFramePadding;
    const bool alongWidth = logical.x() >= frame.left() && logical.x() <= frame.right();
    const bool alongHeight = logical.y() >= frame.top() && logical.y() <= frame.bottom();
    if (alongWidth && logical.y() >= frame.top() - reach && logical.y() < frame.top() + inset) {
        return TransformHandle::Top;
    }
    if (alongWidth && logical.y() <= frame.bottom() + reach && logical.y() > frame.bottom() - inset) {
        return TransformHandle::Bottom;
    }
    if (alongHeight && logical.x() >= frame.left() - reach && logical.x() < frame.left() + inset) {
        return TransformHandle::Left;
    }
    if (alongHeight && logical.x() <= frame.right() + reach && logical.x() > frame.right() - inset) {
        return TransformHandle::Right;
    }
    return std::nullopt;
}

std::vector<ObjectRef> CanvasWidget::EraseTargetsAt(std::size_t pageIndex, Vec2 start, Vec2 end) const {
    std::vector<ObjectRef> targets;
    const CanvasPageView view = CalculatePageView(pageIndex);
    if (!view.valid) {
        return targets;
    }
    const Document& document = controller_.DocumentRef();
    const Page& page = document.PageAt(pageIndex);

    // Reference App::EraseAt (App.cpp:5713-5799): the eraser radius is
    // max(3.5, eraseBrushSize * 4) logical pixels (App::EraserRadiusPixels,
    // App.cpp:7426-7428) scaled into page units, swept along the segment from
    // the previous sample to this one.
    const float radiusPage = std::max(kEraserMinRadiusPixels, eraseBrushSize_ * 4.0f) / std::max(0.1f, view.scale);

    for (std::size_t index = 0; index < page.images.size(); ++index) {
        if (ImageHitsSegment(page.images[index], start, end, radiusPage)) {
            targets.push_back(ObjectRef { pageIndex, ObjectKind::Image, index });
        }
    }
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        const PageText& text = page.texts[index];
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page.width);
        if (RotatedRectHitsSegment(bounds.left, bounds.top, bounds.right, bounds.bottom,
                text.rotationDegrees, start, end, radiusPage)) {
            targets.push_back(ObjectRef { pageIndex, ObjectKind::Text, index });
        }
    }
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        const PageLatex& latex = page.latexes[index];
        const LatexBounds bounds = MeasurePageLatexBounds(latex, page.width);
        if (RotatedRectHitsSegment(bounds.left, bounds.top, bounds.right, bounds.bottom,
                latex.rotationDegrees, start, end, radiusPage)) {
            targets.push_back(ObjectRef { pageIndex, ObjectKind::Latex, index });
        }
    }
    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        const Stroke& stroke = page.strokes[index];
        // Cheap reject before the smoothing pass: the smoothed curve stays
        // within the raw points' bounds inflated by the pen half-width plus
        // a slack for the smoothing deviation (a quadratic midpoint chain
        // never leaves the convex hull of its raw neighbours).
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        if (StrokeBounds(stroke, &minX, &minY, &maxX, &maxY)) {
            const float margin = stroke.style.width * 0.5f + 8.0f;
            if (!SegmentHitsRect(minX - margin, minY - margin, maxX + margin, maxY + margin,
                    start, end, radiusPage)) {
                continue;
            }
        }
        if (StrokeHitsSegment(stroke, start, end, radiusPage, controller_.InputSettings().strokeSmoothing)) {
            targets.push_back(ObjectRef { pageIndex, ObjectKind::Stroke, index });
        }
    }
    return targets;
}

std::vector<ObjectRef> CanvasWidget::ScratchOutTargets(std::size_t pageIndex, const ScratchOut& scratch) const {
    std::vector<ObjectRef> targets;
    const Stroke* pending = controller_.PendingStroke();
    if (scratch.hull.empty() || pending == nullptr) {
        return targets;
    }
    const Document& document = controller_.DocumentRef();
    const Page& page = document.PageAt(pageIndex);
    // The scratch's ink reaches half its width past the points it was drawn with.
    const float margin = pending->style.width * 0.5f;

    for (std::size_t index = 0; index < page.images.size(); ++index) {
        const PageImage& image = page.images[index];
        if (scratch.CoversArea(RotatedRectCorners(image.x, image.y, image.x + image.width, image.y + image.height,
                image.rotationDegrees), margin)) {
            targets.push_back(ObjectRef { pageIndex, ObjectKind::Image, index });
        }
    }
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        const PageText& text = page.texts[index];
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page.width);
        if (scratch.CoversArea(RotatedRectCorners(bounds.left, bounds.top, bounds.right, bounds.bottom,
                text.rotationDegrees), margin)) {
            targets.push_back(ObjectRef { pageIndex, ObjectKind::Text, index });
        }
    }
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        const PageLatex& latex = page.latexes[index];
        const LatexBounds bounds = MeasurePageLatexBounds(latex, page.width);
        if (scratch.CoversArea(RotatedRectCorners(bounds.left, bounds.top, bounds.right, bounds.bottom,
                latex.rotationDegrees), margin)) {
            targets.push_back(ObjectRef { pageIndex, ObjectKind::Latex, index });
        }
    }

    float hullLeft = scratch.hull.front().x, hullTop = scratch.hull.front().y;
    float hullRight = hullLeft, hullBottom = hullTop;
    for (const Vec2 point : scratch.hull) {
        hullLeft = std::min(hullLeft, point.x);
        hullTop = std::min(hullTop, point.y);
        hullRight = std::max(hullRight, point.x);
        hullBottom = std::max(hullBottom, point.y);
    }
    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        const Stroke& stroke = page.strokes[index];
        // Cheap reject before the smoothing pass, as in EraseTargetsAt: a
        // stroke whose bounds miss the scratch's cannot be half under it.
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        if (!StrokeBounds(stroke, &minX, &minY, &maxX, &maxY) || maxX < hullLeft - margin
            || minX > hullRight + margin || maxY < hullTop - margin || minY > hullBottom + margin) {
            continue;
        }
        if (scratch.CoversPath(BuildStrokeRenderPath(stroke, controller_.InputSettings().strokeSmoothing).points, margin)) {
            targets.push_back(ObjectRef { pageIndex, ObjectKind::Stroke, index });
        }
    }
    return targets;
}

void CanvasWidget::ZoomByWheelSteps(int steps, const QPointF& anchor) {
    ZoomBySteps(steps, anchor);
}

void CanvasWidget::ScrollBy(const QPointF& logicalDelta) {
    // Wheel notches animate through the accumulating chase (StartSmoothScroll).
    StartSmoothScroll(logicalDelta);
}

void CanvasWidget::SetCursor(CursorKind cursor) {
    cursorKind_ = cursor;
    ApplyCursor();
}

void CanvasWidget::SetTool(Tool tool) {
    tool_ = tool;
    router_->SetTool(tool);
    RefreshCursor();
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

void CanvasWidget::SetTransparentBackdrop(bool transparent) {
    if (transparent == transparentBackdrop_) {
        return;
    }
    transparentBackdrop_ = transparent;
    // A clear backdrop no longer covers the widget.
    setAttribute(Qt::WA_OpaquePaintEvent, !transparent);
    update();
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
    // A white-filled disc with an inset black kEraserOutlinePixels outline and
    // a centred hotspot, the diameter of the erase hit test (EraseTargetsAt
    // sweeps a disc) and of the eraser trail's head: max(3.5, eraseBrushSize
    // * 4) * 2 logical pixels (App::EraserDiameterPixels, App.cpp:7430-7432).
    // The reference drew a square (CreateSquareBrushCursor, App.cpp:579-691),
    // whose corners promised erasing the disc never does.
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
        painter.drawEllipse(outer.adjusted(penWidth * 0.5, penWidth * 0.5, -penWidth * 0.5, -penWidth * 0.5));
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
    case CursorKind::ResizeVertical:
        setCursor(Qt::SizeVerCursor);
        return;
    case CursorKind::ResizeHorizontal:
        setCursor(Qt::SizeHorCursor);
        return;
    case CursorKind::ResizeDiagonal:
        setCursor(Qt::SizeFDiagCursor);
        return;
    case CursorKind::ResizeAntiDiagonal:
        setCursor(Qt::SizeBDiagCursor);
        return;
    case CursorKind::Eraser: {
        // The erase tool shows the brush-sized white disc rather than a
        // system cursor, so the user can see what the erase radius will hit
        // (App::EraserCursorForCurrentSize, App.cpp:7459-7468, drew a square).
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

void CanvasWidget::RefreshCursor() {
    const InteractionKind interaction = ActiveInteraction();
    const bool selectTool = tool_ == Tool::Move || tool_ == Tool::Select;
    if (interaction == InteractionKind::Pan) {
        // An in-flight pan owns the cursor even if the pointer left the page.
        cursorKind_ = CursorKind::ClosedHand;
    } else if (interaction == InteractionKind::Transform && transformSession_.has_value()) {
        // A handle drag keeps its handle's cursor wherever the pointer goes.
        cursorKind_ = InteractionRouter::CursorForHandle(transformSession_->handle, true);
    } else if (interaction != InteractionKind::None || !selectTool) {
        // Any other live drag keeps its tool's cursor.
        cursorKind_ = InteractionRouter::CursorForTool(tool_);
    } else if (tool_ == Tool::Move && PageWithPdfTextAt(lastPointerPosition_).has_value()) {
        // MessageLoop.cpp:665-680: an I-beam over PDF text.  A Move-tool press
        // there selects text before it probes the selection handles.
        cursorKind_ = CursorKind::Text;
    } else if (const std::optional<TransformRequest> handle = HitTestSelectionHandle(lastPointerPosition_);
               handle.has_value()) {
        cursorKind_ = InteractionRouter::CursorForHandle(handle->handle, false);
    } else if (PageWithPdfTextAt(lastPointerPosition_).has_value()) {
        cursorKind_ = CursorKind::Text;
    } else {
        cursorKind_ = InteractionRouter::CursorForTool(tool_);
    }
    ApplyCursor();
}

void CanvasWidget::enterEvent(QEnterEvent* event) {
    RefreshCursor();
    QWidget::enterEvent(event);
}

void CanvasWidget::leaveEvent(QEvent* event) {
    // The pointer is gone, so the PDF-text hover cannot apply until the next
    // move re-samples it.
    lastPointerPosition_ = QPointF(-1.0e9, -1.0e9);
    RefreshCursor();
    QWidget::leaveEvent(event);
}

void CanvasWidget::InvalidateCanvas() {
    // Repaint request, not a cache drop. The page rasters are keyed and
    // signature-checked by the renderer, so a content change re-renders on its
    // own; the interaction router calls this for every pointer sample, and
    // clearing the rasters here re-rendered a dense page (~1 s) per sample.
    // The transient state only ever dirties the regions it reported through
    // MarkTransientDirty; repaint that accumulated region instead of the
    // whole widget.
    update(transientDirty_.isEmpty() ? rect() : transientDirty_);
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

void CanvasWidget::BeginStroke(std::size_t pageIndex, Vec2 pagePoint, bool dotted, StrokeSampling sampling,
    std::optional<float> pressure) {
    ResetStrokeLayer();
    controller_.BeginStroke(pageIndex, pagePoint, sampling, pressure);
    controller_.SetPendingStrokeDotted(dotted);
    // The live path replays the controller's stored (clamped) pressures.
    const Stroke* stored = controller_.PendingStroke();
    const bool storesPressure = stored != nullptr && !stored->pressures.empty();
    liveStabilizer_.Reset(sampling, storesPressure);
    liveStroke_ = InkPath {};
    liveStabilizer_.Feed(pagePoint, storesPressure ? stored->pressures.back() : 0.0f, liveStroke_);
    // The press only inks the initial dot: repaint its bounds inflated by the
    // pen half-width and the smoothing lookahead margin.
    const Stroke* begun = controller_.PendingStroke();
    const float width = begun != nullptr ? begun->style.width : 3.0f;
    const float margin = width * 0.5f + kStrokeDirtyMarginPixels;
    MarkTransientDirty(PageRegionDirtyRect(pageIndex, QRectF(
        static_cast<qreal>(pagePoint.x - margin), static_cast<qreal>(pagePoint.y - margin),
        static_cast<qreal>(margin * 2.0f), static_cast<qreal>(margin * 2.0f))));
}

void CanvasWidget::SetStrokeDotted(bool dotted) {
    controller_.SetPendingStrokeDotted(dotted);
    // The dash pattern is path-global, so toggling dotted invalidates the
    // composited prefix.
    ResetStrokeLayer();
    const Stroke* pending = controller_.PendingStroke();
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    if (pending != nullptr && StrokeBounds(*pending, &minX, &minY, &maxX, &maxY)) {
        const float margin = pending->style.width * 0.5f + kStrokeDirtyMarginPixels;
        MarkTransientDirty(PageRegionDirtyRect(controller_.PendingStrokePage(), QRectF(
            static_cast<qreal>(minX - margin), static_cast<qreal>(minY - margin),
            static_cast<qreal>((maxX - minX) + margin * 2.0f),
            static_cast<qreal>((maxY - minY) + margin * 2.0f))));
        return;
    }
    update();
}

void CanvasWidget::AppendStrokePoint(Vec2 pagePoint, bool dotted, std::optional<float> pressure) {
    const std::size_t pageIndex = controller_.PendingStrokePage();
    controller_.ExtendStroke(pagePoint, pressure);
    controller_.SetPendingStrokeDotted(dotted);
    const Stroke* stored = controller_.PendingStroke();
    const float storedPressure = stored != nullptr && !stored->pressures.empty() ? stored->pressures.back() : 0.0f;
    const std::vector<Vec2>& live = liveStroke_.points;
    const std::size_t before = live.size();
    liveStabilizer_.Feed(pagePoint, storedPressure, liveStroke_);
    if (live.size() == before || before == 0U) {
        // A sample the stabilization dropped changes no ink.
        return;
    }
    // Dirty region = the newly stabilized points' bounds, extended back over
    // the smoothing lookahead: the provisional tail (PaintStrokeTail) repaints
    // from the composited chain's end — up to two stabilized points behind the
    // newest one.  Without the lookahead, last frame's tail pixels outside the
    // new segment's bounds stay stale on screen (fragmented ghost ink).
    // QRectF(point, 0x0) is empty and united() drops empty operands, so the
    // span must accumulate explicit min/max rather than union point rects.
    const std::size_t first = before >= 3U ? before - 3U : 0U;
    float minX = live[first].x, minY = live[first].y;
    float maxX = minX, maxY = minY;
    for (std::size_t index = first + 1U; index < live.size(); ++index) {
        const Vec2 p = live[index];
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    // style.width / 2 page units also covers pressure ink at 1.6x
    // (StrokeRenderInkDiameter is width / 2.15 times the factor).
    const float penWidth = stored != nullptr ? stored->style.width : 3.0f;
    const float margin = penWidth * 0.5f + kStrokeDirtyMarginPixels;
    MarkTransientDirty(PageRegionDirtyRect(pageIndex,
        QRectF(minX, minY, maxX - minX, maxY - minY)
            .adjusted(-margin, -margin, margin, margin)));
}

void CanvasWidget::ReplaceStrokePoints(const std::vector<Vec2>& points, bool dotted) {
    const std::size_t pageIndex = controller_.PendingStrokePage();
    ResetStrokeLayer();
    controller_.ReplacePendingStrokePoints(points, dotted);
    // Exact line/oval geometry renders with the identity sampling at
    // constant width.
    liveStabilizer_.Reset({}, false);
    liveStroke_ = InkPath { points, {} };
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
    // The surface lookup must use the pre-commit epoch: EndStroke mutates the
    // page and bumps it.  The captured epoch both validates that the entry's
    // raster is the page's own (indices shift on insert/remove) and keys the
    // composite fast path.
    const std::uint64_t epochBeforeCommit = document.PageEpoch(pageIndex);
    controller_.EndStroke();
    ResetStrokeLayer();
    const CanvasPageView view = CalculatePageView(pageIndex);
    const float devicePixelRatio = cachedDevicePixelRatio_ > 0.0f ? cachedDevicePixelRatio_ : 1.0f;
    SurfaceEntry* entry = nullptr;
    if (const auto pageSurfaces = surfaces_.find(pageIndex); pageSurfaces != surfaces_.end()) {
        for (SurfaceEntry& candidate : pageSurfaces->second.entries) {
            if (candidate.pageEpoch == epochBeforeCommit
                && candidate.scale == view.scale && candidate.devicePixelRatio == devicePixelRatio
                && candidate.cache.valid) {
                entry = &candidate;
                break;
            }
        }
    }
    render::PageSurfaceCache* cache = entry != nullptr ? &entry->cache : nullptr;
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
        // EndStroke bumped the page epoch; re-stamp so the composited raster
        // stays the entry for this page instead of being dropped on the next
        // paint (a full re-render of a dense page would stall the stroke the
        // user is already drawing).
        entry->pageEpoch = document.PageEpoch(pageIndex);
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
    ResetStrokeLayer();
    const CanvasPageView view = CalculatePageView(pageIndex);
    update(view.valid ? view.rect.toAlignedRect() : rect());
}

void CanvasWidget::ClearSelection() {
    // The frame disappears; repaint exactly where it was (handles included).
    const Document& document = controller_.DocumentRef();
    for (std::size_t pageIndex = 0; pageIndex < document.PageCount(); ++pageIndex) {
        if (!controller_.SelectionFor(pageIndex).Empty()) {
            MarkTransientDirty(SelectionDirtyRect(pageIndex));
        }
    }
    controller_.ClearSelection();
}

void CanvasWidget::UpdateSelectionOverlay(
    const QPointF& anchor, const QPointF& current, bool lasso, const std::vector<QPointF>& lassoPoints) {
    // Repaint where the marquee was plus where it is now.
    const QRect before = OverlayDirtyRect();
    // A lasso anchored on PDF text drags a text selection instead of a marquee
    // (Selection.cpp:7237-7241 suppresses the overlay and App paints the
    // covered characters). The probe runs once: the anchor never moves.
    if (lasso) {
        if (!selection_.pdfPage.has_value() && selection_.mode != SelectionGesture::Mode::PdfText) {
            selection_.pdfPage = PageWithPdfTextAt(anchor);
        }
        selection_.mode = selection_.pdfPage.has_value()
            ? SelectionGesture::Mode::PdfText
            : SelectionGesture::Mode::Lasso;
    } else {
        selection_.pdfPage.reset();
        selection_.mode = SelectionGesture::Mode::Box;
    }
    selection_.anchor = anchor;
    selection_.current = current;
    selection_.lassoPoints = lassoPoints;
    const QRect after = OverlayDirtyRect();
    MarkTransientDirty(before.isEmpty() ? after : (after.isEmpty() ? before : before.united(after)));
}

void CanvasWidget::CancelSelectionOverlay() {
    // Drop the live marquee/lasso/pdf drag without committing it.
    const QRect dirty = OverlayDirtyRect();
    selection_ = SelectionGesture {};
    MarkTransientDirty(dirty);
}

void CanvasWidget::CommitSelection(
    const QPointF& anchor, const QPointF& current, bool lasso, bool boxOverride, bool click, bool additive) {
    // Repaint where the marquee/overlay was; the new selection frame's region
    // is painted through update() below.
    MarkTransientDirty(OverlayDirtyRect());
    // The gesture is the single source of truth: committing consumes it.
    SelectionGesture gesture = std::move(selection_);
    selection_ = SelectionGesture {};
    // Direct callers that did not publish an overlay fall back to the hook
    // arguments; the router always goes through UpdateSelectionOverlay.
    const QPointF commitAnchor = gesture.Active() ? gesture.anchor : anchor;
    const QPointF commitCurrent = gesture.Active() ? gesture.current : current;
    const bool commitLasso = gesture.Active()
        ? gesture.mode == SelectionGesture::Mode::Lasso || gesture.mode == SelectionGesture::Mode::PdfText
        : lasso;
    const std::optional<std::size_t> pdfDragPage = gesture.pdfPage;

    const std::optional<CanvasPageView> view = HitTestPageView(commitAnchor);
    if (!view.has_value()) {
        if (!additive) {
            controller_.ClearSelection();
        }
        update();
        return;
    }

    const Document& document = controller_.DocumentRef();
    const Page& page = document.PageAt(view->pageIndex);
    const Vec2 anchorPage = ScreenToPage(*view, commitAnchor, false);
    const Vec2 currentPage = ScreenToPage(*view, commitCurrent, false);

    // Selection.cpp:1373-1431: a lasso (or click) anchored on PDF text selects
    // the character range spanned by the drag instead of hitting objects; the
    // range runs from the nearest box to the anchor to the nearest box at the
    // current point.
    if (pdfDragPage.has_value() && *pdfDragPage == view->pageIndex) {
        if (const std::vector<PdfTextCharBox>* boxes = controller_.PdfTextCharBoxesFor(*pdfDragPage);
            boxes != nullptr && !boxes->empty()) {
            PageSelection selection;
            const std::size_t firstIndex = std::min(
                NearestPdfTextIndex(*boxes, anchorPage), NearestPdfTextIndex(*boxes, currentPage));
            const std::size_t lastIndex = std::max(
                NearestPdfTextIndex(*boxes, anchorPage), NearestPdfTextIndex(*boxes, currentPage));
            selection.pdfText.assign(boxes->begin() + static_cast<std::ptrdiff_t>(firstIndex),
                boxes->begin() + static_cast<std::ptrdiff_t>(lastIndex + 1U));
            // The reference clears every other selection vector when pdf text
            // commits, so the new pdf selection replaces any prior object
            // selection outright.
            controller_.ClearSelection();
            controller_.SelectObjects(view->pageIndex, std::move(selection));
            update();
            return;
        }
    }

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
            const PageText& text = page.texts[index];
            const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page.width);
            if (RotatedRectIntersectsRect(bounds.left, bounds.top, bounds.right, bounds.bottom,
                    text.rotationDegrees, a, b)) {
                selection.texts.push_back(index);
            }
        }
        for (std::size_t index = 0; index < page.latexes.size(); ++index) {
            if (LatexIntersectsRectSelection(page.latexes[index], page.width, a, b)) {
                selection.latexes.push_back(index);
            }
        }
    } else if (commitLasso && !boxOverride) {
        std::vector<Vec2> polygon;
        polygon.reserve(gesture.lassoPoints.size());
        for (const QPointF& point : gesture.lassoPoints) {
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
                const PageText& text = page.texts[index];
                const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page.width);
                if (RotatedRectIntersectsPolygon(bounds.left, bounds.top, bounds.right, bounds.bottom,
                        text.rotationDegrees, polygon)) {
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
            const PageText& text = page.texts[index];
            const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page.width);
            if (RotatedRectIntersectsRect(bounds.left, bounds.top, bounds.right, bounds.bottom,
                    text.rotationDegrees, a, b)) {
                selection.texts.push_back(index);
            }
        }
        for (std::size_t index = 0; index < page.latexes.size(); ++index) {
            if (LatexIntersectsRectSelection(page.latexes[index], page.width, a, b)) {
                selection.latexes.push_back(index);
            }
        }
    }

    if (additive) {
        controller_.AddToSelection(view->pageIndex, std::move(selection));
    } else {
        controller_.SelectObjects(view->pageIndex, std::move(selection));
    }
    update();
}

void CanvasWidget::AddObjectToSelection(ObjectRef object) {
    controller_.AddObjectToSelection(object);
    update();
}

void CanvasWidget::ToggleObjectInSelection(ObjectRef object) {
    controller_.ToggleObjectInSelection(object);
    update();
}

void CanvasWidget::BeginMoveSelection(std::size_t pageIndex) {
    controller_.BeginPageGesture(pageIndex);
    BeginGestureOverlay(pageIndex, false);
}

void CanvasWidget::BeginCloneMoveSelection(std::size_t pageIndex) {
    // Alt-drag: open the move gesture, duplicate the selection inside it, then
    // lift the clones into the drag overlay so the originals stay painted.
    controller_.BeginPageGesture(pageIndex);
    static_cast<void>(controller_.CloneSelection(pageIndex));
    BeginGestureOverlay(pageIndex, false);
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
    // The repaint region is the union of the frame before and after the move.
    const QRect dirtyBefore = SelectionDirtyRect(*pageIndex);
    controller_.MoveSelectionBy(
        *pageIndex,
        Vec2 { static_cast<float>(logicalDelta.x()) / view.scale, static_cast<float>(logicalDelta.y()) / view.scale });
    MarkTransientDirty(dirtyBefore.united(SelectionDirtyRect(*pageIndex)));
}

void CanvasWidget::CommitMoveSelection() {
    controller_.CommitPageGesture();
    EndGestureOverlay();
    update();
}


void CanvasWidget::CancelMoveSelection() {
    if (controller_.GesturePageIndex().has_value()) {
        controller_.CancelPageGesture();
    }
    EndGestureOverlay();
    update();
}

bool CanvasWidget::BeginTransform(const TransformRequest& request) {
    if (transformSession_.has_value()) {
        return false;
    }
    const std::optional<SelectionBounds> from = SelectionBoundsForPage(request.pageIndex);
    if (!from.has_value()) {
        return false;
    }
    transformSession_ = TransformSession { request.handle, *from, request.pressLogical };
    // Same one-transaction-per-gesture machinery as the selection move: the
    // controller snapshots the page and CommitTransform records one
    // ReplacePages action.
    controller_.BeginPageGesture(request.pageIndex);
    BeginGestureOverlay(request.pageIndex, true);
    return true;
}

void CanvasWidget::UpdateTransform(const QPointF& logical, Qt::KeyboardModifiers modifiers) {
    const std::optional<std::size_t> pageIndex = controller_.GesturePageIndex();
    if (!pageIndex.has_value() || !transformSession_.has_value()) {
        return;
    }
    const CanvasPageView view = CalculatePageView(*pageIndex);
    if (!view.valid || view.scale <= 0.0f) {
        return;
    }
    const TransformSession& session = *transformSession_;
    const TransformHandle handle = session.handle;
    if (handle == TransformHandle::Move) {
        // Moving is its own interaction (BeginMoveSelection).
        return;
    }

    const Vec2 pagePoint = ScreenToPage(view, logical, false);
    SelectionTransform transform;
    transform.from = session.from;
    transform.to = session.from;

    const bool shiftPressed = modifiers.testFlag(Qt::ShiftModifier);
    transform.scaleTextFont = shiftPressed;
    if (handle == TransformHandle::Rotate) {
        const Vec2 center {
            (session.from.left + session.from.right) * 0.5f,
            (session.from.top + session.from.bottom) * 0.5f,
        };
        const Vec2 press = ScreenToPage(view, session.pressLogical, false);
        const float pointerAngle = std::atan2(pagePoint.y - center.y, pagePoint.x - center.x);
        const float pressAngle = std::atan2(press.y - center.y, press.x - center.x);
        transform.rotateDegrees = (pointerAngle - pressAngle) * (180.0f / kPi);
        if (shiftPressed) {
            // Reference App.cpp:8997-8999: Shift snaps rotation to 15 degrees.
            transform.rotateDegrees = std::round(transform.rotateDegrees / 15.0f) * 15.0f;
        }
    } else {
        // Resize: each dragged side moves with the pointer while the opposite
        // side stays fixed, so the selection scales about the opposite
        // edge/corner.  The handles sit outside the bounds, so the side follows
        // the pointer's travel since the press rather than jumping to it.
        const bool movesLeft = handle == TransformHandle::Left
            || handle == TransformHandle::TopLeft || handle == TransformHandle::BottomLeft;
        const bool movesRight = handle == TransformHandle::Right
            || handle == TransformHandle::TopRight || handle == TransformHandle::BottomRight;
        const bool movesTop = handle == TransformHandle::Top
            || handle == TransformHandle::TopLeft || handle == TransformHandle::TopRight;
        const bool movesBottom = handle == TransformHandle::Bottom
            || handle == TransformHandle::BottomLeft || handle == TransformHandle::BottomRight;
        const Vec2 press = ScreenToPage(view, session.pressLogical, false);
        const Vec2 travel { pagePoint.x - press.x, pagePoint.y - press.y };
        if (movesLeft) {
            transform.to.left = session.from.left + travel.x;
        }
        if (movesRight) {
            transform.to.right = session.from.right + travel.x;
        }
        if (movesTop) {
            transform.to.top = session.from.top + travel.y;
        }
        if (movesBottom) {
            transform.to.bottom = session.from.bottom + travel.y;
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
                session.from.right - session.from.left);
            const float initialHeight = std::max(kMinSelectionExtent,
                session.from.bottom - session.from.top);
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
                const float centerY = (session.from.top + session.from.bottom) * 0.5f;
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
                const float centerX = (session.from.left + session.from.right) * 0.5f;
                transform.to.left = centerX - (newWidth * 0.5f);
                transform.to.right = centerX + (newWidth * 0.5f);
            }
        }
    }

    // The repaint region is the union of the frame before and after the
    // transform.
    const QRect dirtyBefore = SelectionDirtyRect(*pageIndex);
    controller_.TransformSelection(*pageIndex, transform);
    MarkTransientDirty(dirtyBefore.united(SelectionDirtyRect(*pageIndex)));
}

void CanvasWidget::CommitTransform() {
    if (!transformSession_.has_value()) {
        // Nothing was started; committing must not commit an unrelated page
        // gesture.
        return;
    }
    controller_.CommitPageGesture();
    EndGestureOverlay();
    transformSession_.reset();
    update();
}

void CanvasWidget::CancelTransform() {
    if (controller_.GesturePageIndex().has_value()) {
        controller_.CancelPageGesture();
    }
    EndGestureOverlay();
    transformSession_.reset();
    update();
}

void CanvasWidget::UpdatePendingErase(const std::vector<ObjectRef>& pending) {
    // Fade-in and fade-out each repaint only the objects' own bounds.
    const QRect dirty = PendingEraseDirtyRect();
    pendingErase_ = pending;
    const QRect after = PendingEraseDirtyRect();
    // QRect::united treats an empty rect as a point at the origin, so merge
    // explicitly to avoid dirtying the top-left corner spuriously.
    const QRect combined = dirty.isEmpty() ? after : (after.isEmpty() ? dirty : dirty.united(after));
    MarkTransientDirty(combined);
}

void CanvasWidget::CommitErase(const std::vector<ObjectRef>& pending) {
    const QRect dirty = PendingEraseDirtyRect();
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
    update(dirty.isEmpty() ? rect() : dirty);
}

void CanvasWidget::CommitScratchOut(const std::vector<ObjectRef>& targets) {
    // The scratch's ink fades out over what it erased rather than vanishing.
    if (const Stroke* pending = controller_.PendingStroke(); pending != nullptr) {
        RepaintSnapGhost();
        snapGhost_ = SnapGhost { controller_.PendingStrokePage(), *pending, snapGhostClock_.elapsed() };
        snapGhostTimer_->start();
        RepaintSnapGhost();
    }
    DiscardStroke();
    CommitErase(targets);
}

void CanvasWidget::BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) {
    if (controller_.BeginTextEdit(pageIndex, textIndex)) {
        SyncEditOverlay();
    }
}

void CanvasWidget::CreateTextAt(std::size_t pageIndex, Vec2 pagePoint, float width) {
    // Static text size: the selected small/medium/large preset is stored in
    // page units, so the new text scales with the page at any zoom.
    const ui::CanvasInputSettings input = controller_.InputSettings();
    controller_.AddTextObject(pageIndex, pagePoint, std::max(1.0f, input.textFontSize), width);
    SyncEditOverlay();
}

void CanvasWidget::UpdateTextCreatePreview(std::optional<TextBoxPreview> preview) {
    const QRect before = TextCreatePreviewRect().toAlignedRect();
    if (preview.has_value() && !textCreatePreview_.has_value()) {
        // One empty line in the pending style: what the created text opens as.
        const ui::CanvasInputSettings input = controller_.InputSettings();
        PageText line;
        line.fontSize = std::max(1.0f, input.textFontSize);
        line.fontFamily = input.textFontFamily;
        line.bold = input.textBold;
        line.italic = input.textItalic;
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(line, 0.0f);
        textCreatePreviewHeight_ = bounds.bottom - bounds.top;
    }
    textCreatePreview_ = preview;
    const QRect after = TextCreatePreviewRect().toAlignedRect();
    MarkTransientDirty(before.isEmpty() ? after : (after.isEmpty() ? before : before.united(after)));
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
    if (!VisibleCanvasRect().contains(logical)) {
        return PrecisionInputRoute::PassThrough;
    }
    // The Win32 pointer-frame hook consumes touchpad input at the window
    // level, so its gestures cover the sidebar, toolbar and editors overlaid
    // on the canvas just as much as the canvas itself.  Walk the window's
    // widget tree the way a wheel event would dispatch: when the deepest
    // widget under the cursor is some other chrome, the gesture belongs to
    // it and the platform must keep synthesising the wheel it scrolls with.
    QWidget* under = window();
    QPoint point = under->mapFromGlobal(globalPosition);
    while (QWidget* child = under->childAt(point)) {
        point = child->mapFrom(under, point);
        under = child;
    }
    return under == this ? PrecisionInputRoute::Canvas : PrecisionInputRoute::PassThrough;
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
    // Notches accumulate onto the running target instead of restarting a
    // fixed-duration animation from the current pan: the old cubic re-anchored
    // scrollStart_ on every notch, so each new notch discarded the unfinished
    // tail of the in-flight animation (a fast flick lost distance and the
    // velocity restarted from zero every notch, which read as stutter).
    if (!smoothScroll_.has_value()) {
        smoothScroll_.emplace();
        smoothScroll_->target = QPointF(panX_, panY_);
        smoothScroll_->clock.start();
    }
    smoothScroll_->target += delta;
    if (scrollTimer_ == nullptr) {
        scrollTimer_ = new QTimer(this);
        scrollTimer_->setTimerType(Qt::PreciseTimer);
        scrollTimer_->setInterval(8);
        connect(scrollTimer_, &QTimer::timeout, this, [this]() { UpdateSmoothScroll(); });
    }
    scrollTimer_->start();
}

void CanvasWidget::StopSmoothScroll() {
    smoothScroll_.reset();
    if (scrollTimer_ != nullptr) {
        scrollTimer_->stop();
    }
}

void CanvasWidget::UpdateSmoothScroll() {
    if (!smoothScroll_.has_value()) {
        return;
    }
    // Exponential chase: pan approaches the accumulated target by a fixed
    // fraction of the remaining distance per unit time.  Velocity is
    // continuous across notches (no restart), scales with the backlog (a fast
    // flick moves fast), and is independent of the tick rate.
    constexpr double kScrollChaseMs = 30.0;
    const double dt = static_cast<double>(smoothScroll_->clock.restart());
    const double step = 1.0 - std::exp(-dt / kScrollChaseMs);
    const double dx = smoothScroll_->target.x() - panX_;
    const double dy = smoothScroll_->target.y() - panY_;
    if (std::abs(dx) < 0.5 && std::abs(dy) < 0.5) {
        panX_ = static_cast<float>(smoothScroll_->target.x());
        panY_ = static_cast<float>(smoothScroll_->target.y());
        ClampViewportPan();
        NotifyViewportChanged();
        StopSmoothScroll();
        return;
    }
    const float oldX = panX_;
    const float oldY = panY_;
    panX_ += static_cast<float>(dx * step);
    panY_ += static_cast<float>(dy * step);
    ClampViewportPan();
    if (panX_ == oldX && panY_ == oldY && dt >= 4.0) {
        // The clamp pinned the pan on a real tick: the accumulated target is
        // unreachable (e.g. a flick past the document edge), so the chase
        // would never converge.  Drop the backlog and stop.  A sub-4 ms tick
        // moves too little to measure and keeps chasing instead.
        StopSmoothScroll();
        return;
    }
    NotifyViewportChanged();
}

QRect CanvasWidget::PageRegionDirtyRect(std::size_t pageIndex, const QRectF& pageRect) const {
    const CanvasPageView view = CalculatePageView(pageIndex);
    if (!view.valid) {
        return rect();
    }
    return QRectF(
        view.rect.left() + static_cast<qreal>(pageRect.x() * view.scale),
        view.rect.top() + static_cast<qreal>(pageRect.y() * view.scale),
        static_cast<qreal>(pageRect.width() * view.scale),
        static_cast<qreal>(pageRect.height() * view.scale))
        .toAlignedRect().intersected(rect());
}

void CanvasWidget::MarkTransientDirty(const QRect& logical) {
    if (logical.isEmpty()) {
        return;
    }
    transientDirty_ = transientDirty_.isEmpty() ? logical : transientDirty_.united(logical);
    lastTransientRepaintArea_ = static_cast<std::uint64_t>(logical.width())
        * static_cast<std::uint64_t>(logical.height());
    update(logical);
}

QRect CanvasWidget::PendingEraseDirtyRect() const {
    // Union of every marked object's bounds; marking/unmarking only ever
    // changes pixels inside them.
    QRect dirty;
    const Document& document = controller_.DocumentRef();
    for (const ObjectRef& object : pendingErase_) {
        if (object.pageIndex >= document.PageCount()) {
            continue;
        }
        if (const std::optional<QRectF> bounds = PendingEraseObjectBounds(
                document.PageAt(object.pageIndex), object);
            bounds.has_value()) {
            const QRect rect = PageRegionDirtyRect(object.pageIndex, *bounds);
            dirty = dirty.isEmpty() ? rect : dirty.united(rect);
        }
    }
    return dirty;
}

std::optional<QRectF> CanvasWidget::PendingEraseObjectBounds(
    const Page& page, const ObjectRef& object) const {
    switch (object.kind) {
    case ObjectKind::Stroke: {
        if (object.objectIndex >= page.strokes.size()) {
            return std::nullopt;
        }
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        if (!StrokeBounds(page.strokes[object.objectIndex], &minX, &minY, &maxX, &maxY)) {
            return std::nullopt;
        }
        const float margin = page.strokes[object.objectIndex].style.width * 0.5f + 2.0f;
        return QRectF(
            static_cast<qreal>(minX - margin), static_cast<qreal>(minY - margin),
            static_cast<qreal>((maxX - minX) + margin * 2.0f),
            static_cast<qreal>((maxY - minY) + margin * 2.0f));
    }
    case ObjectKind::Image: {
        if (object.objectIndex >= page.images.size()) {
            return std::nullopt;
        }
        const PageImage& image = page.images[object.objectIndex];
        float minX = page.width, minY = page.height, maxX = 0.0f, maxY = 0.0f;
        for (const Vec2& corner : ImageCorners(image)) {
            minX = std::min(minX, corner.x);
            minY = std::min(minY, corner.y);
            maxX = std::max(maxX, corner.x);
            maxY = std::max(maxY, corner.y);
        }
        return QRectF(minX, minY, maxX - minX, maxY - minY);
    }
    case ObjectKind::Text: {
        if (object.objectIndex >= page.texts.size()) {
            return std::nullopt;
        }
        const PageText& text = page.texts[object.objectIndex];
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page.width);
        float minX = page.width, minY = page.height, maxX = 0.0f, maxY = 0.0f;
        for (const Vec2& corner : RotatedRectCorners(
                bounds.left, bounds.top, bounds.right, bounds.bottom, text.rotationDegrees)) {
            minX = std::min(minX, corner.x);
            minY = std::min(minY, corner.y);
            maxX = std::max(maxX, corner.x);
            maxY = std::max(maxY, corner.y);
        }
        return QRectF(minX, minY, maxX - minX, maxY - minY);
    }
    case ObjectKind::Latex: {
        if (object.objectIndex >= page.latexes.size()) {
            return std::nullopt;
        }
        const PageLatex& latex = page.latexes[object.objectIndex];
        const LatexBounds bounds = MeasurePageLatexBounds(latex, page.width);
        float minX = page.width, minY = page.height, maxX = 0.0f, maxY = 0.0f;
        for (const Vec2& corner : RotatedRectCorners(
                bounds.left, bounds.top, bounds.right, bounds.bottom, latex.rotationDegrees)) {
            minX = std::min(minX, corner.x);
            minY = std::min(minY, corner.y);
            maxX = std::max(maxX, corner.x);
            maxY = std::max(maxY, corner.y);
        }
        return QRectF(minX, minY, maxX - minX, maxY - minY);
    }
    }
    return std::nullopt;
}

QRectF CanvasWidget::PendingErasePageRegion(std::size_t pageIndex) const {
    // Page-space union of the marked objects' bounds: the re-rendered patch
    // covering their faded state.
    QRectF region;
    const Document& document = controller_.DocumentRef();
    if (pageIndex >= document.PageCount()) {
        return region;
    }
    const Page& page = document.PageAt(pageIndex);
    for (const ObjectRef& object : pendingErase_) {
        if (object.pageIndex != pageIndex) {
            continue;
        }
        if (const std::optional<QRectF> bounds = PendingEraseObjectBounds(page, object);
            bounds.has_value()) {
            region = region.isEmpty() ? *bounds : region.united(*bounds);
        }
    }
    return region;
}

QRect CanvasWidget::OverlayDirtyRect() const {
    if (!selection_.Active()) {
        return {};
    }
    // A PDF text drag paints character highlights over the whole page under
    // the anchor instead of a marquee.
    if (selection_.mode == SelectionGesture::Mode::PdfText && selection_.pdfPage.has_value()) {
        const CanvasPageView view = CalculatePageView(*selection_.pdfPage);
        return view.valid ? view.rect.toAlignedRect().intersected(rect()) : rect();
    }
    if (selection_.mode == SelectionGesture::Mode::Lasso && !selection_.lassoPoints.empty()) {
        // QRectF(point, QSizeF(0,0)) is empty, and united() returns the
        // non-empty operand when either side is empty — so unioning
        // point-sized rects collapses the bounds to the LAST point instead
        // of spanning them all.  Track explicit min/max instead.
        float minX = selection_.lassoPoints.front().x();
        float minY = selection_.lassoPoints.front().y();
        float maxX = selection_.lassoPoints.front().x();
        float maxY = selection_.lassoPoints.front().y();
        for (const QPointF& point : selection_.lassoPoints) {
            minX = std::min<float>(minX, point.x());
            minY = std::min<float>(minY, point.y());
            maxX = std::max<float>(maxX, point.x());
            maxY = std::max<float>(maxY, point.y());
        }
        return QRectF(minX, minY, maxX - minX, maxY - minY)
            .adjusted(-2.0, -2.0, 2.0, 2.0).toAlignedRect().intersected(rect());
    }
    return QRectF(selection_.anchor, selection_.current).normalized()
        .adjusted(-2.0, -2.0, 2.0, 2.0).toAlignedRect().intersected(rect());
}

QRectF CanvasWidget::TextCreatePreviewRect() const {
    if (!textCreatePreview_.has_value()) {
        return {};
    }
    const CanvasPageView view = CalculatePageView(textCreatePreview_->pageIndex);
    if (!view.valid) {
        return {};
    }
    return QRectF(
        view.rect.left() + (static_cast<qreal>(textCreatePreview_->topLeft.x) * view.scale),
        view.rect.top() + (static_cast<qreal>(textCreatePreview_->topLeft.y) * view.scale),
        static_cast<qreal>(textCreatePreview_->width) * view.scale,
        static_cast<qreal>(textCreatePreviewHeight_) * view.scale)
        .adjusted(-2.0, -2.0, 2.0, 2.0);
}

void CanvasWidget::PaintTextCreatePreview(QPainter& painter) {
    const QRectF frame = TextCreatePreviewRect().adjusted(2.0, 2.0, -2.0, -2.0);
    if (frame.isEmpty()) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    // A dashed outline in the selection colour: the box the new text wraps in.
    painter.setPen(QPen(QColor::fromRgb(kSelectionColor), 1.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(frame);
    painter.restore();
}

std::optional<SelectionBounds> CanvasWidget::SelectionBoundsForPage(std::size_t pageIndex) const {
    return OutlinesBounds(SelectedObjectOutlines(pageIndex));
}

std::vector<std::array<Vec2, 4>> CanvasWidget::SelectedObjectOutlines(std::size_t pageIndex) const {
    std::vector<std::array<Vec2, 4>> outlines;
    const Document& document = controller_.DocumentRef();
    if (pageIndex >= document.PageCount()) {
        return outlines;
    }
    const Page& page = document.PageAt(pageIndex);
    const PageSelection& selection = controller_.SelectionFor(pageIndex);
    if (selection.Empty()) {
        return outlines;
    }

    for (const std::size_t index : selection.strokes) {
        if (index >= page.strokes.size()) {
            continue;
        }
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        if (StrokeBounds(page.strokes[index], &minX, &minY, &maxX, &maxY)) {
            outlines.push_back({{ { minX, minY }, { maxX, minY }, { maxX, maxY }, { minX, maxY } }});
        }
    }
    for (const std::size_t index : selection.images) {
        if (index < page.images.size()) {
            outlines.push_back(ImageCorners(page.images[index]));
        }
    }
    for (const std::size_t index : selection.texts) {
        if (index >= page.texts.size()) {
            continue;
        }
        const text::QtTextBounds textBounds = text::MeasurePageTextBoundsQt(page.texts[index], page.width);
        outlines.push_back(RotatedRectCorners(
            textBounds.left, textBounds.top, textBounds.right, textBounds.bottom,
            page.texts[index].rotationDegrees));
    }
    for (const std::size_t index : selection.latexes) {
        if (index >= page.latexes.size()) {
            continue;
        }
        const LatexBounds latexBounds = MeasurePageLatexBounds(page.latexes[index], page.width);
        outlines.push_back(RotatedRectCorners(
            latexBounds.left, latexBounds.top, latexBounds.right, latexBounds.bottom,
            page.latexes[index].rotationDegrees));
    }
    return outlines;
}

QRectF CanvasWidget::SelectionFrameRect(const CanvasPageView& view, const SelectionBounds& bounds) {
    return QRectF(
        view.rect.left() + (static_cast<qreal>(bounds.left) * view.scale),
        view.rect.top() + (static_cast<qreal>(bounds.top) * view.scale),
        static_cast<qreal>(bounds.right - bounds.left) * view.scale,
        static_cast<qreal>(bounds.bottom - bounds.top) * view.scale)
        .adjusted(-kSelectionFramePadding, -kSelectionFramePadding, kSelectionFramePadding, kSelectionFramePadding);
}

std::array<QPointF, 4> CanvasWidget::SelectionHandlePositions(const QRectF& frame) {
    const qreal outset = kSelectionHandleOutset;
    return {{
        frame.topLeft() + QPointF(-outset, -outset),
        frame.topRight() + QPointF(outset, -outset),
        frame.bottomRight() + QPointF(outset, outset),
        frame.bottomLeft() + QPointF(-outset, outset),
    }};
}

QPointF CanvasWidget::SelectionRotateHandlePosition(const QRectF& frame) {
    return QPointF(frame.center().x(), frame.top() - kSelectionRotateGap);
}

void CanvasWidget::PaintSelectionOverlay(QPainter& painter) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Committed selection (transient, never cached), drawn like Excalidraw's:
    // one object gets a solid frame; several get a solid outline each plus a
    // dotted frame around them all.  The frame carries white corner and rotate
    // handles.
    const QColor selectionColor = QColor::fromRgb(kSelectionColor);
    const QPen solidPen(selectionColor, 1.0);
    QPen dottedPen(selectionColor, 1.0);
    dottedPen.setDashPattern({ 2.0, 2.0 });
    // Centres the 1 px lines on device pixels (edges on pixel boundaries) so
    // the thin chrome stays crisp instead of smearing over two pixels.
    const qreal dpr = painter.device()->devicePixelRatioF();
    const qreal lineDevice = solidPen.widthF() * dpr;
    const auto snap = [dpr, lineDevice](qreal value) {
        return (std::round((value * dpr) - (lineDevice * 0.5)) + (lineDevice * 0.5)) / dpr;
    };
    const auto snapPoint = [&snap](const QPointF& point) { return QPointF(snap(point.x()), snap(point.y())); };
    for (const CanvasPageView& view : CalculatePageViews()) {
        if (!view.valid) {
            continue;
        }
        const std::vector<std::array<Vec2, 4>> outlines = SelectedObjectOutlines(view.pageIndex);
        const std::optional<SelectionBounds> bounds = OutlinesBounds(outlines);
        if (!bounds.has_value()) {
            continue;
        }
        const QRectF frame = SelectionFrameRect(view, *bounds);

        painter.setBrush(Qt::NoBrush);
        if (outlines.size() > 1U) {
            painter.setPen(solidPen);
            for (const std::array<Vec2, 4>& outline : outlines) {
                QPolygonF polygon = PaddedObjectOutline(view, outline, kSelectionFramePadding);
                for (QPointF& point : polygon) {
                    point = snapPoint(point);
                }
                painter.drawPolygon(polygon);
            }
            painter.setPen(dottedPen);
        } else {
            painter.setPen(solidPen);
        }
        painter.drawRect(QRectF(snapPoint(frame.topLeft()), snapPoint(frame.bottomRight())));

        const qreal handleHalf = kSelectionHandleSize * 0.5;
        const auto handleRect = [&snapPoint, handleHalf](const QPointF& center) {
            return QRectF(snapPoint(center - QPointF(handleHalf, handleHalf)), QSizeF(kSelectionHandleSize, kSelectionHandleSize));
        };
        painter.setPen(solidPen);
        painter.setBrush(Qt::white);
        for (const QPointF& handle : SelectionHandlePositions(frame)) {
            painter.drawRoundedRect(handleRect(handle), kSelectionHandleCornerRadius, kSelectionHandleCornerRadius);
        }
        painter.drawEllipse(handleRect(SelectionRotateHandlePosition(frame)));
    }
    // PDF text highlight (App::PaintPdfTextSelection App.cpp:7818-7915):
    // committed boxes paint translucent over the text; a pdf text drag paints
    // the live range instead of the marquee.
    const QColor pdfHighlight(64, 150, 255, 96);
    for (const CanvasPageView& view : CalculatePageViews()) {
        if (!view.valid || view.scale <= 0.0f) {
            continue;
        }
        const PageSelection& selection = controller_.SelectionFor(view.pageIndex);
        const bool hasCommitted = !selection.pdfText.empty();
        const bool showLive = selection_.mode == SelectionGesture::Mode::PdfText
            && selection_.pdfPage.has_value() && *selection_.pdfPage == view.pageIndex;
        if (!hasCommitted && !showLive) {
            continue;
        }
        const Document& document = controller_.DocumentRef();
        const Page& page = document.PageAt(view.pageIndex);
        // Padding shrinks with zoom like the reference's 0.75 / scale.
        const float padding = 0.75f / view.scale;
        const auto paintBox = [&](const PdfTextCharBox& box) {
            const float left = std::max(0.0f, box.left - padding);
            const float top = std::max(0.0f, box.top - padding);
            const float right = std::min(page.width, box.right + padding);
            const float bottom = std::min(page.height, box.bottom + padding);
            if (right <= left || bottom <= top) {
                return;
            }
            painter.fillRect(QRectF(
                view.rect.left() + (static_cast<qreal>(left) * view.scale),
                view.rect.top() + (static_cast<qreal>(top) * view.scale),
                static_cast<qreal>(right - left) * view.scale,
                static_cast<qreal>(bottom - top) * view.scale),
                pdfHighlight);
        };

        if (hasCommitted) {
            for (const PdfTextCharBox& box : selection.pdfText) {
                paintBox(box);
            }
        }
        if (!showLive) {
            continue;
        }
        const std::vector<PdfTextCharBox>* boxes = controller_.PdfTextCharBoxesFor(view.pageIndex);
        const std::optional<CanvasPageView> currentView = HitTestPageView(selection_.current);
        if (boxes == nullptr || boxes->empty()
            || !currentView.has_value() || currentView->pageIndex != view.pageIndex) {
            continue;
        }
        const Vec2 anchorPage = ScreenToPage(view, selection_.anchor, false);
        const Vec2 currentPage = ScreenToPage(*currentView, selection_.current, false);
        const std::size_t firstIndex = std::min(
            NearestPdfTextIndex(*boxes, anchorPage), NearestPdfTextIndex(*boxes, currentPage));
        const std::size_t lastIndex = std::max(
            NearestPdfTextIndex(*boxes, anchorPage), NearestPdfTextIndex(*boxes, currentPage));
        for (std::size_t index = firstIndex; index <= lastIndex; ++index) {
            paintBox((*boxes)[index]);
        }
    }

    // Live marquee / lasso, Excalidraw's selection box: a solid 1 px outline
    // over a faint blue wash.  A pdf text drag suppresses it entirely (the
    // char boxes above are the feedback).
    if (selection_.Active() && selection_.mode != SelectionGesture::Mode::PdfText) {
        const bool lasso = selection_.mode == SelectionGesture::Mode::Lasso;
        painter.setPen(solidPen);
        painter.setBrush(QColor(0, 0, 200, 10));
        if (lasso && selection_.lassoPoints.size() >= 2U) {
            QPainterPath path;
            path.moveTo(selection_.lassoPoints.front());
            for (std::size_t index = 1; index < selection_.lassoPoints.size(); ++index) {
                path.lineTo(selection_.lassoPoints[index]);
            }
            path.closeSubpath();
            painter.drawPath(path);
        } else if (!lasso) {
            painter.drawRect(QRectF(selection_.anchor, selection_.current).normalized());
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
