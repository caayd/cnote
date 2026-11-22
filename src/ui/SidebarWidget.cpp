#include "ui/SidebarWidget.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>

#include "Document.h"
#include "render/PageRenderer.h"
#include "render/PdfBackgroundProvider.h"
#include "ui/ChromeAssetsQt.h"
#include "ui/ChromeStyleQt.h"

namespace cnote::ui {

namespace {

namespace appui = cnote::appui;
namespace qt = cnote::appui::qt;

QString FormatPageJumpLabel(std::size_t currentPage, std::size_t pageCount) {
    return QStringLiteral("%1 / %2").arg(currentPage + 1U).arg(pageCount);
}

qreal Smoothstep(qreal linear) {
    return linear * linear * (3.0 - (2.0 * linear));
}

// Drag-to-reorder auto-scroll: the band at each end of the content area that
// scrolls the list, and the step per 16 ms tick at the very edge.
constexpr int kDragScrollEdge = 32;
constexpr int kDragScrollMaxStep = 14;
constexpr qreal kDropIndicatorThickness = 3.0;

bool ContainsPixel(const QRect& rect, const QPoint& point) {
    return rect.isValid() && point.x() >= rect.left() && point.x() < rect.right()
        && point.y() >= rect.top() && point.y() < rect.bottom();
}

}  // namespace

SidebarWidget::SidebarWidget(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("cnoteSidebar"));
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    // QVariantAnimation owns frame pacing; the legacy WM_TIMER-style 8 ms
    // PreciseTimer loop is gone.  Qt warns on Custom easing curves inside
    // QVariantAnimation, so the animations stay linear and Smoothstep is
    // applied to the progress in the handlers (same curve as the port).
    slideAnimation_ = new QVariantAnimation(this);
    slideAnimation_->setDuration(kSlideAnimationMs);
    connect(slideAnimation_, &QVariantAnimation::valueChanged, this, [this](const QVariant&) {
        // A value emitted while the old run is being stopped belongs to no
        // transition; it must not republish progress or a terminal state.
        if (slideStopping_) {
            return;
        }
        const qreal t = slideAnimation_->duration() > 0
            ? std::clamp(static_cast<qreal>(slideAnimation_->currentTime()) / slideAnimation_->duration(), 0.0, 1.0)
            : 1.0;
        const qreal from = slideAnimation_->startValue().toReal();
        const qreal to = slideAnimation_->endValue().toReal();
        slideProgress_ = from + ((to - from) * Smoothstep(t));
        if (t >= 1.0) {
            slideState_ = slideTarget_ >= 1.0 ? SlideState::Open : SlideState::Closed;
        }
        UpdateGeometry();
        UpdateVisibility();
        emit SlideProgressChanged(slideProgress_);
    });
    connect(slideAnimation_, &QVariantAnimation::finished, this, [this]() {
        // `stop()` can also emit finished in Qt; a stopped run must not settle
        // the new transition.  Only the run that actually reached its end and
        // still matches the current target may publish Open/Closed.
        if (slideStopping_ || slideAnimation_->currentTime() < slideAnimation_->duration()
            || std::abs(slideAnimation_->endValue().toReal() - slideTarget_) > 0.001) {
            return;
        }
        const bool changed = std::abs(slideProgress_ - slideTarget_) > 0.0001;
        slideProgress_ = slideTarget_;
        slideState_ = slideTarget_ >= 1.0 ? SlideState::Open : SlideState::Closed;
        UpdateGeometry();
        UpdateVisibility();
        if (changed) {
            emit SlideProgressChanged(slideProgress_);
        }
    });

    scrollAnimation_ = new QVariantAnimation(this);
    scrollAnimation_->setDuration(kScrollAnimationMs);
    connect(scrollAnimation_, &QVariantAnimation::valueChanged, this, [this](const QVariant&) {
        const qreal t = scrollAnimation_->duration() > 0
            ? std::clamp(static_cast<qreal>(scrollAnimation_->currentTime()) / scrollAnimation_->duration(), 0.0, 1.0)
            : 1.0;
        const qreal from = scrollAnimation_->startValue().toReal();
        const qreal to = scrollAnimation_->endValue().toReal();
        const int y = static_cast<int>(std::lround(from + ((to - from) * Smoothstep(t))));
        if (y != scrollY_) {
            scrollY_ = y;
            update();
            emit ScrollChanged(scrollY_);
        }
        if (t >= 1.0) {
            scrollTarget_ = y;
        }
    });
    connect(scrollAnimation_, &QVariantAnimation::finished, this, [this]() {
        if (scrollY_ != scrollTarget_) {
            scrollY_ = scrollTarget_;
            update();
            emit ScrollChanged(scrollY_);
        }
    });

    dragScrollTimer_ = new QTimer(this);
    dragScrollTimer_->setInterval(16);
    connect(dragScrollTimer_, &QTimer::timeout, this, &SidebarWidget::AutoScrollPageDrag);

    hide();
}

SidebarWidget::~SidebarWidget() = default;

void SidebarWidget::SetDocument(const Document* document, render::PdfBackgroundProvider* pdfBackground) {
    // A replacement document (or a page insert/remove on the same one) makes
    // the page-jump text and every cached thumbnail meaningless.
    CancelPageJumpEdit();
    const std::size_t pageCount = document != nullptr ? document->PageCount() : 0U;
    const bool documentChanged = document_ != document;
    const bool backgroundChanged = pdfBackground_ != pdfBackground;
    if (!documentChanged && !backgroundChanged && documentPageCount_ == pageCount) {
        return;
    }
    CancelPageDrag();
    ClearPageSelection();
    selectionAnchor_.reset();
    if (documentChanged) {
        // A new document resets navigation and the scroll position; a page
        // insert/remove below only drops the caches.
        currentPage_ = document != nullptr ? document->CurrentPageIndex() : 0;
        scrollY_ = 0;
        scrollTarget_ = 0;
        scrollAnimation_->stop();
        wheelRemainder_ = 0.0;
    }
    document_ = document;
    pdfBackground_ = pdfBackground;
    documentPageCount_ = pageCount;
    InvalidateThumbnails();
    // Removed pages shorten the list: keep the scroll inside it.  Whichever of
    // SetDocument/NotifyDocumentChanged sees the new page count first clamps.
    SetScrollY(scrollY_);
    UpdateVisibility();
    update();
}

void SidebarWidget::NotifyDocumentChanged() {
    // Repaint only for an ordinary content commit: ThumbnailAt re-renders an
    // entry whose page epoch changed, so a commit on one page does not
    // re-rasterize every visible thumbnail.  A page-count change is structural
    // and does invalidate the cache and the page-jump editor.
    if (document_ != nullptr && document_->PageCount() != documentPageCount_) {
        documentPageCount_ = document_->PageCount();
        CancelPageJumpEdit();
        CancelPageDrag();
        InvalidateThumbnails();
        // Removed pages shorten the list: keep the scroll inside it.
        SetScrollY(scrollY_);
    }
    // Any commit (an undo included) may have moved the selected pages; the
    // host reselects a block it just moved (SelectPages).
    ClearPageSelection();
    update();
}

void SidebarWidget::SelectPages(std::size_t first, std::size_t count) {
    const std::size_t pageCount = PageCount();
    selectedPages_.clear();
    for (std::size_t index = first; index < first + count && index < pageCount; ++index) {
        selectedPages_.push_back(index);
    }
    selectionAnchor_ = first < pageCount ? std::optional<std::size_t>(first) : std::nullopt;
    update();
}

void SidebarWidget::ClearPageSelection() {
    if (selectedPages_.empty()) {
        return;
    }
    selectedPages_.clear();
    update();
}

void SidebarWidget::SetCurrentPage(std::size_t index) {
    if (document_ == nullptr || index >= document_->PageCount()) {
        return;
    }
    if (currentPage_ == index) {
        return;
    }
    currentPage_ = index;
    EnsureCurrentPageVisible();
    update();
}

void SidebarWidget::InvalidateThumbnails() {
    thumbnails_.clear();
}

std::size_t SidebarWidget::PageCount() const {
    return document_ != nullptr ? document_->PageCount() : 0U;
}

appui::qt::SidebarMetrics SidebarWidget::Metrics() const {
    return qt::BuildSidebarMetrics();
}

appui::qt::SidebarHeaderLayout SidebarWidget::HeaderLayout() const {
    return qt::BuildSidebarHeaderLayout(QRectF(rect()), maximized_);
}

QRect SidebarWidget::ContentRect() const {
    const qt::SidebarHeaderLayout layout = HeaderLayout();
    return layout.contentRect.toAlignedRect();
}

int SidebarWidget::ItemWidth() const {
    const qt::SidebarHeaderLayout layout = HeaderLayout();
    const qt::SidebarMetrics metrics = Metrics();
    return std::max(1, static_cast<int>(layout.headerRect.width() - (metrics.itemInset * 2.0)));
}

int SidebarWidget::ItemTop(std::size_t index) const {
    if (document_ == nullptr) {
        return 0;
    }
    const qt::SidebarHeaderLayout layout = HeaderLayout();
    const qt::SidebarMetrics metrics = Metrics();
    const int itemWidth = ItemWidth();
    int top = static_cast<int>(layout.contentRect.top() + metrics.contentPadding) - scrollY_;
    for (std::size_t pageIndex = 0; pageIndex < index && pageIndex < document_->PageCount(); ++pageIndex) {
        const Page& page = document_->PageAt(pageIndex);
        const qreal aspect = page.width > 0.0f ? (page.height / page.width) : (11.0f / 8.5f);
        const int thumbHeight = std::max(
            static_cast<int>(metrics.minThumbHeight),
            static_cast<int>(std::lround(static_cast<qreal>(itemWidth) * aspect)));
        top += thumbHeight + static_cast<int>(metrics.labelHeight + metrics.itemGap);
    }
    return top;
}

std::optional<QRect> SidebarWidget::PageItemRect(std::size_t index) const {
    if (document_ == nullptr || index >= document_->PageCount()) {
        return std::nullopt;
    }
    const qt::SidebarMetrics metrics = Metrics();
    const int itemWidth = ItemWidth();
    const int left = ((rect().left() + rect().right()) - itemWidth) / 2;
    const Page& page = document_->PageAt(index);
    const qreal aspect = page.width > 0.0f ? (page.height / page.width) : (11.0f / 8.5f);
    const int thumbHeight = std::max(
        static_cast<int>(metrics.minThumbHeight),
        static_cast<int>(std::lround(static_cast<qreal>(itemWidth) * aspect)));
    const int top = ItemTop(index);
    return QRect(left, top, itemWidth, thumbHeight + static_cast<int>(metrics.labelHeight));
}

std::optional<std::size_t> SidebarWidget::PageAtWidgetPosition(const QPoint& position) const {
    if (document_ == nullptr) {
        return std::nullopt;
    }
    const QRect content = ContentRect();
    if (!content.contains(position)) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < document_->PageCount(); ++index) {
        const std::optional<QRect> item = PageItemRect(index);
        if (item.has_value() && ContainsPixel(*item, position)) {
            return index;
        }
    }
    return std::nullopt;
}

int SidebarWidget::MaxScroll() const {
    if (document_ == nullptr || document_->PageCount() == 0U) {
        return 0;
    }
    const qt::SidebarHeaderLayout layout = HeaderLayout();
    const qt::SidebarMetrics metrics = Metrics();
    const int itemWidth = ItemWidth();
    int contentHeight = static_cast<int>(metrics.contentPadding);
    for (std::size_t pageIndex = 0; pageIndex < document_->PageCount(); ++pageIndex) {
        const Page& page = document_->PageAt(pageIndex);
        const qreal aspect = page.width > 0.0f ? (page.height / page.width) : (11.0f / 8.5f);
        const int thumbHeight = std::max(
            static_cast<int>(metrics.minThumbHeight),
            static_cast<int>(std::lround(static_cast<qreal>(itemWidth) * aspect)));
        contentHeight += thumbHeight + static_cast<int>(metrics.labelHeight + metrics.itemGap);
    }
    const int viewportHeight = std::max(1, layout.contentRect.toAlignedRect().height());
    return std::max(0, contentHeight - viewportHeight);
}

void SidebarWidget::SetScrollY(int scrollY, bool animated) {
    const int clamped = std::clamp(scrollY, 0, MaxScroll());
    if (animated) {
        StartScrollAnimation(clamped);
        return;
    }
    scrollAnimation_->stop();
    scrollTarget_ = clamped;
    if (scrollY_ == clamped) {
        return;
    }
    scrollY_ = clamped;
    update();
    emit ScrollChanged(scrollY_);
}

void SidebarWidget::ScrollByPixels(int pixels, bool animated) {
    SetScrollY(scrollY_ + pixels, animated);
}

void SidebarWidget::StartScrollAnimation(int target) {
    scrollAnimation_->stop();
    scrollTarget_ = target;
    if (target == scrollY_) {
        return;
    }
    scrollAnimation_->setStartValue(scrollY_);
    scrollAnimation_->setEndValue(target);
    scrollAnimation_->start();
}

void SidebarWidget::EnsureCurrentPageVisible() {
    if (document_ == nullptr || document_->PageCount() == 0U) {
        return;
    }
    const qt::SidebarHeaderLayout layout = HeaderLayout();
    const qt::SidebarMetrics metrics = Metrics();
    const std::optional<QRect> item = PageItemRect(currentPage_);
    if (!item.has_value()) {
        return;
    }
    // PageItemRect is widget-space (ItemTop already subtracts scrollY_), so
    // compare against the widget-space content band and fold the current
    // scroll back into the target.  Comparing against scroll-space edges used
    // to add scrollY_ twice, so any ensure-visible beyond the first page
    // jumped the list to an unrelated place.
    const int visibleTop = static_cast<int>(layout.contentRect.top() + metrics.contentPadding);
    const int visibleBottom = static_cast<int>(layout.contentRect.bottom() - metrics.contentPadding);
    if (item->top() < visibleTop) {
        SetScrollY(scrollY_ + (item->top() - visibleTop), true);
    } else if (item->bottom() > visibleBottom) {
        SetScrollY(scrollY_ + (item->bottom() - visibleBottom), true);
    }
}

void SidebarWidget::SetOpen(bool open) {
    // Hiding or replacing the panel abandons a half-typed page jump; the
    // editor is owned by this widget and must not outlive its visible state.
    CancelPageJumpEdit();
    CancelPageDrag();
    ClearPageSelection();
    if (open == IsOpen()) {
        return;
    }
    StartSlideAnimation(open ? 1.0 : 0.0);
    UpdateVisibility();
    emit OpenChanged(open);
}

void SidebarWidget::StartSlideAnimation(qreal target) {
    const qreal clamped = std::clamp(target, 0.0, 1.0);
    // Stop the previous run *before* retargeting, and suppress its `finished`
    // signal: a stop must never publish Open/Closed for the new transition.
    slideStopping_ = true;
    slideAnimation_->stop();
    slideStopping_ = false;
    slideTarget_ = clamped;

    const bool atTarget = std::abs(clamped - slideProgress_) <= 0.0001;
    if (atTarget) {
        slideState_ = clamped >= 1.0 ? SlideState::Open : SlideState::Closed;
        UpdateGeometry();
        UpdateVisibility();
        return;
    }

    // Configuring/starting a stopped QVariantAnimation can synchronously emit
    // valueChanged with the old terminal currentTime, which would settle the
    // new transition before its first real tick.  Block that initial burst and
    // own the transient state explicitly; later ticks are unblocked.
    {
        const QSignalBlocker blocker(slideAnimation_);
        slideAnimation_->setStartValue(slideProgress_);
        slideAnimation_->setEndValue(clamped);
        slideAnimation_->setCurrentTime(0);
        slideAnimation_->start();
    }
    slideState_ = clamped >= 1.0 ? SlideState::Opening : SlideState::Closing;
    UpdateGeometry();
    UpdateVisibility();
}

bool SidebarWidget::SidebarVisible() const {
    return document_ != nullptr && document_->PageCount() > 0U && (IsOpen() || slideProgress_ > 0.001);
}

void SidebarWidget::UpdateVisibility() {
    const bool visible = SidebarVisible();
    if (visible != isVisible()) {
        setVisible(visible);
    }
    // Z-order is the host's call: the sidebar sits above the canvas but below
    // the toolbar band, so it never raises itself (QtMainWindow
    // UpdateChromeGeometry raises it before the toolbar).
}

void SidebarWidget::SetHostClientRect(const QRect& clientRect, int reservedTop) {
    hostClientRect_ = clientRect;
    reservedTop_ = reservedTop;
    UpdateGeometry();
    const int clampedScroll = std::clamp(scrollY_, 0, MaxScroll());
    if (clampedScroll != scrollY_) {
        scrollAnimation_->stop();
        scrollY_ = clampedScroll;
        scrollTarget_ = clampedScroll;
        emit ScrollChanged(scrollY_);
    }
    update();
}

void SidebarWidget::SetMaximized(bool maximized) {
    maximized_ = maximized;
    UpdateGeometry();
    update();
}

void SidebarWidget::UpdateGeometry() {
    if (hostClientRect_.isEmpty()) {
        return;
    }
    const qt::SidebarMetrics metrics = Metrics();
    const int width = static_cast<int>(std::lround(metrics.width));
    const int x = hostClientRect_.left() - static_cast<int>(std::lround(static_cast<qreal>(width) * (1.0 - std::clamp(slideProgress_, 0.0, 1.0))));
    const int y = hostClientRect_.top() + reservedTop_;
    const int height = std::max(1, hostClientRect_.bottom() + 1 - y);
    setGeometry(x, y, width, height);
}

void SidebarWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    update();
}

void SidebarWidget::paintEvent(QPaintEvent*) {
    if (!SidebarVisible()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect hostRect(rect());
    painter.fillRect(hostRect, ChromeColor(kChromeSidebarBackground));

    const qt::SidebarHeaderLayout headerLayout = HeaderLayout();
    const qt::SidebarMetrics metrics = Metrics();
    const QRect contentRect = headerLayout.contentRect.toAlignedRect();

    // Thumbnail list, clipped to the content band exactly like
    // App::PaintPdfSidebarContents (App.cpp:7135 IntersectClipRect).
    painter.save();
    painter.setClipRect(contentRect, Qt::IntersectClip);
    for (std::size_t pageIndex = 0; pageIndex < PageCount(); ++pageIndex) {
        const std::optional<QRect> item = PageItemRect(pageIndex);
        if (!item.has_value()) {
            continue;
        }
        if (item->bottom() < contentRect.top() || item->top() > contentRect.bottom()) {
            continue;
        }
        const QRect labelRect(item->left(), item->bottom() - static_cast<int>(metrics.labelHeight), item->width(), static_cast<int>(metrics.labelHeight));
        const QRect thumbRect(item->left(), item->top(), item->width(), item->height() - static_cast<int>(metrics.labelHeight));
        const bool selected = pageIndex == currentPage_
            || std::binary_search(selectedPages_.begin(), selectedPages_.end(), pageIndex);
        // Dragged pages stay in place, faded, until they are dropped.
        const bool dragged = PageDragActive()
            && std::binary_search(pageDrag_->pages.begin(), pageDrag_->pages.end(), pageIndex);
        painter.setOpacity(dragged ? 0.4 : 1.0);

        if (selected) {
            painter.fillRect(*item, ChromeColor(kChromeSidebarSelected));
        }

        painter.fillRect(thumbRect, QColor(Qt::white));

        if (const QImage* thumbnail = ThumbnailAt(pageIndex, thumbRect.width(), thumbRect.height()); thumbnail != nullptr) {
            painter.drawImage(thumbRect, *thumbnail);
        } else {
            painter.setPen(QPen(ChromeColor(kChromeThumbPlaceholder), 1.0));
            const int inset = static_cast<int>(metrics.placeholderInset);
            const int lineGap = static_cast<int>(metrics.placeholderLineGap);
            painter.drawLine(thumbRect.left() + inset, thumbRect.top() + inset, thumbRect.right() - inset, thumbRect.top() + inset);
            painter.drawLine(thumbRect.left() + inset, thumbRect.top() + inset + lineGap, thumbRect.right() - (inset + 8), thumbRect.top() + inset + lineGap);
            painter.drawLine(thumbRect.left() + inset, thumbRect.top() + inset + (lineGap * 2), thumbRect.right() - (inset + 16), thumbRect.top() + inset + (lineGap * 2));
        }

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ChromeColor(selected ? kChromeAccent : kChromeThumbBorder), selected ? 2.0 : 1.0));
        painter.drawRect(thumbRect.adjusted(0, 0, -1, -1));

        painter.setFont(ChromeFont(14));
        painter.setPen(ChromeColor(selected ? kChromeSidebarLabelSelected : kChromeSidebarLabel));
        painter.drawText(labelRect, Qt::AlignCenter, QString::number(pageIndex + 1U));
    }
    painter.setOpacity(1.0);
    if (const std::optional<std::size_t> slot = DropSlot(); slot.has_value()) {
        if (const std::optional<int> y = DropIndicatorY(*slot); y.has_value()) {
            const int itemWidth = ItemWidth();
            const qreal left = static_cast<qreal>(((rect().left() + rect().right()) - itemWidth) / 2);
            painter.setPen(QPen(ChromeColor(kChromeAccent), kDropIndicatorThickness, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(left, *y), QPointF(left + itemWidth - 1, *y));
        }
    }
    painter.restore();

    // Bottom header band (App.cpp:7085-7108).
    const QRect bottomBar(hostRect.left(), static_cast<int>(headerLayout.contentRect.bottom()), hostRect.width(), hostRect.bottom() + 1 - static_cast<int>(headerLayout.contentRect.bottom()));
    painter.fillRect(bottomBar, ChromeColor(kChromeSidebarBottomBar));
    painter.setPen(QPen(ChromeColor(kChromeSidebarDivider), 1.0));
    painter.drawLine(bottomBar.topLeft(), bottomBar.topRight());

    const QRect addPage = headerLayout.addPageRect.toAlignedRect();
    const QRect newPageSettings = headerLayout.newPageSettingsRect.toAlignedRect();
    const QRect pageLabel = headerLayout.pageLabelRect.toAlignedRect();
    auto paintHeaderIcon = [&](const QRect& target, const QString& iconName, const QString& fallback, const QColor& color) {
        const int iconSize = 26;
        const QRect iconRect(
            target.left() + std::max(0, (target.width() - iconSize) / 2),
            target.top() + std::max(0, (target.height() - iconSize) / 2),
            iconSize,
            iconSize);
        const QPixmap pixmap = ChromeIconPixmap(iconName, QSize(iconSize, iconSize), color, devicePixelRatioF());
        if (!pixmap.isNull()) {
            painter.drawPixmap(iconRect.topLeft(), pixmap);
        } else {
            painter.setPen(color);
            painter.drawText(iconRect, Qt::AlignCenter, fallback);
        }
    };
    paintHeaderIcon(addPage, QStringLiteral("new-page.svg"), QStringLiteral("+"), ChromeColor(kChromeAccent));
    paintHeaderIcon(newPageSettings, QStringLiteral("settings.svg"), QStringLiteral("..."), ChromeColor(kChromeTextPrimary));

    painter.setFont(ChromeFont(14));
    const QString label = PageIndicatorLabel();
    if (pageJump_.active) {
        const QRect entry = pageLabel.adjusted(0, 5, 0, -5);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ChromeColor(kChromeAccent), 1.0));
        painter.drawRect(entry.adjusted(0, 0, -1, -1));
        const QRect textRect = entry.adjusted(6, 0, -6, 0);
        painter.setPen(ChromeColor(kChromeSelectionBorder));
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);
        const qreal advance = QFontMetricsF(ChromeFont(14)).horizontalAdvance(label);
        const qreal caretX = std::min(static_cast<qreal>(textRect.right() - 1), static_cast<qreal>(textRect.left()) + advance + 1.0);
        painter.drawLine(QPointF(caretX, entry.top() + 4.0), QPointF(caretX, entry.bottom() - 4.0));
    } else {
        painter.setPen(ChromeColor(kChromeTextMuted));
        painter.drawText(pageLabel.adjusted(2, 0, -2, 0), Qt::AlignCenter, label);
    }
}

const QImage* SidebarWidget::ThumbnailAt(std::size_t index, int width, int height) {
    if (document_ == nullptr || index >= document_->PageCount() || width <= 0 || height <= 0) {
        return nullptr;
    }
    if (thumbnails_.size() < document_->PageCount()) {
        thumbnails_.resize(document_->PageCount());
    }
    Thumbnail& entry = thumbnails_[index];
    const ThumbnailKey key { document_, document_->PageEpoch(index), width, height };
    if (entry.key == key && entry.key.IsValid()) {
        return &entry.image;
    }

    const Page& page = document_->PageAt(index);
    if (page.width <= 0.0f || page.height <= 0.0f) {
        return nullptr;
    }

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qreal scaleX = static_cast<qreal>(width) / std::max(1.0f, page.width);
    const qreal scaleY = static_cast<qreal>(height) / std::max(1.0f, page.height);
    const float scale = static_cast<float>(std::min(scaleX, scaleY));
    const QRectF destination(0.0, 0.0, static_cast<qreal>(width), static_cast<qreal>(height));

    bool drewBackground = false;
    if (pdfBackground_ != nullptr && page.sourcePdfSourceIndex >= 0 && page.sourcePdfPageIndex >= 0) {
        const QImage background = pdfBackground_->PageBackground(page.sourcePdfSourceIndex, page.sourcePdfPageIndex, scale);
        if (!background.isNull()) {
            // The provider renders source orientation; page rotation applies
            // about the thumbnail centre.
            painter.save();
            painter.translate(destination.center());
            painter.rotate(static_cast<qreal>(page.rotationDegrees));
            painter.drawImage(QRectF(
                    -static_cast<qreal>(background.width()) * 0.5,
                    -static_cast<qreal>(background.height()) * 0.5,
                    static_cast<qreal>(background.width()),
                    static_cast<qreal>(background.height())),
                background);
            painter.restore();
            drewBackground = true;
        }
    }
    if (!drewBackground && !page.raster.pixels.empty() && page.raster.width > 0 && page.raster.height > 0) {
        const QImage view = render::WrapBgraImage(page.raster.pixels.data(), page.raster.width, page.raster.height, page.raster.stride);
        painter.drawImage(destination, view);
    }

    render::PageRenderParams params;
    params.scale = scale;
    params.devicePixelRatio = 1.0f;
    for (const PageImage& pageImage : page.images) {
        render::PaintImage(painter, pageImage, params, 1.0f);
    }
    // The reference thumbnail cache paints images and strokes only; text and
    // LaTeX are intentionally absent (App.cpp:4388-4437).
    for (const Stroke& stroke : page.strokes) {
        render::PaintStrokePreview(painter, stroke, params, 1.0f);
    }
    painter.end();

    entry.key = key;
    entry.image = std::move(image);
    return &entry.image;
}

bool SidebarWidget::HasThumbnail(std::size_t index) const {
    if (document_ == nullptr || index >= document_->PageCount() || index >= thumbnails_.size()) {
        return false;
    }
    const std::optional<QRect> item = PageItemRect(index);
    if (!item.has_value()) {
        return false;
    }
    const ThumbnailKey key {
        document_,
        document_->PageEpoch(index),
        item->width(),
        item->height() - static_cast<int>(Metrics().labelHeight),
    };
    return key == thumbnails_[index].key && key.IsValid();
}

std::size_t SidebarWidget::RenderedThumbnailCount() const {
    std::size_t count = 0;
    for (const Thumbnail& thumbnail : thumbnails_) {
        if (thumbnail.key.IsValid()) {
            ++count;
        }
    }
    return count;
}

std::size_t SidebarWidget::VisibleThumbnailCount() const {
    if (document_ == nullptr) {
        return 0;
    }
    const QRect content = ContentRect();
    std::size_t count = 0;
    for (std::size_t index = 0; index < document_->PageCount(); ++index) {
        const std::optional<QRect> item = PageItemRect(index);
        if (item.has_value() && item->bottom() >= content.top() && item->top() <= content.bottom()) {
            ++count;
        }
    }
    return count;
}

QString SidebarWidget::PageIndicatorLabel() const {
    if (pageJump_.active) {
        return pageJump_.text;
    }
    return FormatPageJumpLabel(currentPage_, PageCount());
}

void SidebarWidget::BeginPageJumpEdit() {
    if (document_ == nullptr || document_->PageCount() == 0U) {
        return;
    }
    pageJump_.active = true;
    pageJump_.text = QString::number(currentPage_ + 1U);
    setFocus(Qt::MouseFocusReason);
    update();
}

void SidebarWidget::CancelPageJumpEdit() {
    if (!pageJump_.active) {
        return;
    }
    pageJump_.active = false;
    pageJump_.text.clear();
    update();
}

void SidebarWidget::CommitPageJumpEdit() {
    if (!pageJump_.active) {
        return;
    }
    bool ok = false;
    const unsigned long value = pageJump_.text.toULong(&ok);
    pageJump_.active = false;
    pageJump_.text.clear();
    update();
    if (!ok || value == 0UL) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(value - 1UL);
    if (document_ != nullptr && index < document_->PageCount()) {
        emit PageJumpRequested(index);
    }
}

std::optional<std::size_t> SidebarWidget::DropSlot() const {
    return PageDragActive() ? pageDrag_->dropSlot : std::nullopt;
}

std::optional<int> SidebarWidget::DropIndicatorY(std::size_t slot) const {
    const std::size_t pageCount = PageCount();
    if (pageCount == 0U || slot > pageCount) {
        return std::nullopt;
    }
    const int halfGap = static_cast<int>(std::lround(Metrics().itemGap / 2.0));
    if (slot < pageCount) {
        return PageItemRect(slot)->top() - halfGap;
    }
    return PageItemRect(pageCount - 1U)->bottom() + 1 + halfGap;
}

void SidebarWidget::UpdatePageDrag(const QPoint& position) {
    if (!pageDrag_.has_value()) {
        return;
    }
    pageDrag_->position = position;
    if (!pageDrag_->active) {
        if ((position - pageDrag_->pressPosition).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }
        pageDrag_->active = true;
        setCursor(Qt::ClosedHandCursor);
    }

    // The gap above the first page whose centre is below the pointer; past
    // the last centre, the gap after the last page.
    const std::size_t pageCount = PageCount();
    std::size_t slot = pageCount;
    for (std::size_t index = 0; index < pageCount; ++index) {
        if (position.y() < PageItemRect(index)->center().y()) {
            slot = index;
            break;
        }
    }
    // A slot that would leave every page where it is (a single page's own
    // gaps, or inside or around an already contiguous block) is no drop.
    const std::vector<std::size_t> order = Document::MovedPageOrder(pageCount, pageDrag_->pages, slot);
    bool moves = false;
    for (std::size_t index = 0; index < order.size() && !moves; ++index) {
        moves = order[index] != index;
    }
    pageDrag_->dropSlot = moves ? std::optional<std::size_t>(slot) : std::nullopt;

    const QRect content = ContentRect();
    const bool nearEdge = position.y() < content.top() + kDragScrollEdge || position.y() > content.bottom() - kDragScrollEdge;
    if (nearEdge && !dragScrollTimer_->isActive()) {
        dragScrollTimer_->start();
    } else if (!nearEdge) {
        dragScrollTimer_->stop();
    }
    update();
}

void SidebarWidget::AutoScrollPageDrag() {
    if (!PageDragActive()) {
        dragScrollTimer_->stop();
        return;
    }
    const QRect content = ContentRect();
    const int y = pageDrag_->position.y();
    int step = 0;
    if (y < content.top() + kDragScrollEdge) {
        const int depth = std::min(kDragScrollEdge, content.top() + kDragScrollEdge - y);
        step = -std::max(1, (kDragScrollMaxStep * depth) / kDragScrollEdge);
    } else if (y > content.bottom() - kDragScrollEdge) {
        const int depth = std::min(kDragScrollEdge, y - (content.bottom() - kDragScrollEdge));
        step = std::max(1, (kDragScrollMaxStep * depth) / kDragScrollEdge);
    }
    const int before = scrollY_;
    SetScrollY(scrollY_ + step);
    if (scrollY_ == before) {
        dragScrollTimer_->stop();
        return;
    }
    UpdatePageDrag(pageDrag_->position);
}

void SidebarWidget::CancelPageDrag() {
    if (!pageDrag_.has_value()) {
        return;
    }
    const bool wasActive = pageDrag_->active;
    pageDrag_.reset();
    dragScrollTimer_->stop();
    if (wasActive) {
        unsetCursor();
        update();
    }
}

void SidebarWidget::mousePressEvent(QMouseEvent* event) {
    CancelPageDrag();
    if (event->button() == Qt::RightButton) {
        if (const auto page = PageAtWidgetPosition(event->position().toPoint()); page.has_value()) {
            // A right-click inside the multi-page selection keeps it, so the
            // page menu acts on every selected page; elsewhere it drops it.
            if (!std::binary_search(selectedPages_.begin(), selectedPages_.end(), *page)) {
                ClearPageSelection();
                selectionAnchor_ = *page;
            }
            emit PageActivated(*page);
            emit PageContextMenuRequested(*page, event->globalPosition().toPoint());
        }
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const QPoint position = event->position().toPoint();
    const qt::SidebarHeaderLayout headerLayout = HeaderLayout();

    if (pageJump_.active && !ContainsPixel(headerLayout.pageLabelRect.toAlignedRect(), position)) {
        CommitPageJumpEdit();
    }

    if (ContainsPixel(headerLayout.pageLabelRect.toAlignedRect(), position)) {
        BeginPageJumpEdit();
        return;
    }
    if (ContainsPixel(headerLayout.addPageRect.toAlignedRect(), position)) {
        emit AddPageRequested();
        return;
    }
    if (ContainsPixel(headerLayout.newPageSettingsRect.toAlignedRect(), position)) {
        emit NewPageSettingsRequested();
        return;
    }
    if (const auto page = PageAtWidgetPosition(position); page.has_value()) {
        PageDrag drag;
        drag.pressedPage = *page;
        drag.pressPosition = position;
        drag.position = position;
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            // Range from the anchor; the anchor stays for the next shift-click.
            const std::size_t anchor = std::min(selectionAnchor_.value_or(currentPage_), PageCount() - 1U);
            selectedPages_.clear();
            for (std::size_t index = std::min(anchor, *page); index <= std::max(anchor, *page); ++index) {
                selectedPages_.push_back(index);
            }
            selectionAnchor_ = anchor;
            update();
        } else if (std::binary_search(selectedPages_.begin(), selectedPages_.end(), *page)) {
            drag.collapseOnClick = true;  // keep the selection: this may drag it
        } else {
            ClearPageSelection();
            selectionAnchor_ = *page;
        }
        // A press may become a reorder drag once it moves far enough; it
        // carries the whole selection when the pressed page is part of it.
        drag.pages = selectedPages_.size() > 1U
                && std::binary_search(selectedPages_.begin(), selectedPages_.end(), *page)
            ? selectedPages_
            : std::vector<std::size_t> { *page };
        pageDrag_ = std::move(drag);
        emit PageActivated(*page);
    }
    // The reference HandleSidebarClick consumes any click inside the sidebar.
}

void SidebarWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!pageDrag_.has_value() || !event->buttons().testFlag(Qt::LeftButton)) {
        event->ignore();
        return;
    }
    UpdatePageDrag(event->position().toPoint());
    event->accept();
}

void SidebarWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !pageDrag_.has_value()) {
        event->ignore();
        return;
    }
    const std::vector<std::size_t> pages = pageDrag_->pages;
    const bool collapse = !pageDrag_->active && pageDrag_->collapseOnClick;
    const std::size_t pressedPage = pageDrag_->pressedPage;
    const std::optional<std::size_t> slot = DropSlot();
    CancelPageDrag();
    if (collapse) {
        // A plain click on a selected page selects only that page.
        ClearPageSelection();
        selectionAnchor_ = pressedPage;
    }
    if (slot.has_value()) {
        emit PagesMoveRequested(pages, *slot);
    }
    event->accept();
}

void SidebarWidget::wheelEvent(QWheelEvent* event) {
    int delta = event->angleDelta().y();
    if (!event->pixelDelta().isNull()) {
        delta = event->pixelDelta().y() * 8;  // precision scrolling reported as pixels
    }
    if (delta == 0) {
        event->ignore();
        return;
    }

    int lines = QApplication::wheelScrollLines();
    float scrollPixels = 0.0f;
    if (lines < 0) {
        scrollPixels = (static_cast<float>(delta) / 120.0f) * static_cast<float>(std::max(1, ContentRect().height()));
    } else {
        if (lines == 0) {
            lines = 3;
        }
        scrollPixels = (static_cast<float>(delta) / 120.0f) * static_cast<float>(lines) * 18.0f;
    }

    wheelRemainder_ += scrollPixels;
    const int wholePixels = static_cast<int>(std::trunc(wheelRemainder_));
    wheelRemainder_ -= static_cast<float>(wholePixels);
    const int target = std::clamp(scrollY_ - wholePixels, 0, MaxScroll());
    if (target != scrollY_) {
        scrollAnimation_->stop();
        scrollY_ = target;
        scrollTarget_ = target;
        update();
        emit ScrollChanged(scrollY_);
        if (PageDragActive()) {
            UpdatePageDrag(pageDrag_->position);
        }
    }
    // Sidebar-local scrolling never pans the canvas (plan step 5.3).
    event->accept();
}

bool SidebarWidget::event(QEvent* event) {
    // The page-jump editor is hand-drawn, not a real line edit, so it must
    // claim ShortcutOverride itself: otherwise window shortcuts (zoom presets
    // on 1-5, tool keys) swallow the digits it is editing.
    if (pageJump_.active && event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        const QString text = key->text();
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
            || key->key() == Qt::Key_Escape || key->key() == Qt::Key_Backspace
            || (!text.isEmpty() && text.at(0).isDigit())) {
            event->accept();
            return true;
        }
    }
    // Escape cancels a page drag before any window shortcut sees it.
    if (PageDragActive() && event->type() == QEvent::ShortcutOverride
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void SidebarWidget::keyPressEvent(QKeyEvent* event) {
    if (PageDragActive() && event->key() == Qt::Key_Escape) {
        CancelPageDrag();
        return;
    }
    if (!pageJump_.active) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        CommitPageJumpEdit();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        CancelPageJumpEdit();
        return;
    }
    if (event->key() == Qt::Key_Backspace) {
        pageJump_.text.chop(1);
        update();
        return;
    }
    const QString text = event->text();
    if (!text.isEmpty() && text.at(0).isDigit()) {
        pageJump_.text += text;
        update();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SidebarWidget::focusOutEvent(QFocusEvent* event) {
    // The page indicator is a hand-drawn editor; losing focus must behave like
    // Return (emit the jump intent) instead of leaving it visually stuck.
    if (pageJump_.active) {
        CommitPageJumpEdit();
    }
    QWidget::focusOutEvent(event);
}

}  // namespace cnote::ui
