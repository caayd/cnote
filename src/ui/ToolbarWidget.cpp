#include "ui/ToolbarWidget.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QTimer>
#include <QWindow>

#include "ui/ChromeAssetsQt.h"
#include "ui/ChromeStyleQt.h"

namespace cnote::ui {

namespace {

namespace appui = cnote::appui;
namespace qt = cnote::appui::qt;

constexpr qreal kWheelPi = 3.14159265358979323846;
constexpr qreal kQuickSizePreviewMinDip = 4.0;   // App.cpp:163
constexpr qreal kQuickSizeThumbHalfDip = 6.0;    // App.cpp:164
constexpr qreal kEraserMinRadiusPixels = 3.5f;   // App.cpp:168
constexpr qreal kEraserOutlinePixels = 0.75f;    // App.cpp:169
constexpr int kWheelBadgeInsetRightDip = 3;      // App.cpp:173
constexpr int kWheelBadgeTopDip = 4;             // App.cpp:174
constexpr int kWheelBadgeSizeDip = 6;            // App.cpp:175
constexpr float kMinBrushSize = 0.5f;            // App.cpp:994
constexpr float kMaxBrushSize = 20.0f;           // App.cpp:995
constexpr float kBrushSizeStep = 0.25f;          // App.cpp:996
constexpr float kQuickSizePresetSnapRadius = 0.75f;  // ToolbarState.cpp:24
constexpr int kMaxPaletteMenuColumns = 4;        // Layout.cpp:377

// Palette-wheel paint constants (App.cpp:155-164, App::PaintPaletteWheel).
constexpr std::uint32_t kWheelHalo = 0xD2121820U;             // App.cpp:6782 (210,18,24,32)
constexpr std::uint32_t kWheelUnselectedFill = 0xF5202934U;   // App.cpp:6855 (245,32,41,52)
constexpr std::uint32_t kWheelSelectedFill = 0xFF3D76A4U;     // App.cpp:6853 (255,61,118,164)
constexpr std::uint32_t kWheelSelectedStroke = 0xFFF4F7FCU;   // App.cpp:6877 (255,244,247,252)
constexpr std::uint32_t kWheelSeparatorStroke = 0x78F4F7FCU;  // App.cpp:6878 (120,244,247,252)
constexpr std::uint32_t kWheelToolIconTint = 0xFFE8EEF5U;     // App.cpp:6892 RGB(232,238,245)
constexpr std::uint32_t kWheelSelectedIconTint = 0xFFFFFFFFU;  // App.cpp:6892
constexpr std::uint32_t kWheelCenterFill = 0xFF121820U;       // App.cpp:7022 (255,18,24,32)
constexpr std::uint32_t kWheelZoomCenterFill = 0xFF253A4EU;   // App.cpp:7021 (255,37,58,78)
constexpr std::uint32_t kWheelCenterBorder = 0xB4F4F7FCU;     // App.cpp:7037 (180,244,247,252)
constexpr std::uint32_t kWheelZoomCenterBorder = 0xEB84BFFFU;  // App.cpp:7036 (235,132,191,255)
constexpr qreal kPaletteWheelShadowInsetDip = 6.0;            // App.cpp:156
constexpr qreal kPaletteWheelStrokeDip = 1.2f;                // App.cpp:161
constexpr qreal kPaletteWheelSelectedStrokeDip = 3.0f;        // App.cpp:160
constexpr qreal kPaletteWheelBorderDip = 1.5f;                // App.cpp:162
constexpr qreal kPaletteWheelCenterIconHalfDip = 13.0;        // App.cpp:7027 ScaleForDpi(13)
constexpr qreal kPaletteWheelSwatchDropDip = 72.0;            // ToolbarState.cpp:135 (logical px)

// Win32 PtInRect semantics: the right/bottom edges are exclusive.
bool ContainsPoint(const QRectF& rect, const QPointF& point) {
    return point.x() >= rect.left() && point.x() < rect.right()
        && point.y() >= rect.top() && point.y() < rect.bottom();
}

bool ContainsPixel(const QRect& rect, const QPoint& point) {
    return rect.isValid() && point.x() >= rect.left() && point.x() < rect.right()
        && point.y() >= rect.top() && point.y() < rect.bottom();
}

float SnapBrushSize(float size) {
    const float clamped = std::clamp(size, kMinBrushSize, kMaxBrushSize);
    const float steps = std::round((clamped - kMinBrushSize) / kBrushSizeStep);
    return kMinBrushSize + (steps * kBrushSizeStep);
}

QString FormatBrushSizeValue(float size) {
    QString value = QString::number(SnapBrushSize(size), 'f', 2);
    while (value.endsWith(QLatin1Char('0'))) {
        value.chop(1);
    }
    if (value.endsWith(QLatin1Char('.'))) {
        value.chop(1);
    }
    return value;
}

QString FormatFontSizeValue(float size) {
    QString value = QString::number(size, 'f', 1);
    while (value.endsWith(QLatin1Char('0'))) {
        value.chop(1);
    }
    if (value.endsWith(QLatin1Char('.'))) {
        value.chop(1);
    }
    return value;
}

QString ToolIconName(settings::Tool tool) {
    switch (tool) {
    case settings::Tool::Select:
        return QStringLiteral("lasso.svg");
    case settings::Tool::Move:
        return QStringLiteral("cursor.svg");
    case settings::Tool::Text:
    case settings::Tool::Latex:
        return QStringLiteral("text.svg");
    case settings::Tool::Draw:
        return QStringLiteral("draw.svg");
    case settings::Tool::Pan:
        return QStringLiteral("pan.svg");
    case settings::Tool::Erase:
        return QStringLiteral("erase.svg");
    case settings::Tool::Zoom:
        return QStringLiteral("zoom.svg");
    }
    return {};
}

QString ToolFallbackLabel(settings::Tool tool) {
    switch (tool) {
    case settings::Tool::Select:
        return QStringLiteral("S");
    case settings::Tool::Move:
        return QStringLiteral("M");
    case settings::Tool::Text:
    case settings::Tool::Latex:
        return QStringLiteral("T");
    case settings::Tool::Draw:
        return QStringLiteral("D");
    case settings::Tool::Pan:
        return QStringLiteral("H");
    case settings::Tool::Erase:
        return QStringLiteral("E");
    case settings::Tool::Zoom:
        return QStringLiteral("Z");
    }
    return {};
}

QString FontFamilyLabel(TextFontFamily family) {
    switch (family) {
    case TextFontFamily::Serif:
        return QStringLiteral("Serif");
    case TextFontFamily::Monospace:
        return QStringLiteral("Monospace");
    case TextFontFamily::ShantellSans:
        return QStringLiteral("Shantell Sans");
    case TextFontFamily::Sans:
    default:
        return QStringLiteral("Sans");
    }
}

QString AlignmentIconName(TextAlignment alignment) {
    switch (alignment) {
    case TextAlignment::Center:
        return QStringLiteral("text/cent-or.svg");
    case TextAlignment::Right:
        return QStringLiteral("text/right-or.svg");
    case TextAlignment::Left:
    default:
        return QStringLiteral("text/left-or.svg");
    }
}

void PaintIconOrFallback(QPainter& painter, const QRectF& rect, const QString& iconName, const QString& fallback, const QColor& color, int fontPixelSize) {
    const QPixmap pixmap = ChromeIconPixmap(iconName, rect.size().toSize(), color, painter.device() != nullptr ? painter.device()->devicePixelRatioF() : 1.0);
    if (!pixmap.isNull()) {
        painter.drawPixmap(rect.topLeft(), pixmap);
        return;
    }
    painter.setPen(color);
    QFont font = ChromeFont(fontPixelSize);
    painter.setFont(font);
    painter.drawText(rect, Qt::AlignCenter, fallback);
}

// The reference preview diameter (App::QuickSizePreviewDiameterForTool).
qreal PreviewDiameter(settings::Tool tool, float brushSize, qreal pageScale) {
    if (tool == settings::Tool::Erase) {
        const qreal radius = std::max(kEraserMinRadiusPixels, static_cast<qreal>(brushSize) * 4.0);
        return radius * 2.0;
    }
    return std::max(1.25, static_cast<qreal>(brushSize) * std::max(0.1, pageScale) / 2.15);
}

int PreviewExtent(settings::Tool tool, float brushSize, qreal pageScale) {
    return std::max(
        static_cast<int>(kQuickSizePreviewMinDip),
        static_cast<int>(std::ceil(PreviewDiameter(tool, brushSize, pageScale))) + 2);
}

float SnapQuickSizeValue(float size, float sliderMax, const std::vector<float>& presets) {
    const float clamped = std::clamp(size, kMinBrushSize, sliderMax);
    if (!presets.empty()) {
        float nearest = std::numeric_limits<float>::max();
        for (const float preset : presets) {
            if (preset > sliderMax) {
                continue;
            }
            const float distance = std::abs(preset - clamped);
            if (distance < nearest) {
                nearest = distance;
            }
        }
        for (const float preset : presets) {
            if (preset <= sliderMax && std::abs(preset - clamped) <= kQuickSizePresetSnapRadius
                && std::abs(preset - clamped) <= nearest + 1e-6f) {
                return preset;
            }
        }
    }
    return SnapBrushSize(clamped);
}

}  // namespace

// ---------------------------------------------------------------------------
// Quick-size brush popup (App::PaintQuickSizePopup / HandleQuickSizePopupClick)
// ---------------------------------------------------------------------------
class QuickSizePopup final : public QWidget {
public:
    QuickSizePopup(QWidget* host, ToolbarWidget* owner)
        : QWidget(host), owner_(owner) {
        setObjectName(QStringLiteral("cnoteQuickSizePopup"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
    }

    void Configure(const qt::QuickSizePopupLayout& layout, const QRect& bounds) {
        layout_ = layout;
        bounds_ = bounds;
        setGeometry(bounds);
        update();
    }

    void SetState(settings::Tool tool, float brushSize, float sliderMax, const std::vector<float>& presets, std::uint32_t strokeColor) {
        tool_ = tool;
        brushSize_ = brushSize;
        sliderMax_ = std::max(1.0f, sliderMax);
        presets_ = presets;
        strokeColor_ = strokeColor;
        if (!editing_) {
            editText_ = FormatBrushSizeValue(brushSize_);
        }
        update();
    }

    [[nodiscard]] bool SliderDragging() const { return dragging_; }
    [[nodiscard]] float BrushSize() const { return brushSize_; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QPointF origin = -QPointF(bounds_.topLeft());

        const QRectF panel = layout_.panelRect.translated(origin);
        const QRectF preview = layout_.previewRect.translated(origin);
        ChromePanel(painter, QRectF(panel).adjusted(-3.0, -3.0, 3.0, 3.0));
        ChromePanel(painter, panel);

        const QColor previewFill = tool_ == settings::Tool::Erase ? QColor(Qt::white) : ChromeColor(strokeColor_);
        const qreal diameter = PreviewDiameter(tool_, brushSize_, owner_->PageScale());
        const QPointF previewCenter = preview.center();
        painter.setBrush(previewFill);
        if (tool_ == settings::Tool::Erase) {
            painter.setPen(QPen(QColor(0, 0, 0), std::max(0.5, kEraserOutlinePixels)));
            painter.drawRect(QRectF(previewCenter.x() - diameter * 0.5, previewCenter.y() - diameter * 0.5, diameter, diameter));
        } else {
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(previewCenter, diameter * 0.5, diameter * 0.5);
        }

        const QRectF slider = layout_.sliderRect.translated(origin);
        const QRectF track = qt::ToolbarBrushTrackRect(layout_.sliderRect).translated(origin);
        ChromeFillRoundedRect(painter, track, ChromeColor(kChromeSliderTrack), 3.0);
        const qreal thumbX = qt::ToolbarBrushThumbCenterX(layout_.sliderRect, brushSize_, sliderMax_) + origin.x();
        QRectF fill = track;
        fill.setRight(std::max(track.left() + 1.0, thumbX + 1.0));
        ChromeFillRoundedRect(painter, fill, ChromeColor(kChromeAccent), 3.0);

        if (!presets_.empty()) {
            painter.setPen(QPen(QColor(228, 233, 240), 1.0));
            for (const float preset : presets_) {
                if (preset > sliderMax_) {
                    continue;
                }
                const qreal tickX = qt::ToolbarBrushThumbCenterX(layout_.sliderRect, preset, sliderMax_) + origin.x();
                painter.drawLine(QPointF(tickX, track.top() - 2.0), QPointF(tickX, track.bottom() + 2.0));
            }
        }

        const qreal half = kQuickSizeThumbHalfDip;
        const QRectF thumb(thumbX - half, slider.center().y() - half, half * 2.0 + 1.0, half * 2.0 + 1.0);
        ChromeFillRoundedRect(painter, thumb, ChromeColor(kChromeSliderThumb), 3.0);
        ChromeStrokeRoundedRect(painter, thumb, dragging_ ? ChromeColor(kChromeSliderThumbActiveBorder) : ChromeColor(kChromeSliderThumbBorder), 1.0, 3.0);

        const QRectF valueRect = layout_.valueRect.translated(origin);
        painter.setFont(ChromeFont(13));
        if (editing_) {
            ChromeStrokeRoundedRect(painter, valueRect, ChromeColor(kChromeAccent), 1.0, 3.0);
            const QRectF textRect = valueRect.adjusted(5.0, 0.0, -5.0, 0.0);
            painter.setPen(ChromeColor(kChromeSelectionBorder));
            painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, editText_);
            const qreal advance = QFontMetricsF(ChromeFont(13)).horizontalAdvance(editText_);
            const qreal caretX = std::min(textRect.right() - 1.0, textRect.left() + advance + 1.0);
            painter.drawLine(QPointF(caretX, valueRect.top() + 6.0), QPointF(caretX, valueRect.bottom() - 6.0));
        } else {
            painter.setPen(ChromeColor(kChromeTextPrimary));
            painter.drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter, FormatBrushSizeValue(brushSize_));
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        const QPointF local = event->position();
        const QPointF origin = -QPointF(bounds_.topLeft());
        const QRectF slider = layout_.sliderRect.translated(origin);
        const QRectF value = layout_.valueRect.translated(origin);
        if (event->button() != Qt::LeftButton) {
            return;
        }
        if (ContainsPoint(slider, local)) {
            CancelEdit();
            dragging_ = true;
            setFocus(Qt::MouseFocusReason);
            UpdateFromPointer(local.x());
            return;
        }
        if (ContainsPoint(value, local)) {
            editing_ = true;
            editText_ = FormatBrushSizeValue(brushSize_);
            setFocus(Qt::MouseFocusReason);
            update();
            return;
        }
        event->ignore();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_) {
            UpdateFromPointer(event->position().x());
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragging_ = false;
            owner_->OnQuickSizeChanged(tool_, brushSize_);
            update();
        }
    }

    bool event(QEvent* event) override {
        // The value editor is hand-drawn, not a real line edit, so it must
        // claim ShortcutOverride itself: otherwise window shortcuts (zoom
        // presets on 1-5, tool keys) swallow the digits it is editing.
        if (editing_ && event->type() == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            const QString text = key->text();
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
                || key->key() == Qt::Key_Escape || key->key() == Qt::Key_Backspace
                || (!text.isEmpty() && (text.at(0).isDigit() || text.at(0) == QLatin1Char('.')))) {
                event->accept();
                return true;
            }
        }
        return QWidget::event(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (!editing_) {
            QWidget::keyPressEvent(event);
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            CommitEdit();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            CancelEdit();
            return;
        }
        if (event->key() == Qt::Key_Backspace) {
            editText_.chop(1);
            update();
            return;
        }
        const QString text = event->text();
        if (!text.isEmpty() && (text.at(0).isDigit() || text.at(0) == QLatin1Char('.'))) {
            if (editText_ == FormatBrushSizeValue(brushSize_) && !editText_.contains(QLatin1Char('.'))) {
                editText_.clear();
            }
            editText_ += text;
            update();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void focusOutEvent(QFocusEvent* event) override {
        if (editing_) {
            CommitEdit();
        }
        QWidget::focusOutEvent(event);
    }

private:
    friend class ToolbarWidget;

    void UpdateFromPointer(qreal localX) {
        // localX is popup-local; the layout rects are host coordinates, so the
        // track is translated like paintEvent does before comparing.
        const QRectF track = qt::ToolbarBrushTrackRect(layout_.sliderRect).translated(-QPointF(bounds_.topLeft()));
        if (track.right() <= track.left()) {
            return;
        }
        const qreal clampedX = std::clamp(localX, track.left(), track.right() - 1.0);
        const qreal width = std::max(1.0, track.right() - track.left() - 1.0);
        const qreal normalized = (clampedX - track.left()) / width;
        const float size = kMinBrushSize + static_cast<float>(normalized) * (sliderMax_ - kMinBrushSize);
        brushSize_ = SnapQuickSizeValue(size, sliderMax_, presets_);
        editText_ = FormatBrushSizeValue(brushSize_);
        owner_->OnQuickSizeChanged(tool_, brushSize_);
        update();
    }

    void CommitEdit() {
        bool ok = false;
        const float parsed = editText_.toFloat(&ok);
        if (ok && parsed > 0.0f) {
            brushSize_ = SnapQuickSizeValue(parsed, sliderMax_, presets_);
            owner_->OnQuickSizeChanged(tool_, brushSize_);
        }
        CancelEdit();
    }

    void CancelEdit() {
        editing_ = false;
        editText_ = FormatBrushSizeValue(brushSize_);
        update();
    }

    ToolbarWidget* owner_ = nullptr;
    qt::QuickSizePopupLayout layout_ {};
    QRect bounds_ {};
    settings::Tool tool_ = settings::Tool::Draw;
    float brushSize_ = 3.0f;
    float sliderMax_ = 20.0f;
    std::vector<float> presets_ {};
    std::uint32_t strokeColor_ = 0xFF1F2937U;
    bool dragging_ = false;
    bool editing_ = false;
    QString editText_ {};
};

// ---------------------------------------------------------------------------
// Text-format popup (App::PaintTextFormatPopup / HandleTextFormatPopupClick)
// ---------------------------------------------------------------------------
class TextFormatPopup final : public QWidget {
public:
    TextFormatPopup(QWidget* host, ToolbarWidget* owner)
        : QWidget(host), owner_(owner) {
        setObjectName(QStringLiteral("cnoteTextFormatPopup"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
    }

    void Configure(const qt::TextFormatPopupLayout& layout, const QRect& bounds) {
        layout_ = layout;
        bounds_ = bounds;
        setGeometry(bounds);
        update();
    }

    void SetState(const std::optional<PageTextStyle>& style, TextAlignment alignment, std::size_t sizePreset) {
        style_ = style.value_or(PageTextStyle {});
        alignment_ = alignment;
        sizePreset_ = sizePreset;
        update();
    }

    void SetFontMenuOpen(bool open) {
        fontMenuOpen_ = open;
        update();
    }
    void SetSizeMenuOpen(bool open) {
        sizeMenuOpen_ = open;
        update();
    }

    [[nodiscard]] bool FontMenuOpen() const { return fontMenuOpen_; }
    [[nodiscard]] bool SizeMenuOpen() const { return sizeMenuOpen_; }
    [[nodiscard]] std::optional<TextFontFamily> HoveredFontOption() const { return hoveredFontOption_; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QPointF origin = -QPointF(bounds_.topLeft());
        ChromePanel(painter, layout_.panelRect.translated(origin));

        auto drawButton = [&](const QRectF& rect, const QString& iconName, const QString& fallback, bool active) {
            const QRectF local = rect.translated(origin);
            ChromeFillRoundedRect(painter, local, ChromeColor(active ? kChromeFormatAccentFill : kChromeFormatButtonFill));
            ChromeStrokeRoundedRect(painter, local, ChromeColor(active ? kChromeAccent : kChromeFormatButtonBorder), 1.0);
            PaintIconOrFallback(painter, local, iconName, fallback, ChromeColor(active ? kChromeTextPrimary : kChromeFormatTextMuted), 13);
        };

        drawButton(layout_.alignCycleRect, AlignmentIconName(alignment_), QStringLiteral("L"), true);
        drawButton(layout_.boldRect, QStringLiteral("text/bold.svg"), QStringLiteral("B"), style_.bold);
        drawButton(layout_.italicRect, QStringLiteral("text/italics.svg"), QStringLiteral("I"), style_.italic);
        drawButton(layout_.underlineRect, QStringLiteral("text/underline.svg"), QStringLiteral("U"), style_.underline);

        const QRectF sizeDropdown = layout_.sizeDropdownRect.translated(origin);
        ChromeFillRoundedRect(painter, sizeDropdown, ChromeColor(kChromeFormatButtonFill));
        ChromeStrokeRoundedRect(painter, sizeDropdown, ChromeColor(sizeMenuOpen_ ? kChromeAccent : kChromeFormatButtonBorder), 1.0);
        painter.setFont(ChromeFont(13));
        painter.setPen(ChromeColor(kChromeTextPrimary));
        painter.drawText(sizeDropdown.adjusted(10.0, 0.0, -10.0, 0.0), Qt::AlignLeft | Qt::AlignVCenter,
            TextSizePresetLabel(std::min(sizePreset_, sizeOptions_.size() - 1U)));

        const QRectF dropdown = layout_.fontDropdownRect.translated(origin);
        ChromeFillRoundedRect(painter, dropdown, ChromeColor(kChromeFormatButtonFill));
        ChromeStrokeRoundedRect(painter, dropdown, ChromeColor(fontMenuOpen_ ? kChromeAccent : kChromeFormatButtonBorder), 1.0);
        painter.setPen(ChromeColor(kChromeTextPrimary));
        painter.drawText(dropdown.adjusted(10.0, 0.0, -10.0, 0.0), Qt::AlignLeft | Qt::AlignVCenter, FontFamilyLabel(style_.fontFamily));

        if (fontMenuOpen_ && layout_.fontMenuRect.isValid()) {
            painter.setBrush(Qt::NoBrush);
            ChromePanel(painter, layout_.fontMenuRect.translated(origin));
            for (std::size_t index = 0; index < layout_.fontOptionRects.size(); ++index) {
                const auto family = static_cast<TextFontFamily>(index);
                const QRectF option = layout_.fontOptionRects[index].translated(origin);
                const bool active = style_.fontFamily == family;
                const bool hovered = hoveredFontOption_.has_value() && *hoveredFontOption_ == family;
                ChromeFillRoundedRect(painter, option, ChromeColor(active ? kChromeFormatAccentFill : (hovered ? kChromeButtonFillHover : kChromePanelFill)), 3.0);
                painter.setPen(ChromeColor(active ? kChromeTextPrimary : kChromeFormatTextMuted));
                painter.drawText(option.adjusted(8.0, 0.0, -8.0, 0.0), Qt::AlignLeft | Qt::AlignVCenter, FontFamilyLabel(family));
            }
        }

        if (sizeMenuOpen_ && layout_.sizeMenuRect.isValid()) {
            painter.setBrush(Qt::NoBrush);
            ChromePanel(painter, layout_.sizeMenuRect.translated(origin));
            for (std::size_t index = 0; index < layout_.sizeOptionRects.size(); ++index) {
                const QRectF option = layout_.sizeOptionRects[index].translated(origin);
                const bool active = sizePreset_ == index;
                const bool hovered = hoveredSizeOption_.has_value() && *hoveredSizeOption_ == index;
                ChromeFillRoundedRect(painter, option, ChromeColor(active ? kChromeFormatAccentFill : (hovered ? kChromeButtonFillHover : kChromePanelFill)), 3.0);
                painter.setPen(ChromeColor(active ? kChromeTextPrimary : kChromeFormatTextMuted));
                painter.drawText(option.adjusted(8.0, 0.0, -8.0, 0.0), Qt::AlignLeft | Qt::AlignVCenter, TextSizePresetLabel(index));
            }
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        const QPointF local = event->position();
        const QPointF origin = -QPointF(bounds_.topLeft());

        if (fontMenuOpen_) {
            for (std::size_t index = 0; index < layout_.fontOptionRects.size(); ++index) {
                if (ContainsPoint(layout_.fontOptionRects[index].translated(origin), local)) {
                    fontMenuOpen_ = false;
                    owner_->SetTextFormatFontMenuOpen(false);
                    owner_->OnTextFormatChanged();
                    emit owner_->TextFontFamilyChanged(static_cast<TextFontFamily>(index));
                    return;
                }
            }
        }
        if (sizeMenuOpen_) {
            for (std::size_t index = 0; index < layout_.sizeOptionRects.size(); ++index) {
                if (ContainsPoint(layout_.sizeOptionRects[index].translated(origin), local)) {
                    sizeMenuOpen_ = false;
                    owner_->SetTextFormatSizeMenuOpen(false);
                    owner_->OnTextFormatChanged();
                    emit owner_->TextSizePresetChanged(index);
                    return;
                }
            }
        }
        if (ContainsPoint(layout_.alignCycleRect.translated(origin), local)) {
            CloseMenus();
            emit owner_->TextAlignCycleRequested();
            return;
        }
        if (ContainsPoint(layout_.boldRect.translated(origin), local)) {
            owner_->ToggleTextFormatBold();
            return;
        }
        if (ContainsPoint(layout_.italicRect.translated(origin), local)) {
            owner_->ToggleTextFormatItalic();
            return;
        }
        if (ContainsPoint(layout_.underlineRect.translated(origin), local)) {
            owner_->ToggleTextFormatUnderline();
            return;
        }
        if (ContainsPoint(layout_.sizeDropdownRect.translated(origin), local)) {
            fontMenuOpen_ = false;
            sizeMenuOpen_ = !sizeMenuOpen_;
            owner_->SetTextFormatFontMenuOpen(false);
            owner_->SetTextFormatSizeMenuOpen(sizeMenuOpen_);
            update();
            return;
        }
        if (ContainsPoint(layout_.fontDropdownRect.translated(origin), local)) {
            sizeMenuOpen_ = false;
            fontMenuOpen_ = !fontMenuOpen_;
            owner_->SetTextFormatSizeMenuOpen(false);
            owner_->SetTextFormatFontMenuOpen(fontMenuOpen_);
            update();
            return;
        }
        event->ignore();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!fontMenuOpen_ && !sizeMenuOpen_) {
            if (hoveredFontOption_.has_value() || hoveredSizeOption_.has_value()) {
                hoveredFontOption_.reset();
                hoveredSizeOption_.reset();
                update();
            }
            return;
        }
        const QPointF local = event->position();
        const QPointF origin = -QPointF(bounds_.topLeft());
        std::optional<TextFontFamily> nextFont;
        std::optional<std::size_t> nextSize;
        if (fontMenuOpen_) {
            for (std::size_t index = 0; index < layout_.fontOptionRects.size(); ++index) {
                if (ContainsPoint(layout_.fontOptionRects[index].translated(origin), local)) {
                    nextFont = static_cast<TextFontFamily>(index);
                    break;
                }
            }
        }
        if (sizeMenuOpen_) {
            for (std::size_t index = 0; index < layout_.sizeOptionRects.size(); ++index) {
                if (ContainsPoint(layout_.sizeOptionRects[index].translated(origin), local)) {
                    nextSize = index;
                    break;
                }
            }
        }
        if (nextFont != hoveredFontOption_ || nextSize != hoveredSizeOption_) {
            hoveredFontOption_ = nextFont;
            hoveredSizeOption_ = nextSize;
            update();
        }
    }

private:
    void CloseMenus() {
        fontMenuOpen_ = false;
        sizeMenuOpen_ = false;
        owner_->SetTextFormatFontMenuOpen(false);
        owner_->SetTextFormatSizeMenuOpen(false);
    }

    // Preset names mirror the settings dialog's Small/Medium/Large row order.
    QString TextSizePresetLabel(std::size_t index) const {
        const QString name = index < sizeOptions_.size() ? sizeOptions_[index] : QStringLiteral("?");
        const std::size_t clamped = std::min(index, owner_->settings_.textSizePresets.size() - 1U);
        const float value = owner_->settings_.textSizePresets[clamped];
        return QStringLiteral("%1 (%2)").arg(name, FormatFontSizeValue(value));
    }

    ToolbarWidget* owner_ = nullptr;
    qt::TextFormatPopupLayout layout_ {};
    QRect bounds_ {};
    PageTextStyle style_ {};
    TextAlignment alignment_ = TextAlignment::Left;
    std::size_t sizePreset_ = 0;
    std::array<QString, 3> sizeOptions_ {
        QStringLiteral("Small"), QStringLiteral("Medium"), QStringLiteral("Large") };
    bool fontMenuOpen_ = false;
    bool sizeMenuOpen_ = false;
    std::optional<TextFontFamily> hoveredFontOption_ {};
    std::optional<std::size_t> hoveredSizeOption_ {};
};

// ---------------------------------------------------------------------------
// Palette swatch menu (App::PaintPaletteMenu / HandlePaletteClick)
// ---------------------------------------------------------------------------
class PaletteMenuPopup final : public QWidget {
public:
    PaletteMenuPopup(QWidget* host, ToolbarWidget* owner)
        : QWidget(host), owner_(owner) {
        setObjectName(QStringLiteral("cnotePaletteMenu"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
    }

    void Configure(const qt::PaletteMenuLayout& layout, const QRect& bounds) {
        layout_ = layout;
        bounds_ = bounds;
        setGeometry(bounds);
        update();
    }

    void SetState(const std::vector<std::uint32_t>& palette, std::size_t colorIndex) {
        palette_ = palette;
        colorIndex_ = colorIndex;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPointF origin = -QPointF(bounds_.topLeft());
        ChromePanel(painter, layout_.panelRect.translated(origin));
        for (std::size_t index = 0; index < layout_.swatchRects.size() && index < palette_.size(); ++index) {
            const QRectF swatch = layout_.swatchRects[index].translated(origin);
            ChromeFillRoundedRect(painter, swatch, ChromeColor(palette_[index]));
            ChromeStrokeRoundedRect(
                painter,
                swatch,
                ChromeColor(index == colorIndex_ ? kChromeSelectionBorder : kChromePanelStroke),
                index == colorIndex_ ? 2.0 : 1.0);
        }
        ChromeFillRoundedRect(painter, layout_.palettePickerRect.translated(origin), ChromeColor(kChromePalettePicker));
        ChromeStrokeRoundedRect(painter, layout_.palettePickerRect.translated(origin), ChromeColor(kChromePanelStroke), 1.0);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        const QPointF local = event->position();
        const QPointF origin = -QPointF(bounds_.topLeft());
        for (std::size_t index = 0; index < layout_.swatchRects.size(); ++index) {
            if (ContainsPoint(layout_.swatchRects[index].translated(origin), local)) {
                owner_->OnPaletteSelection(index);
                return;
            }
        }
        if (ContainsPoint(layout_.palettePickerRect.translated(origin), local)) {
            emit owner_->PalettePickerRequested();
            owner_->CloseOverlays();
            return;
        }
        if (ContainsPoint(QRectF(layout_.panelRect).translated(origin), local)) {
            return;  // consume presses inside the panel
        }
        event->ignore();
    }

private:
    ToolbarWidget* owner_ = nullptr;
    qt::PaletteMenuLayout layout_ {};
    QRect bounds_ {};
    std::vector<std::uint32_t> palette_ {};
    std::size_t colorIndex_ = 0;
};

// ---------------------------------------------------------------------------
// Palette / tool wheel (App::PaintPaletteWheel / HandlePaletteClick)
// ---------------------------------------------------------------------------
class PaletteWheelOverlay final : public QWidget {
public:
    PaletteWheelOverlay(QWidget* host, ToolbarWidget* owner)
        : QWidget(host), owner_(owner) {
        setObjectName(QStringLiteral("cnotePaletteWheel"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
    }

    void Configure(const QRect& hostClientRect) {
        setGeometry(hostClientRect);
        update();
    }

    void SetEntries(const std::vector<std::uint32_t>& palette, const std::vector<settings::Tool>& tools, std::size_t colorIndex, bool zoomPresetMode, const std::array<float, settings::kZoomPresetCount>& zoomPresets) {
        palette_ = palette;
        tools_ = tools;
        colorIndex_ = colorIndex;
        zoomPresetMode_ = zoomPresetMode;
        zoomPresets_ = zoomPresets;
        update();
    }

    [[nodiscard]] bool ZoomPresetMode() const { return zoomPresetMode_; }

    // Recomputes the hovered slice for a host-space position.  The owner seeds
    // it with the wheel anchor right after opening, mirroring the reference's
    // UpdatePaletteWheelHover(cursor) call (ToolbarState.cpp:147).
    void UpdateHover(const QPointF& position) {
        const std::size_t entryCount = EntryCount();
        std::optional<std::size_t> next;
        if (entryCount > 0U) {
            next = qt::PaletteWheelIndexAtPoint(BuildLayout(), position, entryCount);
        }
        if (next != owner_->wheelHoverIndex_) {
            owner_->wheelHoverIndex_ = next;
            update();
        }
    }

protected:
    // App::PaintPaletteWheel (App.cpp:6755-7044): halo, slice fills, separators,
    // the selected/hovered outline, tool icons or preset labels, then the centre
    // cut-out with the zoom magnifier.
    void paintEvent(QPaintEvent*) override {
        const std::size_t entries = EntryCount();
        if (entries == 0U) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const qt::PaletteWheelLayout layout = BuildLayout();
        const QPointF center = layout.center;
        const qreal inner = layout.innerRadius;
        const qreal outer = layout.outerRadius;
        const qreal dpr = devicePixelRatioF();

        const auto isHovered = [&](std::size_t index) {
            return owner_->wheelHoverIndex_.has_value() && *owner_->wheelHoverIndex_ == index;
        };
        const auto sliceOuter = [&](std::size_t index) {
            return isHovered(index) ? outer + qt::PaletteWheelHoverGrow() : outer;
        };
        // A hovered slice's icon/label slides outward and its centre radius
        // grows by half the hover grow (App.cpp:6839-6841, 6989-6993).
        const auto glyphCenter = [&](std::size_t index) {
            const qreal fraction = (static_cast<qreal>(index) + 0.5) / static_cast<qreal>(entries);
            const qreal radius = inner + ((outer - inner) * 0.5);
            if (!isHovered(index)) {
                return PointAt(center, radius, fraction);
            }
            const qreal angle = AngleAt(fraction);
            const qreal grown = radius + (qt::PaletteWheelHoverGrow() * 0.5) + qt::PaletteWheelHoverOffset();
            return QPointF(center.x() + (std::cos(angle) * grown), center.y() + (std::sin(angle) * grown));
        };

        // Halo behind the ring (App.cpp:6781-6790).
        painter.setPen(Qt::NoPen);
        painter.setBrush(ChromeColor(kWheelHalo));
        const qreal haloRadius = outer + kPaletteWheelShadowInsetDip;
        painter.drawEllipse(center, haloRadius, haloRadius);

        for (std::size_t index = 0; index < entries; ++index) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(FillColor(index));
            painter.drawPolygon(WedgePolygon(center, inner, sliceOuter(index), index, entries));
        }

        // Base separators: 120-alpha white outer arcs and radial dividers, both
        // skipped around the hovered slice, which paints its own outline
        // (App.cpp:6906-6938).
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ChromeColor(kWheelSeparatorStroke), kPaletteWheelStrokeDip));
        if (entries == 1U) {
            painter.drawEllipse(center, outer, outer);
        } else {
            for (std::size_t index = 0; index < entries; ++index) {
                const std::size_t previous = index == 0U ? entries - 1U : index - 1U;
                const qreal divider = static_cast<qreal>(index) / static_cast<qreal>(entries);
                if (!isHovered(index)) {
                    painter.drawPolyline(OuterArc(center, outer, index, entries));
                }
                if (!isHovered(index) && !isHovered(previous)) {
                    painter.drawLine(PointAt(center, inner, divider), PointAt(center, outer, divider));
                }
            }
        }

        // Selected/hovered slice outline (App.cpp:6873-6879).
        painter.setPen(QPen(ChromeColor(kWheelSelectedStroke), kPaletteWheelSelectedStrokeDip));
        for (std::size_t index = 0; index < entries; ++index) {
            if (isHovered(index) || IsSelectedEntry(index)) {
                painter.drawPolygon(WedgePolygon(center, inner, sliceOuter(index), index, entries));
            }
        }

        for (std::size_t index = 0; index < entries; ++index) {
            if (IsColorEntry(index)) {
                continue;
            }
            const QPointF glyph = glyphCenter(index);
            if (zoomPresetMode_) {
                float preset = 0.0f;
                if (!ZoomPresetValueAt(index, preset)) {
                    continue;
                }
                painter.setPen(ChromeColor(kChromeTextPrimary));
                painter.setFont(ChromeFont(13));
                const QString label = QString::number(static_cast<int>(std::lround(preset * 100.0))) + QStringLiteral("%");
                painter.drawText(QRectF(glyph.x() - 24.0, glyph.y() - 10.0, 48.0, 20.0), Qt::AlignCenter, label);
                continue;
            }
            const std::size_t toolIndex = index - palette_.size();
            if (toolIndex >= tools_.size()) {
                continue;
            }
            // Reference icon rect is PaletteWheelIconHalfSize each side of the
            // centre, tinted white while selected (App.cpp:6881-6897).
            const qreal half = qt::PaletteWheelIconHalfSize();
            const int extent = std::max(1, static_cast<int>(std::lround(half * 2.0)));
            const QPixmap icon = ChromeIconPixmap(ToolIconName(tools_[toolIndex]), QSize(extent, extent),
                ChromeColor(IsSelectedEntry(index) ? kWheelSelectedIconTint : kWheelToolIconTint), dpr);
            if (!icon.isNull()) {
                painter.drawPixmap(glyph - QPointF(half, half), icon);
            }
        }

        // Centre cut-out, magnifier and border (App.cpp:7019-7044).
        const bool zoomToggleEnabled = HasZoomEntry();
        const bool zoomCentre = zoomToggleEnabled && zoomPresetMode_;
        painter.setPen(Qt::NoPen);
        painter.setBrush(ChromeColor(zoomCentre ? kWheelZoomCenterFill : kWheelCenterFill));
        painter.drawEllipse(center, inner, inner);
        if (zoomToggleEnabled) {
            const int extent = std::max(1, static_cast<int>(std::lround(kPaletteWheelCenterIconHalfDip * 2.0)));
            const QPixmap icon = ChromeIconPixmap(ToolIconName(settings::Tool::Zoom), QSize(extent, extent),
                ChromeColor(zoomCentre ? kWheelSelectedIconTint : kWheelToolIconTint), dpr);
            if (!icon.isNull()) {
                painter.drawPixmap(QPointF(center.x() - kPaletteWheelCenterIconHalfDip, center.y() - kPaletteWheelCenterIconHalfDip), icon);
            }
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ChromeColor(zoomCentre ? kWheelZoomCenterBorder : kWheelCenterBorder), kPaletteWheelBorderDip));
        painter.drawEllipse(center, inner, inner);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        UpdateHover(event->position());
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        const qt::PaletteWheelLayout layout = BuildLayout();
        const QPointF delta = event->position() - layout.center;
        const bool centerHit = delta.manhattanLength() >= 0.0
            && (delta.x() * delta.x() + delta.y() * delta.y()) < layout.innerRadius * layout.innerRadius;
        if (centerHit && HasZoomEntry()) {
            zoomPresetMode_ = !zoomPresetMode_;
            owner_->OnWheelZoomModeChanged(zoomPresetMode_);
            update();
            return;
        }
        if (zoomPresetMode_) {
            return;  // preset selection commits on release
        }
        if (const auto index = qt::PaletteWheelIndexAtPoint(layout, event->position(), EntryCount()); index.has_value()) {
            if (*index < palette_.size()) {
                owner_->OnPaletteSelection(*index);
            } else {
                const std::size_t toolIndex = *index - palette_.size();
                if (toolIndex < tools_.size()) {
                    owner_->OnPaletteToolSelection(tools_[toolIndex]);
                }
            }
            return;
        }
        owner_->CloseOverlays();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!zoomPresetMode_ || event->button() != Qt::LeftButton) {
            return;
        }
        const qt::PaletteWheelLayout layout = BuildLayout();
        const std::size_t entryCount = EntryCount();
        if (entryCount == 0U) {
            return;
        }
        const QPointF delta = event->position() - layout.center;
        const bool centerHit = (delta.x() * delta.x() + delta.y() * delta.y()) < layout.innerRadius * layout.innerRadius;
        if (centerHit) {
            return;
        }
        const auto index = qt::PaletteWheelIndexAtPoint(layout, event->position(), entryCount);
        if (!index.has_value()) {
            return;
        }
        const std::vector<std::size_t> presets = ZoomPresetIndices();
        if (*index < presets.size()) {
            emit owner_->ZoomPresetRequested(zoomPresets_[presets[*index]]);
        }
        owner_->CloseOverlays();
    }

private:
    [[nodiscard]] bool HasZoomEntry() const {
        return std::find(owner_->settings_.wheelTools.begin(), owner_->settings_.wheelTools.end(), settings::Tool::Zoom)
            != owner_->settings_.wheelTools.end();
    }

    [[nodiscard]] std::vector<std::size_t> ZoomPresetIndices() const {
        std::vector<std::size_t> indices;
        for (std::size_t index = 0; index < zoomPresets_.size(); ++index) {
            if (zoomPresets_[index] > 0.0f) {
                indices.push_back(index);
            }
        }
        return indices;
    }

    [[nodiscard]] std::size_t EntryCount() const {
        if (zoomPresetMode_) {
            return ZoomPresetIndices().size();
        }
        return palette_.size() + tools_.size();
    }

    [[nodiscard]] qt::PaletteWheelLayout BuildLayout() const {
        return qt::BuildPaletteWheelLayout(QRectF(rect()), owner_->wheelCenter_, std::max<std::size_t>(1U, EntryCount()), qt::ToolbarHeight(owner_->maximized_));
    }

    // Point on the wheel at normalized angle `fraction` (0 == the reference
    // index-0 slice start): atan2 = fraction * 2pi - pi/2, mirroring
    // appui::PaletteWheelIndexAtPoint.
    static qreal AngleAt(qreal fraction) {
        return (fraction * 2.0 * kWheelPi) - (kWheelPi * 0.5);
    }

    static QPointF PointAt(const QPointF& center, qreal radius, qreal fraction) {
        const qreal angle = AngleAt(fraction);
        return QPointF(center.x() + std::cos(angle) * radius, center.y() + std::sin(angle) * radius);
    }

    static void AppendArcPoints(QPolygonF& polygon, const QPointF& center, qreal radius, qreal from, qreal to) {
        constexpr int kSamples = 16;
        for (int sample = 0; sample <= kSamples; ++sample) {
            polygon << PointAt(center, radius, from + ((to - from) * static_cast<qreal>(sample) / static_cast<qreal>(kSamples)));
        }
    }

    static QPolygonF OuterArc(const QPointF& center, qreal radius, std::size_t index, std::size_t count) {
        QPolygonF arc;
        AppendArcPoints(arc, center, radius,
            static_cast<qreal>(index) / static_cast<qreal>(count),
            static_cast<qreal>(index + 1U) / static_cast<qreal>(count));
        return arc;
    }

    static QPolygonF WedgePolygon(const QPointF& center, qreal inner, qreal outer, std::size_t index, std::size_t count) {
        const qreal start = static_cast<qreal>(index) / static_cast<qreal>(count);
        const qreal end = static_cast<qreal>(index + 1U) / static_cast<qreal>(count);
        QPolygonF polygon;
        AppendArcPoints(polygon, center, outer, start, end);
        AppendArcPoints(polygon, center, inner, end, start);
        return polygon;
    }

    // Reference isColorEntry / selected (App.cpp:6847-6856).
    [[nodiscard]] bool IsColorEntry(std::size_t index) const {
        return !zoomPresetMode_ && index < palette_.size();
    }

    [[nodiscard]] bool IsCurrentZoomPreset(float preset) const {
        return preset > 0.0f && std::abs(preset - owner_->pageScale_) < 0.001;  // App.cpp:6795-6798
    }

    // The zoom-mode entry `index` is the index-th non-zero preset
    // (App::PaletteWheelZoomPresetIndices, ToolbarState.cpp:116-126).
    [[nodiscard]] bool ZoomPresetValueAt(std::size_t index, float& value) const {
        std::size_t seen = 0;
        for (const float preset : zoomPresets_) {
            if (preset <= 0.0f) {
                continue;
            }
            if (seen == index) {
                value = preset;
                return true;
            }
            ++seen;
        }
        return false;
    }

    [[nodiscard]] bool IsSelectedEntry(std::size_t index) const {
        if (zoomPresetMode_) {
            float preset = 0.0f;
            return ZoomPresetValueAt(index, preset) && IsCurrentZoomPreset(preset);
        }
        if (index < palette_.size()) {
            return index == colorIndex_;
        }
        const std::size_t toolIndex = index - palette_.size();
        return toolIndex < tools_.size()
            && tools_[toolIndex] != settings::Tool::Zoom  // isWheelToolSelected excludes Zoom
            && tools_[toolIndex] == owner_->activeTool_;
    }

    [[nodiscard]] QColor FillColor(std::size_t index) const {
        if (IsColorEntry(index)) {
            return ChromeColor(palette_[index]);
        }
        return ChromeColor(IsSelectedEntry(index) ? kWheelSelectedFill : kWheelUnselectedFill);
    }

    ToolbarWidget* owner_ = nullptr;
    std::vector<std::uint32_t> palette_ {};
    std::vector<settings::Tool> tools_ {};
    std::size_t colorIndex_ = 0;
    bool zoomPresetMode_ = false;
    std::array<float, settings::kZoomPresetCount> zoomPresets_ {};
};

// ---------------------------------------------------------------------------
// ToolbarWidget
// ---------------------------------------------------------------------------
ToolbarWidget::ToolbarWidget(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("cnoteToolbar"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAutoFillBackground(false);
    settings_.autoHideToolbar = false;

    revealTimer_ = new QTimer(this);
    revealTimer_->setTimerType(Qt::PreciseTimer);
    revealTimer_->setInterval(8);
    connect(revealTimer_, &QTimer::timeout, this, &ToolbarWidget::OnRevealTick);

    UpdateToolbarGeometry();

    // One application-scoped filter tracks host resize/mouse-move (auto-hide)
    // and dismisses open overlays on the first click outside them, matching the
    // reference WM_LBUTTONDOWN overlay order in App::HandlePaletteClick /
    // HandleQuickSizePopupClick / HandleTextFormatPopupClick.
    if (QApplication::instance() != nullptr) {
        QApplication::instance()->installEventFilter(this);
    }
}

ToolbarWidget::~ToolbarWidget() {
    if (QApplication::instance() != nullptr) {
        QApplication::instance()->removeEventFilter(this);
    }
}

void ToolbarWidget::SetSettings(const settings::Settings& settings) {
    const bool wasAutoHide = settings_.autoHideToolbar;
    settings_ = settings;
    // A wheel-tool toggle while the wheel is open re-slices it (the reference
    // repaints from wheel_.tools on the next frame, App.cpp:6755-6760).
    if (paletteWheel_ != nullptr && paletteWheel_->isVisible()) {
        paletteWheel_->SetEntries(settings_.palette, PaletteWheelDisplayedTools(), settings_.colorIndex, paletteWheel_->ZoomPresetMode(), settings_.zoomPresets);
    }
    if (!settings_.autoHideToolbar) {
        revealProgress_ = 1.0;
        revealed_ = true;
    } else if (!wasAutoHide) {
        // Auto-hide just turned on (including the first settings load): the
        // band parks above the host.  SetAutoHideEnabled early-returns once
        // the flag already matches, so without this the band would start
        // revealed and slide away on the first mouse move.
        revealProgress_ = 0.0;
        revealed_ = false;
    }
    UpdateToolbarGeometry();
    update();
}

bool ToolbarWidget::IsToolInWheel(settings::Tool tool) const {
    return std::find(settings_.wheelTools.begin(), settings_.wheelTools.end(), tool) != settings_.wheelTools.end();
}

bool ToolbarWidget::ShowWheelToolMenu(const QPointF& widgetPosition, const QPoint& globalPosition) {
    // Reference App::HandleToolbarRightClick (App.cpp:6195-6239): the zoom
    // indicator is probed first, then the six tool buttons in the reference
    // order; a hit shows the single-item "Add/Remove from Quick Wheel" menu.
    std::optional<settings::Tool> tool;
    if (ContainsPoint(ComputeTitleLayout().zoomRect, widgetPosition)) {
        tool = settings::Tool::Zoom;
    } else {
        const struct {
            ToolbarControl control;
            settings::Tool tool;
        } toolButtons[] = {
            { ToolbarControl::SelectTool, settings::Tool::Select },
            { ToolbarControl::TextTool, settings::Tool::Text },
            { ToolbarControl::MoveTool, settings::Tool::Move },
            { ToolbarControl::DrawTool, settings::Tool::Draw },
            { ToolbarControl::PanTool, settings::Tool::Pan },
            { ToolbarControl::EraseTool, settings::Tool::Erase },
        };
        for (const auto& button : toolButtons) {
            if (ContainsPoint(ControlRect(button.control), widgetPosition)) {
                tool = button.tool;
                break;
            }
        }
    }
    if (!tool.has_value()) {
        return false;
    }

    QMenu menu(this);
    QAction* toggle = menu.addAction(IsToolInWheel(*tool)
        ? QStringLiteral("Remove from Quick Wheel")
        : QStringLiteral("Add to Quick Wheel"));
    if (menu.exec(globalPosition) == toggle) {
        emit WheelToolToggled(*tool);
    }
    return true;
}

void ToolbarWidget::SetActiveTool(settings::Tool tool) {
    if (activeTool_ == tool) {
        return;
    }
    activeTool_ = tool;
    update();
}

void ToolbarWidget::SetUndoRedoEnabled(bool canUndo, bool canRedo) {
    if (canUndo_ == canUndo && canRedo_ == canRedo) {
        return;
    }
    canUndo_ = canUndo;
    canRedo_ = canRedo;
    update();
}

void ToolbarWidget::SetTitle(const QString& title) {
    if (title_ == title) {
        return;
    }
    title_ = title;
    update();
}

void ToolbarWidget::SetZoomLabel(const QString& label) {
    if (zoomLabel_ == label) {
        return;
    }
    zoomLabel_ = label;
    update();
}

void ToolbarWidget::SetMaximized(bool maximized) {
    if (maximized_ == maximized) {
        return;
    }
    maximized_ = maximized;
    UpdateToolbarGeometry();
    update();
}

void ToolbarWidget::SetPageScale(qreal scale) {
    pageScale_ = scale > 0.0 ? scale : 1.0;
}

void ToolbarWidget::SetCaptionButtonsVisible(bool visible) {
    if (captionButtonsVisible_ == visible) {
        return;
    }
    captionButtonsVisible_ = visible;
    update();
}

void ToolbarWidget::SetFrameThicknessX(qreal thickness) {
    const qreal clamped = thickness > 0.0 ? thickness : 0.0;
    if (qFuzzyCompare(frameThicknessX_ + 1.0, clamped + 1.0)) {
        return;
    }
    frameThicknessX_ = clamped;
    UpdateToolbarGeometry();
    update();
}

void ToolbarWidget::SetTextFormatAvailable(bool available) {
    textFormatAvailable_ = available;
    if (!available) {
        HideTextFormatPopup();
    }
}

void ToolbarWidget::SetTextFormatState(const std::optional<PageTextStyle>& style, TextAlignment alignment, std::size_t sizePreset) {
    formatStyle_ = style;
    formatAlignment_ = alignment;
    formatSizePreset_ = sizePreset;
    if (textFormatPopup_ != nullptr) {
        textFormatPopup_->SetState(style, alignment, sizePreset);
    }
    update();
}

appui::qt::ToolbarLayout ToolbarWidget::ComputeLayout() const {
    const QRectF clientRect(0.0, 0.0, static_cast<qreal>(width()), static_cast<qreal>(qt::ToolbarHeight(maximized_)));
    return qt::BuildToolbarLayout(clientRect, settings_.palette.size(), maximized_, static_cast<int>(activeTool_), frameThicknessX_);
}

appui::qt::ToolbarTitleLayout ToolbarWidget::ComputeTitleLayout() const {
    return qt::ResolveToolbarTitleLayout(ComputeLayout(), ChromeFont(15), title_, zoomLabel_);
}

QRectF ToolbarWidget::ControlRect(ToolbarControl control) const {
    const qt::ToolbarLayout layout = ComputeLayout();
    switch (control) {
    case ToolbarControl::TitleMenu:
        return ComputeTitleLayout().fileTitleRect;
    case ToolbarControl::Zoom:
        return ComputeTitleLayout().zoomRect;
    case ToolbarControl::Minimize:
        return layout.minimizeRect;
    case ToolbarControl::Maximize:
        return layout.maximizeRect;
    case ToolbarControl::Close:
        return layout.closeRect;
    case ToolbarControl::Undo:
        return layout.undoRect;
    case ToolbarControl::Redo:
        return layout.redoRect;
    case ToolbarControl::SelectTool:
        return layout.selectToolRect;
    case ToolbarControl::MoveTool:
        return layout.moveToolRect;
    case ToolbarControl::TextTool:
        return layout.textToolRect;
    case ToolbarControl::DrawTool:
        return layout.drawToolRect;
    case ToolbarControl::PanTool:
        return layout.panToolRect;
    case ToolbarControl::EraseTool:
        return layout.eraseToolRect;
    case ToolbarControl::CurrentColor:
        return layout.currentColorRect;
    case ToolbarControl::None:
        break;
    }
    return {};
}

ToolbarControl ToolbarWidget::ControlAt(const QPointF& widgetPosition) const {
    static constexpr ToolbarControl kControls[] = {
        ToolbarControl::TitleMenu,
        ToolbarControl::Zoom,
        ToolbarControl::Minimize,
        ToolbarControl::Maximize,
        ToolbarControl::Close,
        ToolbarControl::Undo,
        ToolbarControl::Redo,
        ToolbarControl::SelectTool,
        ToolbarControl::MoveTool,
        ToolbarControl::TextTool,
        ToolbarControl::DrawTool,
        ToolbarControl::PanTool,
        ToolbarControl::EraseTool,
        ToolbarControl::CurrentColor,
    };
    for (const ToolbarControl control : kControls) {
        if (!captionButtonsVisible_ && (control == ToolbarControl::Minimize || control == ToolbarControl::Maximize || control == ToolbarControl::Close)) {
            continue;
        }
        if (control == ToolbarControl::Undo && !canUndo_) {
            continue;
        }
        if (control == ToolbarControl::Redo && !canRedo_) {
            continue;
        }
        if (ContainsPoint(ControlRect(control), widgetPosition)) {
            return control;
        }
    }
    return ToolbarControl::None;
}

ToolbarControl ToolbarWidget::ControlAtHostPosition(const QPointF& hostPosition) const {
    return ControlAt(hostPosition - QPointF(x(), y()));
}

void ToolbarWidget::UpdateHoverAtHostPosition(const QPointF& hostPosition) {
    const QPointF widgetPosition = hostPosition - QPointF(x(), y());
    const bool insideBand = widgetPosition.y() >= 0.0 && widgetPosition.y() < qt::ToolbarHeight(maximized_);
    const ToolbarControl control = insideBand ? ControlAt(widgetPosition) : ToolbarControl::None;
    if (control != hovered_) {
        hovered_ = control;
        update();
    }
}

void ToolbarWidget::SetPressedControl(ToolbarControl control) {
    if (pressed_ == control) {
        return;
    }
    pressed_ = control;
    update();
}

std::optional<settings::Tool> ToolbarWidget::ToolForControl(ToolbarControl control) {
    switch (control) {
    case ToolbarControl::SelectTool:
        return settings::Tool::Select;
    case ToolbarControl::MoveTool:
        return settings::Tool::Move;
    case ToolbarControl::TextTool:
        return settings::Tool::Text;
    case ToolbarControl::DrawTool:
        return settings::Tool::Draw;
    case ToolbarControl::PanTool:
        return settings::Tool::Pan;
    case ToolbarControl::EraseTool:
        return settings::Tool::Erase;
    default:
        return std::nullopt;
    }
}

ToolbarControl ToolbarWidget::ControlForTool(settings::Tool tool) {
    switch (tool) {
    case settings::Tool::Select:
        return ToolbarControl::SelectTool;
    case settings::Tool::Move:
        return ToolbarControl::MoveTool;
    case settings::Tool::Text:
    case settings::Tool::Latex:
        return ToolbarControl::TextTool;
    case settings::Tool::Draw:
        return ToolbarControl::DrawTool;
    case settings::Tool::Pan:
        return ToolbarControl::PanTool;
    case settings::Tool::Erase:
        return ToolbarControl::EraseTool;
    case settings::Tool::Zoom:
        return ToolbarControl::Zoom;
    }
    return ToolbarControl::None;
}

QSize ToolbarWidget::sizeHint() const {
    return QSize(640, static_cast<int>(std::lround(qt::ToolbarHeight(maximized_))));
}

void ToolbarWidget::UpdateToolbarGeometry() {
    QWidget* host = window();
    const int hostWidth = host != nullptr ? host->width() : width();
    const int toolbarHeight = static_cast<int>(std::lround(qt::ToolbarHeight(maximized_)));
    if (toolbarHeight <= 0) {
        return;
    }
    const int offsetY = settings_.autoHideToolbar
        ? static_cast<int>(std::lround(ToolbarOffsetY()))
        : 0;
    const QRect target(0, offsetY, hostWidth, toolbarHeight);
    if (geometry() != target) {
        setGeometry(target);
        emit ToolbarGeometryChanged();
    }
    PositionOverlays();
}

qreal ToolbarWidget::ToolbarOffsetY() const {
    const qreal toolbarHeight = qt::ToolbarHeight(maximized_);
    return -(toolbarHeight + 2.0) * (1.0 - std::clamp(revealProgress_, 0.0, 1.0));
}

qreal ToolbarWidget::VisibleToolbarHeight() const {
    if (!settings_.autoHideToolbar) {
        return qt::ToolbarHeight(maximized_);
    }
    // App::IsToolbarVisible (App.cpp:2297-2310): the band stays up while the
    // reveal animation runs, while it is revealed, or while an inline editor or
    // an open overlay owns it.
    const bool visible = revealProgress_ > 0.001
        || revealed_
        || zoomEditing_
        || QuickSizeSliderDragging()
        || AnyOverlayVisible();
    return visible ? qt::ToolbarHeight(maximized_) : 0.0;
}

void ToolbarWidget::SetAutoHideEnabled(bool enabled) {
    if (settings_.autoHideToolbar == enabled) {
        return;
    }
    settings_.autoHideToolbar = enabled;
    if (enabled) {
        revealProgress_ = 0.0;
        revealed_ = false;
    } else {
        revealProgress_ = 1.0;
        revealed_ = true;
    }
    UpdateToolbarGeometry();
    emit RevealStateChanged(revealed_);
    emit RevealProgressChanged(revealProgress_);
    update();
}

void ToolbarWidget::SetRevealed(bool revealed) {
    if (!settings_.autoHideToolbar) {
        revealed = true;
    }
    if (revealed_ == revealed) {
        return;
    }
    revealed_ = revealed;
    emit RevealStateChanged(revealed_);
    StartRevealAnimation(revealed ? 1.0 : 0.0);
}

void ToolbarWidget::UpdateAutoHideForCursor(const QPointF& hostPosition, bool interactionActive) {
    if (!settings_.autoHideToolbar) {
        return;
    }
    // The reference guard list (App.cpp:2444-2464) also covers its popups:
    // the palette menu, zoom/brush editing and slider drags all keep the band
    // up.  The Qt overlays are real widgets, so an open wheel/popup covering
    // the band would otherwise deliver a Leave that hides it mid-gesture.
    const qt::ToolbarAutoHide decision = qt::ResolveToolbarAutoHide(
        hostPosition.y(), maximized_, revealed_,
        interactionActive || AnyOverlayVisible() || zoomEditing_, frameThicknessX_);
    if (decision == qt::ToolbarAutoHide::Reveal) {
        SetRevealed(true);
    } else if (decision == qt::ToolbarAutoHide::Hide) {
        SetRevealed(false);
    }
}

void ToolbarWidget::StartRevealAnimation(qreal target) {
    if (!settings_.autoHideToolbar) {
        revealProgress_ = std::clamp(target, 0.0, 1.0);
        UpdateToolbarGeometry();
        return;
    }
    // Elapsed-time driven (plan step 5.3): capture the current progress and ease
    // toward the target over the reference 120 ms, never blocking.
    if (revealTimer_->isActive()) {
        OnRevealTick();
    }
    revealFrom_ = revealProgress_;
    revealTo_ = std::clamp(target, 0.0, 1.0);
    if (std::abs(revealFrom_ - revealTo_) <= 0.001) {
        revealProgress_ = revealTo_;
        revealTimer_->stop();
        UpdateToolbarGeometry();
        emit RevealProgressChanged(revealProgress_);
        update();
        return;
    }
    revealClock_.start();
    revealTimer_->start();
}

void ToolbarWidget::OnRevealTick() {
    const double elapsed = static_cast<double>(revealClock_.elapsed());
    const qreal linear = std::clamp(elapsed / 120.0, 0.0, 1.0);
    const qreal eased = linear * linear * (3.0 - (2.0 * linear));  // smoothstep, App.cpp:2501
    revealProgress_ = revealFrom_ + ((revealTo_ - revealFrom_) * eased);
    if (linear >= 1.0) {
        revealProgress_ = revealTo_;
        revealTimer_->stop();
    }
    UpdateToolbarGeometry();
    emit RevealProgressChanged(revealProgress_);
    update();
}

void ToolbarWidget::paintEvent(QPaintEvent*) {
    if (settings_.autoHideToolbar && revealProgress_ <= 0.001) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qreal toolbarHeight = qt::ToolbarHeight(maximized_);
    const QRectF toolbarRect(0.0, 0.0, static_cast<qreal>(width()), toolbarHeight);
    painter.fillRect(toolbarRect, ChromeColor(kChromeToolbarBackground));
    // App::PaintToolbar clips the surface to ToolbarVisibleClipHeight
    // (App.cpp:6487, 2354-2357): the last two logical pixels of the band show
    // the window background, and the canvas starts at the full band height.
    constexpr qreal kToolbarEdgeGuard = 2.0;
    if (toolbarHeight > kToolbarEdgeGuard) {
        painter.fillRect(
            QRectF(0.0, toolbarHeight - kToolbarEdgeGuard, static_cast<qreal>(width()), kToolbarEdgeGuard),
            ChromeColor(kChromeWindowBackground));
    }

    const qt::ToolbarLayout layout = ComputeLayout();
    const qt::ToolbarTitleLayout titleLayout = ComputeTitleLayout();

    auto paintIconButton = [&](const QRectF& rect, settings::Tool tool, bool selected, bool enabled = true) {
        const QColor tint = !enabled ? ChromeColor(kChromeIconDisabled)
            : (selected ? ChromeColor(kChromeIconSelected) : ChromeColor(kChromeTextPrimary));
        PaintIconOrFallback(painter, rect, ToolIconName(tool), ToolFallbackLabel(tool), tint, 15);
    };

    auto paintHover = [&](const QRectF& rect, ToolbarControl control) {
        if (hovered_ == control && rect.isValid()) {
            ChromeFillRoundedRect(painter, rect.adjusted(1.0, 1.0, -1.0, -1.0), ChromeColor(kChromeButtonFillHover), 4.0);
        }
    };

    if (captionButtonsVisible_) {
        // OS-native caption styling (ChromeStyleQt.h PaintCaptionButton):
        // Segoe Fluent/MDL2 glyphs, the WinUI 3 caption fills and the system
        // close red; the shell's hit test answers the real HT*BUTTON codes so
        // Windows drives the press cycle (and the Win11 snap flyout) while the
        // pressed state mirrors through SetPressedControl.
        const bool windowActive = window() == nullptr || window()->isActiveWindow();
        const auto paintWindowButton = [&](const QRectF& rect, CaptionGlyph glyph, ToolbarControl control) {
            PaintCaptionButton(painter, rect, glyph,
                hovered_ == control, pressed_ == control, windowActive);
        };
        paintWindowButton(layout.minimizeRect, CaptionGlyph::Minimize, ToolbarControl::Minimize);
        paintWindowButton(layout.maximizeRect,
            maximized_ ? CaptionGlyph::Restore : CaptionGlyph::Maximize, ToolbarControl::Maximize);
        paintWindowButton(layout.closeRect, CaptionGlyph::Close, ToolbarControl::Close);
    }

    if (layout.undoRect.isValid()) {
        paintHover(layout.undoRect, ToolbarControl::Undo);
        PaintIconOrFallback(painter, layout.undoRect, QStringLiteral("undo.svg"), QStringLiteral("Undo"),
            canUndo_ ? ChromeColor(kChromeTextPrimary) : ChromeColor(kChromeIconDisabled), 15);
    }
    if (layout.redoRect.isValid()) {
        paintHover(layout.redoRect, ToolbarControl::Redo);
        PaintIconOrFallback(painter, layout.redoRect, QStringLiteral("redo.svg"), QStringLiteral("Redo"),
            canRedo_ ? ChromeColor(kChromeTextPrimary) : ChromeColor(kChromeIconDisabled), 15);
    }

    const struct {
        QRectF rect;
        settings::Tool tool;
    } toolButtons[] = {
        { layout.selectToolRect, settings::Tool::Select },
        { layout.textToolRect, settings::Tool::Text },
        { layout.moveToolRect, settings::Tool::Move },
        { layout.eraseToolRect, settings::Tool::Erase },
        { layout.drawToolRect, settings::Tool::Draw },
        { layout.panToolRect, settings::Tool::Pan },
    };
    for (const auto& button : toolButtons) {
        const ToolbarControl control = ControlForTool(button.tool);
        paintHover(button.rect, control);
        paintIconButton(button.rect, button.tool, activeTool_ == button.tool);
        if (std::find(settings_.wheelTools.begin(), settings_.wheelTools.end(), button.tool) != settings_.wheelTools.end()) {
            const QRectF badge(
                button.rect.right() - kWheelBadgeInsetRightDip - kWheelBadgeSizeDip,
                button.rect.top() + kWheelBadgeTopDip,
                static_cast<qreal>(kWheelBadgeSizeDip),
                static_cast<qreal>(kWheelBadgeSizeDip));
            ChromeFillRoundedRect(painter, badge, ChromeColor(kChromeWheelBadge), 1.0);
        }
    }

    if (titleLayout.fileTitleRect.isValid()) {
        painter.setFont(ChromeFont(15));
        painter.setPen(hovered_ == ToolbarControl::TitleMenu ? ChromeColor(kChromeAccent) : ChromeColor(kChromeTextMuted));
        painter.drawText(titleLayout.titleTextRect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetricsF(ChromeFont(15)).elidedText(title_, Qt::ElideRight, titleLayout.titleTextRect.width()));
    }

    if (titleLayout.fileTitleRect.isValid() && titleLayout.zoomRect.isValid()) {
        painter.setPen(QPen(ChromeColor(kChromePanelBorder), 1.0));
        painter.drawLine(QPointF(titleLayout.dividerX, titleLayout.fileTitleRect.top() + 11.0), QPointF(titleLayout.dividerX, titleLayout.fileTitleRect.bottom() - 11.0));
    }

    if (titleLayout.zoomRect.isValid()) {
        painter.setFont(ChromeFont(15));
        if (hovered_ == ToolbarControl::Zoom) {
            ChromeFillRoundedRect(painter, titleLayout.zoomRect.adjusted(1.0, 1.0, -1.0, -1.0), ChromeColor(kChromeButtonFillHover), 4.0);
        }
        if (zoomEditing_) {
            const QRectF entry = titleLayout.zoomRect.adjusted(0.0, 7.0, 0.0, -7.0);
            ChromeStrokeRoundedRect(painter, entry, ChromeColor(kChromeAccent), 1.0, 4.0);
            painter.setPen(ChromeColor(kChromeSelectionBorder));
            const QRectF textRect = entry.adjusted(6.0, 0.0, -6.0, 0.0);
            painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, zoomEditText_);
            const qreal advance = QFontMetricsF(ChromeFont(15)).horizontalAdvance(zoomEditText_);
            const qreal caretX = std::min(textRect.right() - 1.0, textRect.left() + advance + 1.0);
            painter.drawLine(QPointF(caretX, entry.top() + 4.0), QPointF(caretX, entry.bottom() - 4.0));
        } else {
            painter.setPen(ChromeColor(kChromeTextMuted));
            painter.drawText(titleLayout.zoomTextRect, Qt::AlignLeft | Qt::AlignVCenter, zoomLabel_);
        }
    }

    ChromeFillRoundedRect(painter, layout.currentColorRect, ChromeColor(settings_.palette.empty() ? 0xFFFFFFFFU : settings_.palette[settings_.colorIndex % settings_.palette.size()]), 4.0);
    ChromeStrokeRoundedRect(painter, layout.currentColorRect,
        ChromeColor(PaletteMenuVisible() ? kChromeSelectionBorder : kChromePanelBorder),
        PaletteMenuVisible() ? 2.0 : 1.0, 4.0);
    if (hovered_ == ToolbarControl::CurrentColor) {
        ChromeStrokeRoundedRect(painter, layout.currentColorRect.adjusted(-1.0, -1.0, 1.0, 1.0), ChromeColor(kChromeSelectionBorder), 1.0, 4.0);
    }
    painter.setBrush(Qt::NoBrush);
}

void ToolbarWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    PositionOverlays();
    emit ToolbarGeometryChanged();
}

void ToolbarWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        // App::HandleToolbarRightClick (MessageLoop.cpp:586-594): tool buttons
        // and the zoom indicator offer the Quick Wheel membership menu.
        if (ShowWheelToolMenu(event->position(), event->globalPosition().toPoint())) {
            return;
        }
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    EnsureOverlays();
    const ToolbarControl control = ControlAt(event->position());
    if (control == ToolbarControl::None) {
        ZoomCommitIfEditing();
        event->ignore();
        return;
    }

    if (control == ToolbarControl::Zoom) {
        HidePaletteMenu();
        HideQuickSizePopup();
        BeginZoomEdit();
        return;
    }
    ZoomCommitIfEditing();

    if (const auto tool = ToolForControl(control); tool.has_value()) {
        SetActiveTool(*tool);
        emit ToolSelected(*tool);
        return;
    }

    // The in-client caption cluster uses the native press cycle: the button
    // arms here, fires on a release that still lands on it, and cancels when
    // the cursor slides off.  On Windows the same transition normally arrives
    // as WM_NCLBUTTONDOWN/UP (the hit test answers HT*BUTTON); the widget path
    // only runs for synthetic/touch delivery.
    if (control == ToolbarControl::Minimize || control == ToolbarControl::Maximize || control == ToolbarControl::Close) {
        SetPressedControl(control);
        return;
    }

    switch (control) {
    case ToolbarControl::TitleMenu:
        emit CommandTriggered(ToolbarCommand::OpenTitleMenu);
        break;
    case ToolbarControl::Undo:
        emit CommandTriggered(ToolbarCommand::Undo);
        break;
    case ToolbarControl::Redo:
        emit CommandTriggered(ToolbarCommand::Redo);
        break;
    case ToolbarControl::CurrentColor:
        TogglePaletteMenu();
        break;
    default:
        break;
    }
}

void ToolbarWidget::mouseMoveEvent(QMouseEvent* event) {
    const ToolbarControl control = ControlAt(event->position());
    if (control != hovered_) {
        hovered_ = control;
        update();
    }
    UpdateAutoHideForCursor(event->position(), false);
}

void ToolbarWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (pressed_ != ToolbarControl::None) {
        const ToolbarControl armed = pressed_;
        SetPressedControl(ToolbarControl::None);
        if (event->button() == Qt::LeftButton && ControlAt(event->position()) == armed) {
            switch (armed) {
            case ToolbarControl::Minimize: emit CommandTriggered(ToolbarCommand::Minimize); break;
            case ToolbarControl::Maximize: emit CommandTriggered(ToolbarCommand::Maximize); break;
            case ToolbarControl::Close: emit CommandTriggered(ToolbarCommand::Close); break;
            default: break;
            }
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void ToolbarWidget::leaveEvent(QEvent* event) {
    if (hovered_ != ToolbarControl::None) {
        hovered_ = ToolbarControl::None;
        update();
    }
    if (pressed_ != ToolbarControl::None) {
        pressed_ = ToolbarControl::None;
        update();
    }
    UpdateAutoHideForCursor(QPointF(-1.0, -1.0), false);
    QWidget::leaveEvent(event);
}

void ToolbarWidget::keyPressEvent(QKeyEvent* event) {
    if (!zoomEditing_) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        CommitZoomEdit();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        CancelZoomEdit();
        return;
    }
    if (event->key() == Qt::Key_Backspace) {
        zoomEditText_.chop(1);
        update();
        return;
    }
    const QString text = event->text();
    if (!text.isEmpty() && (text.at(0).isDigit() || text.at(0) == QLatin1Char('%') || text.at(0) == QLatin1Char('.'))) {
        if (text.at(0) != QLatin1Char('%')) {
            zoomEditText_ += text;
        }
        update();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ToolbarWidget::BeginZoomEdit() {
    zoomEditing_ = true;
    zoomEditText_ = zoomLabel_;
    setFocus(Qt::MouseFocusReason);
    update();
}

void ToolbarWidget::CancelZoomEdit() {
    zoomEditing_ = false;
    update();
}

void ToolbarWidget::CommitZoomEdit() {
    if (!zoomEditing_) {
        return;
    }
    const QString text = zoomEditText_.remove(QLatin1Char('%')).trimmed();
    zoomEditing_ = false;
    update();
    if (!text.isEmpty()) {
        emit ZoomTextCommitted(text);
    }
}

void ToolbarWidget::ZoomCommitIfEditing() {
    if (zoomEditing_) {
        CommitZoomEdit();
    }
}

void ToolbarWidget::EnsureOverlays() {
    QWidget* host = window();
    if (host == nullptr) {
        return;
    }
    if (quickSizePopup_ == nullptr) {
        quickSizePopup_ = new QuickSizePopup(host, this);
    }
    if (textFormatPopup_ == nullptr) {
        textFormatPopup_ = new TextFormatPopup(host, this);
    }
    if (paletteMenuPopup_ == nullptr) {
        paletteMenuPopup_ = new PaletteMenuPopup(host, this);
    }
    if (paletteWheel_ == nullptr) {
        paletteWheel_ = new PaletteWheelOverlay(host, this);
    }
}

QRectF ToolbarWidget::HostClientRect() const {
    const QWidget* host = window() != nullptr ? window() : this;
    return QRectF(0.0, 0.0, static_cast<qreal>(host->width()), static_cast<qreal>(host->height()));
}

QRectF ToolbarWidget::HostCanvasRect() const {
    QRectF rect = HostClientRect();
    rect.setTop(qt::ReservedToolbarHeight(settings_.autoHideToolbar, maximized_));
    return rect;
}

void ToolbarWidget::PositionOverlays() {
    if (textFormatPopup_ != nullptr && textFormatPopup_->isVisible()) {
        const QRectF canvas = HostCanvasRect();
        const QRectF editRect = textFormatEditRect_;
        const qt::TextFormatPopupLayout layout = qt::BuildTextFormatPopupLayout(
            canvas, editRect, textFormatPopup_->FontMenuOpen(), textFormatPopup_->SizeMenuOpen());
        textFormatPopup_->Configure(layout, qt::TextFormatPopupBounds(layout).toAlignedRect());
    }
    if (paletteWheel_ != nullptr && paletteWheel_->isVisible()) {
        paletteWheel_->Configure(HostClientRect().toAlignedRect());
    }
}

void ToolbarWidget::ShowQuickSizePopup(settings::Tool tool, const QPointF& hostAnchor) {
    EnsureOverlays();
    const QRectF canvas = HostCanvasRect();
    const float brushSize = tool == settings::Tool::Erase ? settings_.eraseBrushSize : settings_.drawBrushSize;
    const qt::QuickSizePopupLayout layout = qt::BuildQuickSizePopupLayout(
        canvas, hostAnchor, PreviewExtent(tool, brushSize, pageScale_));
    quickSizePopup_->Configure(layout, qt::QuickSizePopupBounds(layout).toAlignedRect());
    quickSizePopup_->SetState(tool, brushSize, settings_.quickSizeSliderMax, settings_.brushSizePresets,
        tool == settings::Tool::Erase ? 0xFFFFFFFFU : (settings_.palette.empty() ? 0xFF1F2937U : settings_.palette[settings_.colorIndex % settings_.palette.size()]));
    quickSizePopup_->show();
    quickSizePopup_->raise();

}

void ToolbarWidget::HideQuickSizePopup() {
    if (quickSizePopup_ != nullptr && quickSizePopup_->isVisible()) {
        quickSizePopup_->hide();

    }
}

bool ToolbarWidget::QuickSizePopupVisible() const {
    return quickSizePopup_ != nullptr && quickSizePopup_->isVisible();
}

QRect ToolbarWidget::QuickSizePopupBounds() const {
    return quickSizePopup_ != nullptr ? quickSizePopup_->geometry() : QRect();
}

bool ToolbarWidget::QuickSizeSliderDragging() const {
    return quickSizePopup_ != nullptr && quickSizePopup_->SliderDragging();
}

float ToolbarWidget::QuickSizeValue() const {
    return quickSizePopup_ != nullptr ? quickSizePopup_->BrushSize() : 0.0f;
}

settings::Tool ToolbarWidget::QuickSizeTool() const {
    return quickSizePopup_ != nullptr ? quickSizePopup_->tool_ : activeTool_;
}

void ToolbarWidget::ShowTextFormatPopup(const QRectF& editRectInHost) {
    if (!textFormatAvailable_) {
        return;
    }
    EnsureOverlays();
    textFormatEditRect_ = editRectInHost;
    const QRectF canvas = HostCanvasRect();
    const qt::TextFormatPopupLayout layout = qt::BuildTextFormatPopupLayout(
        canvas, editRectInHost, textFormatPopup_->FontMenuOpen(), textFormatPopup_->SizeMenuOpen());
    textFormatPopup_->SetState(formatStyle_, formatAlignment_, formatSizePreset_);
    textFormatPopup_->Configure(layout, qt::TextFormatPopupBounds(layout).toAlignedRect());
    textFormatPopup_->show();
    textFormatPopup_->raise();

}

void ToolbarWidget::HideTextFormatPopup() {
    if (textFormatPopup_ != nullptr && textFormatPopup_->isVisible()) {
        textFormatPopup_->hide();
        textFormatPopup_->SetFontMenuOpen(false);
        textFormatPopup_->SetSizeMenuOpen(false);

    }
}

bool ToolbarWidget::TextFormatPopupVisible() const {
    return textFormatPopup_ != nullptr && textFormatPopup_->isVisible();
}

QRect ToolbarWidget::TextFormatPopupBounds() const {
    return textFormatPopup_ != nullptr ? textFormatPopup_->geometry() : QRect();
}

void ToolbarWidget::TogglePaletteMenu() {
    EnsureOverlays();
    const bool open = !paletteMenuPopup_->isVisible();
    if (open) {
        CloseOverlays();
        const qt::PaletteMenuLayout layout = qt::BuildPaletteMenuLayout(ComputeLayout(), settings_.palette.size());
        paletteMenuPopup_->SetState(settings_.palette, settings_.colorIndex);
        paletteMenuPopup_->Configure(layout, layout.panelRect.toAlignedRect());
        paletteMenuPopup_->show();
        paletteMenuPopup_->raise();

    } else {
        paletteMenuPopup_->hide();

    }
    emit PaletteOverlayToggled(open);
    update();
}

void ToolbarWidget::HidePaletteMenu() {
    if (paletteMenuPopup_ != nullptr && paletteMenuPopup_->isVisible()) {
        paletteMenuPopup_->hide();
        emit PaletteOverlayToggled(false);

        update();
    }
}

bool ToolbarWidget::PaletteMenuVisible() const {
    return paletteMenuPopup_ != nullptr && paletteMenuPopup_->isVisible();
}

QRectF ToolbarWidget::PaletteMenuBounds() const {
    return paletteMenuPopup_ != nullptr ? QRectF(paletteMenuPopup_->geometry()) : QRectF();
}

std::vector<settings::Tool> ToolbarWidget::PaletteWheelDisplayedTools() const {
    std::vector<settings::Tool> tools;
    for (const settings::Tool tool : settings_.wheelTools) {
        if (tool != settings::Tool::Zoom) {
            tools.push_back(tool);
        }
    }
    return tools;
}

// Reference App::TogglePaletteWheel (ToolbarState.cpp:127-159): the wheel opens
// centred on the cursor, and - when the cursor is outside the host client rect
// (Win32 PtInRect, right/bottom exclusive) - below the toolbar's current-colour
// swatch, one kPaletteWheelSwatchDropDip below its bottom edge.  Every
// coordinate here is a host logical pixel and the constant is in the same unit,
// so no device-pixel-ratio conversion is involved.
void ToolbarWidget::TogglePaletteWheel() {
    QWidget* host = window();
    if (host == nullptr) {
        return;
    }
    QPointF anchor = host->mapFromGlobal(QCursor::pos());
    if (!ContainsPoint(HostClientRect(), anchor)) {
        const QPointF hostOrigin(mapTo(host, QPoint(0, 0)));
        const QRectF colorRect = ControlRect(ToolbarControl::CurrentColor).translated(hostOrigin);
        anchor = QPointF(colorRect.center().x(), colorRect.bottom() + kPaletteWheelSwatchDropDip);
    }
    TogglePaletteWheel(anchor);
}

void ToolbarWidget::TogglePaletteWheel(const QPointF& hostCenter) {
    EnsureOverlays();
    const bool open = !paletteWheel_->isVisible();
    if (open) {
        CloseOverlays();
        wheelCenter_ = hostCenter;
        wheelHoverIndex_.reset();
        paletteWheel_->SetEntries(settings_.palette, PaletteWheelDisplayedTools(), settings_.colorIndex, false, settings_.zoomPresets);
        paletteWheel_->Configure(HostClientRect().toAlignedRect());
        paletteWheel_->show();
        paletteWheel_->raise();
        // The reference seeds the hover slice under the cursor right after
        // opening (ToolbarState.cpp:145-147 UpdatePaletteWheelHover(cursor)).
        paletteWheel_->UpdateHover(hostCenter);
    } else {
        paletteWheel_->hide();
        wheelHoverIndex_.reset();
    }
    emit PaletteOverlayToggled(open);
    update();
}

void ToolbarWidget::HidePaletteWheel() {
    if (paletteWheel_ != nullptr && paletteWheel_->isVisible()) {
        paletteWheel_->hide();
        wheelHoverIndex_.reset();
        emit PaletteOverlayToggled(false);

        update();
    }
}

bool ToolbarWidget::PaletteWheelVisible() const {
    return paletteWheel_ != nullptr && paletteWheel_->isVisible();
}

void ToolbarWidget::CloseOverlays() {
    HideQuickSizePopup();
    HideTextFormatPopup();
    HidePaletteWheel();
    HidePaletteMenu();

}

bool ToolbarWidget::AnyOverlayVisible() const {
    return QuickSizePopupVisible() || TextFormatPopupVisible() || PaletteWheelVisible() || PaletteMenuVisible();
}

void ToolbarWidget::OnQuickSizeChanged(settings::Tool tool, float size) {
    if (tool == settings::Tool::Erase) {
        settings_.eraseBrushSize = size;
    } else {
        settings_.drawBrushSize = size;
    }
    emit BrushSizeChanged(tool, size);
}

void ToolbarWidget::OnTextFormatChanged() {
    update();
}

void ToolbarWidget::OnPaletteSelection(std::size_t index) {
    if (index >= settings_.palette.size()) {
        return;
    }
    SetColorIndex(index);
    emit ColorSelected(index);
    CloseOverlays();
}

void ToolbarWidget::OnPaletteToolSelection(settings::Tool tool) {
    SetActiveTool(tool);
    emit ToolSelected(tool);
    CloseOverlays();
}

void ToolbarWidget::SetColorIndex(std::size_t index) {
    if (index >= settings_.palette.size()) {
        return;
    }
    settings_.colorIndex = index;
    if (paletteMenuPopup_ != nullptr) {
        paletteMenuPopup_->SetState(settings_.palette, index);
    }
    if (paletteWheel_ != nullptr) {
        paletteWheel_->SetEntries(settings_.palette, PaletteWheelDisplayedTools(), index, paletteWheel_->ZoomPresetMode(), settings_.zoomPresets);
    }
    update();
}

void ToolbarWidget::SetTextFormatFontMenuOpen(bool open) {
    if (textFormatPopup_ == nullptr) {
        return;
    }
    textFormatPopup_->SetFontMenuOpen(open);
    if (open) {
        PositionOverlays();
    }
}

void ToolbarWidget::SetTextFormatSizeMenuOpen(bool open) {
    if (textFormatPopup_ == nullptr) {
        return;
    }
    textFormatPopup_->SetSizeMenuOpen(open);
    if (open) {
        PositionOverlays();
    }
}

void ToolbarWidget::ToggleTextFormatBold() {
    if (formatStyle_.has_value()) {
        formatStyle_->bold = !formatStyle_->bold;
    }
    if (textFormatPopup_ != nullptr) {
        textFormatPopup_->SetState(formatStyle_, formatAlignment_, formatSizePreset_);
    }
    update();
    emit TextBoldToggleRequested();
}

void ToolbarWidget::ToggleTextFormatItalic() {
    if (formatStyle_.has_value()) {
        formatStyle_->italic = !formatStyle_->italic;
    }
    if (textFormatPopup_ != nullptr) {
        textFormatPopup_->SetState(formatStyle_, formatAlignment_, formatSizePreset_);
    }
    update();
    emit TextItalicToggleRequested();
}

void ToolbarWidget::ToggleTextFormatUnderline() {
    if (formatStyle_.has_value()) {
        formatStyle_->underline = !formatStyle_->underline;
    }
    if (textFormatPopup_ != nullptr) {
        textFormatPopup_->SetState(formatStyle_, formatAlignment_, formatSizePreset_);
    }
    update();
    emit TextUnderlineToggleRequested();
}

void ToolbarWidget::OnWheelZoomModeChanged(bool zoomPresetMode) {
    if (paletteWheel_ != nullptr) {
        paletteWheel_->SetEntries(settings_.palette, PaletteWheelDisplayedTools(), settings_.colorIndex, zoomPresetMode, settings_.zoomPresets);
    }
    update();
}

bool ToolbarWidget::event(QEvent* event) {
    // The native caption glyph dims on inactive windows; the shell does not
    // send a repaint on activation changes, so the widget asks for its own.
    if (event->type() == QEvent::ActivationChange && captionButtonsVisible_) {
        update();
    }
    return QWidget::event(event);
}

bool ToolbarWidget::eventFilter(QObject* watched, QEvent* event) {
    QWidget* host = window();
    if (host != nullptr && watched == host && event->type() == QEvent::Resize) {
        UpdateToolbarGeometry();
    }

    if (settings_.autoHideToolbar && host != nullptr && event->type() == QEvent::MouseMove) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        UpdateAutoHideForCursor(host->mapFromGlobal(mouse->globalPosition().toPoint()), false);
    }

    if (AnyOverlayVisible()) {
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                CloseOverlays();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint hostPosition = host != nullptr
                ? host->mapFromGlobal(mouse->globalPosition().toPoint())
                : mouse->globalPosition().toPoint();

            // Reference order (App.cpp:186-230): an open palette menu or wheel
            // consumes the first click outside it and closes; the quick-size and
            // text-format popups close but let the click through.
            if (PaletteMenuVisible() && !ContainsPixel(PaletteMenuBounds().toAlignedRect(), hostPosition)) {
                CloseOverlays();
                return true;
            }
            if (PaletteWheelVisible() && !paletteWheel_->geometry().contains(hostPosition)) {
                CloseOverlays();
                return true;
            }
            if (QuickSizePopupVisible() && !QuickSizePopupBounds().contains(hostPosition)) {
                HideQuickSizePopup();
            }
            if (TextFormatPopupVisible() && !TextFormatPopupBounds().contains(hostPosition)) {
                HideTextFormatPopup();
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

}  // namespace cnote::ui
