#include "ui/SidebarWidget.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
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

    slideTimer_ = new QTimer(this);
    slideTimer_->setTimerType(Qt::PreciseTimer);
    slideTimer_->setInterval(8);
    connect(slideTimer_, &QTimer::timeout, this, &SidebarWidget::OnSlideTick);

    scrollTimer_ = new QTimer(this);
    scrollTimer_->setTimerType(Qt::PreciseTimer);
    scrollTimer_->setInterval(8);
    connect(scrollTimer_, &QTimer::timeout, this, &SidebarWidget::OnScrollTick);

    hide();
}

SidebarWidget::~SidebarWidget() = default;

void SidebarWidget::SetDocument(const Document* document, render::PdfBackgroundProvider* pdfBackground) {
    if (document_ == document && pdfBackground_ == pdfBackground) {
        return;
    }
    document_ = document;
    pdfBackground_ = pdfBackground;
    currentPage_ = document != nullptr ? document->CurrentPageIndex() : 0;
    scrollY_ = 0;
    scrollTarget_ = 0;
    wheelRemainder_ = 0.0;
    InvalidateThumbnails();
    UpdateVisibility();
    update();
}

void SidebarWidget::NotifyDocumentChanged() {
    InvalidateThumbnails();
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
    scrollTimer_->stop();
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
    scrollFrom_ = scrollY_;
    scrollTarget_ = target;
    if (scrollFrom_ == scrollTarget_) {
        return;
    }
    scrollClock_.start();
    scrollTimer_->start();
}

void SidebarWidget::OnScrollTick() {
    const qreal linear = std::clamp(static_cast<qreal>(scrollClock_.elapsed()) / kScrollAnimationMs, 0.0, 1.0);
    const qreal eased = Smoothstep(linear);
    const int value = static_cast<int>(std::lround(static_cast<qreal>(scrollFrom_) + (static_cast<qreal>(scrollTarget_ - scrollFrom_) * eased)));
    if (value != scrollY_) {
        scrollY_ = value;
        update();
        emit ScrollChanged(scrollY_);
    }
    if (linear >= 1.0) {
        scrollTimer_->stop();
        if (scrollY_ != scrollTarget_) {
            scrollY_ = scrollTarget_;
            update();
            emit ScrollChanged(scrollY_);
        }
    }
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
    const int visibleTop = static_cast<int>(layout.contentRect.top() + metrics.contentPadding + scrollY_);
    const int visibleBottom = static_cast<int>(layout.contentRect.bottom() - metrics.contentPadding + scrollY_);
    if (item->top() < visibleTop) {
        SetScrollY(static_cast<int>(item->top() - layout.contentRect.top() - metrics.contentPadding), true);
    } else if (item->bottom() > visibleBottom) {
        SetScrollY(static_cast<int>(item->bottom() - (layout.contentRect.bottom() - metrics.contentPadding)), true);
    }
}

void SidebarWidget::SetOpen(bool open) {
    if (open_ == open) {
        return;
    }
    open_ = open;
    UpdateVisibility();
    StartSlideAnimation(open ? 1.0 : 0.0);
    emit OpenChanged(open_);
}

void SidebarWidget::StartSlideAnimation(qreal target) {
    const qreal clamped = std::clamp(target, 0.0, 1.0);
    slideFrom_ = static_cast<int>(std::lround(slideProgress_ * 1000.0));
    const int to = static_cast<int>(std::lround(clamped * 1000.0));
    if (slideFrom_ == to) {
        slideProgress_ = clamped;
        UpdateGeometry();
        return;
    }
    slideTarget_ = to;
    slideClock_.start();
    slideTimer_->start();
}

void SidebarWidget::OnSlideTick() {
    const qreal linear = std::clamp(static_cast<qreal>(slideClock_.elapsed()) / kSlideAnimationMs, 0.0, 1.0);
    const qreal eased = Smoothstep(linear);
    const qreal from = static_cast<qreal>(slideFrom_) / 1000.0;
    const qreal to = static_cast<qreal>(slideTarget_) / 1000.0;
    slideProgress_ = from + ((to - from) * eased);
    if (linear >= 1.0) {
        slideProgress_ = to;
        slideTimer_->stop();
    }
    UpdateGeometry();
    UpdateVisibility();
    emit SlideProgressChanged(slideProgress_);
}

bool SidebarWidget::SidebarVisible() const {
    return document_ != nullptr && document_->PageCount() > 0U && (open_ || slideProgress_ > 0.001);
}

void SidebarWidget::UpdateVisibility() {
    const bool visible = SidebarVisible();
    if (visible != isVisible()) {
        setVisible(visible);
    }
    if (visible && open_ && slideProgress_ >= 0.999) {
        raise();
    }
}

void SidebarWidget::SetHostClientRect(const QRect& clientRect, int reservedTop) {
    hostClientRect_ = clientRect;
    reservedTop_ = reservedTop;
    UpdateGeometry();
    const int clampedScroll = std::clamp(scrollY_, 0, MaxScroll());
    if (clampedScroll != scrollY_) {
        scrollY_ = clampedScroll;
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
        const bool selected = pageIndex == currentPage_;

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
    if (pageJumpEditing_) {
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
    if (entry.valid && entry.width == width && entry.height == height) {
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
            painter.drawImage(destination, background);
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

    entry.image = std::move(image);
    entry.width = width;
    entry.height = height;
    entry.valid = true;
    return &entry.image;
}

bool SidebarWidget::HasThumbnail(std::size_t index) const {
    return index < thumbnails_.size() && thumbnails_[index].valid;
}

std::size_t SidebarWidget::RenderedThumbnailCount() const {
    std::size_t count = 0;
    for (const Thumbnail& thumbnail : thumbnails_) {
        if (thumbnail.valid) {
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
    if (pageJumpEditing_) {
        return pageJumpText_;
    }
    return FormatPageJumpLabel(currentPage_, PageCount());
}

void SidebarWidget::BeginPageJumpEdit() {
    if (document_ == nullptr || document_->PageCount() == 0U) {
        return;
    }
    pageJumpEditing_ = true;
    pageJumpText_ = QString::number(currentPage_ + 1U);
    setFocus(Qt::MouseFocusReason);
    update();
}

void SidebarWidget::CancelPageJumpEdit() {
    if (!pageJumpEditing_) {
        return;
    }
    pageJumpEditing_ = false;
    pageJumpText_.clear();
    update();
}

void SidebarWidget::CommitPageJumpEdit() {
    if (!pageJumpEditing_) {
        return;
    }
    bool ok = false;
    const unsigned long value = pageJumpText_.toULong(&ok);
    pageJumpEditing_ = false;
    pageJumpText_.clear();
    update();
    if (!ok || value == 0UL) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(value - 1UL);
    if (document_ != nullptr && index < document_->PageCount()) {
        emit PageJumpRequested(index);
    }
}

void SidebarWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        if (const auto page = PageAtWidgetPosition(event->position().toPoint()); page.has_value()) {
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

    if (pageJumpEditing_ && !ContainsPixel(headerLayout.pageLabelRect.toAlignedRect(), position)) {
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
        emit PageActivated(*page);
    }
    // The reference HandleSidebarClick consumes any click inside the sidebar.
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
        scrollTimer_->stop();
        scrollY_ = target;
        update();
        emit ScrollChanged(scrollY_);
    }
    // Sidebar-local scrolling never pans the canvas (plan step 5.3).
    event->accept();
}

void SidebarWidget::keyPressEvent(QKeyEvent* event) {
    if (!pageJumpEditing_) {
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
        pageJumpText_.chop(1);
        update();
        return;
    }
    const QString text = event->text();
    if (!text.isEmpty() && text.at(0).isDigit()) {
        pageJumpText_ += text;
        update();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace cnote::ui
