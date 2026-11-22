#include "ui/ToolbarWidget.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <utility>

#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QShowEvent>
#include <QTimer>
#include <QWindow>

#include "ui/ChromeAssetsQt.h"
#include "ui/ChromeStyleQt.h"
#include "ui/PointerPositionQt.h"

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

// Palette-wheel paint.  Slices are separate rounded cards floating on soft
// shadows; the hovered one lifts (grows outward, slides off the centre and
// casts a deeper shadow), animated together with the wheel's open fade.
constexpr std::uint32_t kWheelUnselectedFill = 0xF5202934U;   // App.cpp:6855 (245,32,41,52)
constexpr std::uint32_t kWheelSliceEdge = 0x30FFFFFFU;        // hairline so dark slices read on dark chrome
constexpr std::uint32_t kWheelSelectionTab = kChromeAccent;   // selected-entry arc outside the slice
constexpr std::uint32_t kWheelToolIconTint = 0xFFE8EEF5U;     // App.cpp:6892 RGB(232,238,245)
constexpr std::uint32_t kWheelSelectedIconTint = 0xFFFFFFFFU;  // App.cpp:6892
constexpr std::uint32_t kWheelCenterFill = 0xFF121820U;       // App.cpp:7022 (255,18,24,32)
constexpr std::uint32_t kWheelZoomCenterFill = 0xFF253A4EU;   // App.cpp:7021 (255,37,58,78)
constexpr std::uint32_t kWheelZoomCenterBorder = 0xEB84BFFFU;  // App.cpp:7036 (235,132,191,255)
constexpr qreal kPaletteWheelSliceGapDip = 5.0;               // even gap between neighbouring slices
constexpr qreal kPaletteWheelRingGapDip = 6.0;                // gap between the centre disc and the slices
constexpr qreal kPaletteWheelCornerDip = 7.0;                 // slice corner radius (outer corners)
constexpr qreal kPaletteWheelTabGapDip = 5.0;                 // slice outer edge to the tab's centre line
constexpr qreal kPaletteWheelTabWidthDip = 3.5;
constexpr qreal kPaletteWheelHoverIconScale = 0.15;           // extra icon scale at full lift
constexpr qreal kPaletteWheelOpenScale = 0.9;                 // wheel scale at the start of the open animation
constexpr int kPaletteWheelHoverMs = 140;
constexpr int kPaletteWheelOpenMs = 160;
constexpr int kPaletteWheelFrameMs = 16;
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
        return QStringLiteral("text.svg");
    case settings::Tool::Latex:
        return QStringLiteral("latex.svg");
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
        return QStringLiteral("T");
    case settings::Tool::Latex:
        return QStringLiteral("L");
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

    // Hiding the popup abandons an in-flight drag or text edit; otherwise a
    // later reopen would paint the stale active thumb and keep the auto-hide
    // guard raised through QuickSizeSliderDragging().
    void ResetInteraction() {
        dragging_ = false;
        editing_ = false;
        editText_.clear();
        update();
    }

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
        // Mouse-only and opened over a live text edit: clicking a button must
        // leave the keyboard with the editor (typing, Ctrl+Enter, Escape), as
        // Excalidraw's property panel leaves it with its textarea.
        setFocusPolicy(Qt::NoFocus);
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

    // Closing the popup drops both dropdowns and any hover highlight so the
    // next open starts from a clean paint state.
    void ResetInteraction() {
        fontMenuOpen_ = false;
        sizeMenuOpen_ = false;
        hoveredFontOption_.reset();
        hoveredSizeOption_.reset();
        update();
    }

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
        animationTimer_.setInterval(kPaletteWheelFrameMs);
        QObject::connect(&animationTimer_, &QTimer::timeout, this, [this]() { AdvanceAnimation(); });
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
            StartAnimation();
            update();
        }
    }

protected:
    // Floating-slice wheel: soft shadows, rounded slices separated by an even
    // gap, the hovered slice lifted, tool icons or preset labels, then the
    // centre disc with the zoom magnifier.
    void paintEvent(QPaintEvent*) override {
        const std::size_t entries = EntryCount();
        if (entries == 0U) {
            return;
        }
        SyncHoverProgress();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const qt::PaletteWheelLayout layout = BuildLayout();
        const QPointF center = layout.center;
        const qreal ringInner = layout.innerRadius + kPaletteWheelRingGapDip;
        const qreal outer = layout.outerRadius;
        const qreal dpr = devicePixelRatioF();

        const qreal open = EaseOut(openProgress_);
        const qreal scale = kPaletteWheelOpenScale + ((1.0 - kPaletteWheelOpenScale) * open);
        painter.setOpacity(open);
        painter.translate(center);
        painter.scale(scale, scale);
        painter.translate(-center);

        // Slice `index` at lift 0..1: grown outward by the hover grow and
        // pushed off the centre along its bisector by the hover offset.
        const auto lift = [&](std::size_t index) { return EaseOut(hoverProgress_[index]); };
        const auto bisector = [&](std::size_t index) {
            return AngleAt((static_cast<qreal>(index) + 0.5) / static_cast<qreal>(entries));
        };
        const auto sliceOrigin = [&](std::size_t index) {
            return Polar(center, qt::PaletteWheelHoverOffset() * lift(index), bisector(index));
        };
        const auto slicePath = [&](std::size_t index) {
            const QPointF origin = sliceOrigin(index);
            const qreal sliceOuter = outer + (qt::PaletteWheelHoverGrow() * lift(index));
            if (entries == 1U) {
                QPainterPath ring;
                ring.addEllipse(origin, sliceOuter, sliceOuter);
                ring.addEllipse(origin, ringInner, ringInner);
                return ring;
            }
            return SlicePath(origin, ringInner, sliceOuter,
                AngleAt(static_cast<qreal>(index) / static_cast<qreal>(entries)),
                AngleAt(static_cast<qreal>(index + 1U) / static_cast<qreal>(entries)));
        };

        // Resting shadows go down first so no slice darkens its neighbours;
        // a lifted slice adds its deeper shadow right under itself instead.
        for (std::size_t index = 0; index < entries; ++index) {
            PaintShadow(painter, slicePath(index), 0.0);
        }
        std::vector<std::size_t> order(entries);
        std::iota(order.begin(), order.end(), std::size_t { 0 });
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return hoverProgress_[a] < hoverProgress_[b];
        });
        for (const std::size_t index : order) {
            const QPainterPath path = slicePath(index);
            if (lift(index) > 0.0) {
                PaintShadow(painter, path, lift(index));
            }
            painter.setPen(QPen(ChromeColor(kWheelSliceEdge), 1.0));
            painter.setBrush(FillColor(index));
            painter.drawPath(path);
            const qreal glyphRadius = (ringInner + outer + (qt::PaletteWheelHoverGrow() * lift(index))) * 0.5;
            PaintGlyph(painter, index, Polar(sliceOrigin(index), glyphRadius, bisector(index)), lift(index), dpr);
            if (IsSelectedEntry(index)) {
                PaintSelectionTab(painter, sliceOrigin(index), outer + (qt::PaletteWheelHoverGrow() * lift(index)),
                    AngleAt(static_cast<qreal>(index) / static_cast<qreal>(entries)),
                    AngleAt(static_cast<qreal>(index + 1U) / static_cast<qreal>(entries)));
            }
        }

        const bool zoomToggleEnabled = HasZoomEntry();
        const bool zoomCentre = zoomToggleEnabled && zoomPresetMode_;
        QPainterPath disc;
        disc.addEllipse(center, layout.innerRadius, layout.innerRadius);
        PaintShadow(painter, disc, 0.0);
        painter.setPen(QPen(ChromeColor(zoomCentre ? kWheelZoomCenterBorder : kWheelSliceEdge), zoomCentre ? 1.5 : 1.0));
        painter.setBrush(ChromeColor(zoomCentre ? kWheelZoomCenterFill : kWheelCenterFill));
        painter.drawPath(disc);
        if (zoomToggleEnabled) {
            const int extent = std::max(1, static_cast<int>(std::lround(kPaletteWheelCenterIconHalfDip * 2.0)));
            const QPixmap icon = ChromeIconPixmap(ToolIconName(settings::Tool::Zoom), QSize(extent, extent),
                ChromeColor(zoomCentre ? kWheelSelectedIconTint : kWheelToolIconTint), dpr);
            if (!icon.isNull()) {
                painter.drawPixmap(QPointF(center.x() - kPaletteWheelCenterIconHalfDip, center.y() - kPaletteWheelCenterIconHalfDip), icon);
            }
        }
    }

    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        openProgress_ = 0.0;
        hoverProgress_.assign(EntryCount(), 0.0);
        StartAnimation();
    }

    void hideEvent(QHideEvent* event) override {
        animationTimer_.stop();
        QWidget::hideEvent(event);
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

    // Normalized angle `fraction` (0 == the index-0 slice start) to screen
    // radians: atan2 = fraction * 2pi - pi/2, mirroring
    // appui::PaletteWheelIndexAtPoint.
    static qreal AngleAt(qreal fraction) {
        return (fraction * 2.0 * kWheelPi) - (kWheelPi * 0.5);
    }

    static QPointF Polar(const QPointF& center, qreal radius, qreal angle) {
        return QPointF(center.x() + (std::cos(angle) * radius), center.y() + (std::sin(angle) * radius));
    }

    static qreal EaseOut(qreal progress) {
        const qreal remaining = 1.0 - std::clamp(progress, 0.0, 1.0);
        return 1.0 - (remaining * remaining * remaining);
    }

    // Appends a circular arc from screen angle `from` to `to` (radians,
    // clockwise on screen); QPainterPath::arcTo takes counter-clockwise degrees.
    static void AppendArc(QPainterPath& path, const QPointF& center, qreal radius, qreal from, qreal to) {
        constexpr qreal kDegrees = 180.0 / kWheelPi;
        path.arcTo(QRectF(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0), -from * kDegrees, -(to - from) * kDegrees);
    }

    // Annular sector between screen angles `start` and `end`, inset so
    // neighbours keep an even kPaletteWheelSliceGapDip gap (each straight
    // edge is parallel to the boundary radius) and with rounded corners.
    static QPainterPath SlicePath(const QPointF& center, qreal inner, qreal outer, qreal start, qreal end) {
        const qreal halfGap = kPaletteWheelSliceGapDip * 0.5;
        const qreal outerStart = start + std::asin(std::min(1.0, halfGap / outer));
        const qreal outerEnd = end - std::asin(std::min(1.0, halfGap / outer));
        qreal innerStart = start + std::asin(std::min(1.0, halfGap / inner));
        qreal innerEnd = end - std::asin(std::min(1.0, halfGap / inner));
        if (innerEnd < innerStart) {
            innerStart = innerEnd = (start + end) * 0.5;  // a thin slice tapers to a point
        }
        const QPointF outerA = Polar(center, outer, outerStart);
        const QPointF outerB = Polar(center, outer, outerEnd);
        const QPointF innerA = Polar(center, inner, innerStart);
        const QPointF innerB = Polar(center, inner, innerEnd);
        const qreal edge = QLineF(innerA, outerA).length();
        const qreal outerCorner = std::min({ kPaletteWheelCornerDip, edge * 0.5, outer * (outerEnd - outerStart) * 0.5 });
        const qreal innerCorner = std::min({ kPaletteWheelCornerDip * 0.6, edge * 0.5, inner * (innerEnd - innerStart) * 0.5 });
        const auto toward = [](const QPointF& from, const QPointF& to, qreal distance) {
            const qreal length = QLineF(from, to).length();
            return length > 0.0 ? from + ((to - from) * (distance / length)) : from;
        };
        QPainterPath path;
        path.moveTo(toward(outerA, innerA, outerCorner));
        path.quadTo(outerA, Polar(center, outer, outerStart + (outerCorner / outer)));
        AppendArc(path, center, outer, outerStart + (outerCorner / outer), outerEnd - (outerCorner / outer));
        path.quadTo(outerB, toward(outerB, innerB, outerCorner));
        path.lineTo(toward(innerB, outerB, innerCorner));
        path.quadTo(innerB, Polar(center, inner, innerEnd - (innerCorner / inner)));
        AppendArc(path, center, inner, innerEnd - (innerCorner / inner), innerStart + (innerCorner / inner));
        path.quadTo(innerA, toward(innerA, outerA, innerCorner));
        path.closeSubpath();
        return path;
    }

    // Soft drop shadow: stacked translucent round-joined strokes, so alpha
    // builds up toward the shape's edge.  `lift` deepens and lowers it.
    static void PaintShadow(QPainter& painter, const QPainterPath& path, qreal lift) {
        constexpr int kLayers = 5;
        painter.save();
        painter.translate(0.0, 2.0 + (3.0 * lift));
        painter.setBrush(Qt::NoBrush);
        const int alpha = static_cast<int>(std::lround(lift > 0.0 ? 9.0 * lift : 5.0));
        for (int layer = kLayers; layer >= 1; --layer) {
            const qreal spread = static_cast<qreal>(layer) * (1.5 + lift);
            painter.setPen(QPen(QColor(0, 0, 0, alpha), spread * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(path);
        }
        painter.restore();
    }

    // Selected entry (colour, tool or zoom preset): a rounded accent arc just
    // outside the slice's outer edge, inset past the slice's rounded corners.
    // It sits on the backdrop, so it reads the same on every palette colour,
    // and it follows the lift.
    static void PaintSelectionTab(QPainter& painter, const QPointF& origin, qreal sliceOuter, qreal start, qreal end) {
        constexpr qreal kDegrees = 180.0 / kWheelPi;
        const qreal radius = sliceOuter + kPaletteWheelTabGapDip;
        const qreal inset = ((kPaletteWheelSliceGapDip * 0.5) + kPaletteWheelCornerDip) / radius;
        const qreal from = start + inset;
        const qreal to = end - inset;
        if (to <= from) {
            return;
        }
        const QRectF circle(origin.x() - radius, origin.y() - radius, radius * 2.0, radius * 2.0);
        QPainterPath arc;
        arc.arcMoveTo(circle, -from * kDegrees);
        arc.arcTo(circle, -from * kDegrees, -(to - from) * kDegrees);
        painter.setPen(QPen(ChromeColor(kWheelSelectionTab), kPaletteWheelTabWidthDip, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(arc);
    }

    // Tool icon or zoom-preset label at `glyph`; colour slices carry none.
    void PaintGlyph(QPainter& painter, std::size_t index, const QPointF& glyph, qreal lift, qreal dpr) const {
        if (IsColorEntry(index)) {
            return;
        }
        if (zoomPresetMode_) {
            float preset = 0.0f;
            if (!ZoomPresetValueAt(index, preset)) {
                return;
            }
            painter.setPen(ChromeColor(kChromeTextPrimary));
            painter.setFont(ChromeFont(13));
            const QString label = QString::number(static_cast<int>(std::lround(preset * 100.0))) + QStringLiteral("%");
            painter.drawText(QRectF(glyph.x() - 24.0, glyph.y() - 10.0, 48.0, 20.0), Qt::AlignCenter, label);
            return;
        }
        const std::size_t toolIndex = index - palette_.size();
        if (toolIndex >= tools_.size()) {
            return;
        }
        const qreal half = qt::PaletteWheelIconHalfSize() * (1.0 + (kPaletteWheelHoverIconScale * lift));
        const int extent = std::max(1, static_cast<int>(std::lround(half * 2.0)));
        const QPixmap icon = ChromeIconPixmap(ToolIconName(tools_[toolIndex]), QSize(extent, extent),
            ChromeColor(IsSelectedEntry(index) ? kWheelSelectedIconTint : kWheelToolIconTint), dpr);
        if (!icon.isNull()) {
            painter.drawPixmap(glyph - QPointF(static_cast<qreal>(extent) * 0.5, static_cast<qreal>(extent) * 0.5), icon);
        }
    }

    void SyncHoverProgress() {
        const std::size_t entries = EntryCount();
        if (hoverProgress_.size() != entries) {
            hoverProgress_.assign(entries, 0.0);
        }
    }

    void StartAnimation() {
        if (!animationTimer_.isActive()) {
            animationClock_.start();
            animationTimer_.start();
        }
    }

    // Steps the open fade and every slice's lift toward its target; stops the
    // timer once nothing moves.
    void AdvanceAnimation() {
        const qreal elapsed = static_cast<qreal>(animationClock_.restart());
        SyncHoverProgress();
        openProgress_ = std::min(1.0, openProgress_ + (elapsed / kPaletteWheelOpenMs));
        bool moving = openProgress_ < 1.0;
        const qreal step = elapsed / kPaletteWheelHoverMs;
        for (std::size_t index = 0; index < hoverProgress_.size(); ++index) {
            const bool hovered = owner_->wheelHoverIndex_.has_value() && *owner_->wheelHoverIndex_ == index;
            qreal& progress = hoverProgress_[index];
            progress = hovered ? std::min(1.0, progress + step) : std::max(0.0, progress - step);
            moving = moving || (hovered ? progress < 1.0 : progress > 0.0);
        }
        if (!moving) {
            animationTimer_.stop();
        }
        update();
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
        return ChromeColor(kWheelUnselectedFill);
    }

    ToolbarWidget* owner_ = nullptr;
    std::vector<std::uint32_t> palette_ {};
    std::vector<settings::Tool> tools_ {};
    std::size_t colorIndex_ = 0;
    bool zoomPresetMode_ = false;
    std::array<float, settings::kZoomPresetCount> zoomPresets_ {};
    QTimer animationTimer_ {};
    QElapsedTimer animationClock_ {};
    qreal openProgress_ = 1.0;
    std::vector<qreal> hoverProgress_ {};  // per entry, 0 (resting) .. 1 (lifted), linear
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

    // One application-scoped filter tracks host resizes and dismisses open
    // overlays on the first click outside them, matching the reference
    // WM_LBUTTONDOWN overlay order in App::HandlePaletteClick /
    // HandleQuickSizePopupClick / HandleTextFormatPopupClick.  The auto-hide
    // cursor input comes from the host window (QtMainWindow), which also knows
    // whether a canvas interaction is active.
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
    if (activeOverlay_ == OverlayId::PaletteWheel && paletteWheel_ != nullptr) {
        paletteWheel_->SetEntries(settings_.palette, PaletteWheelDisplayedTools(), settings_.colorIndex, paletteWheel_->ZoomPresetMode(), settings_.zoomPresets);
    }
    if (!settings_.autoHideToolbar) {
        SetRevealTarget(true, /*animate=*/false);
    } else if (!wasAutoHide) {
        // Auto-hide just turned on (including the first settings load): the
        // band parks above the host.  SetAutoHideEnabled early-returns once
        // the flag already matches, so without this the band would start
        // revealed and slide away on the first mouse move.
        SetRevealTarget(false, /*animate=*/false);
    } else {
        UpdateToolbarGeometry();
    }
    update();
}

bool ToolbarWidget::IsToolInWheel(settings::Tool tool) const {
    return std::find(settings_.wheelTools.begin(), settings_.wheelTools.end(), tool) != settings_.wheelTools.end();
}

bool ToolbarWidget::ShowWheelToolMenu(const QPointF& widgetPosition, const QPoint& globalPosition) {
    // Reference App::HandleToolbarRightClick (App.cpp:6195-6239): the zoom
    // indicator is probed first, then the tool buttons in the reference order
    // (plus the LaTeX button); a hit shows the single-item "Add/Remove from
    // Quick Wheel" menu.
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
            { ToolbarControl::LatexTool, settings::Tool::Latex },
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

bool ToolbarWidget::MayShowOverlay(OverlayId id) const {
    switch (id) {
    case OverlayId::QuickSize:
        return activeTool_ == settings::Tool::Draw || activeTool_ == settings::Tool::Erase;
    case OverlayId::TextFormat:
        return activeTool_ == settings::Tool::Text;
    case OverlayId::None:
    case OverlayId::PaletteMenu:
    case OverlayId::PaletteWheel:
        return true;
    }
    return true;
}

void ToolbarWidget::SetActiveTool(settings::Tool tool) {
    if (activeTool_ == tool) {
        return;
    }
    activeTool_ = tool;
    // A popup that belongs to the old tool cannot survive the switch.
    if (activeOverlay_ != OverlayId::None && !MayShowOverlay(activeOverlay_)) {
        CloseOverlay(activeOverlay_);
    }
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
    case ToolbarControl::LatexTool:
        return layout.latexToolRect;
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
        ToolbarControl::LatexTool,
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
    case ToolbarControl::LatexTool:
        return settings::Tool::Latex;
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
        return ToolbarControl::TextTool;
    case settings::Tool::Latex:
        return ToolbarControl::LatexTool;
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
        || Revealed()
        || zoomEdit_.active
        || QuickSizeSliderDragging()
        || AnyOverlayVisible();
    return visible ? qt::ToolbarHeight(maximized_) : 0.0;
}

void ToolbarWidget::SetAutoHideEnabled(bool enabled) {
    if (settings_.autoHideToolbar == enabled) {
        return;
    }
    settings_.autoHideToolbar = enabled;
    SetRevealTarget(!enabled, /*animate=*/false);
    update();
}

void ToolbarWidget::SetRevealed(bool revealed) {
    SetRevealTarget(revealed);
}

void ToolbarWidget::SetRevealTarget(bool revealed, bool animate) {
    if (!settings_.autoHideToolbar) {
        revealed = true;
    }
    const qreal target = revealed ? 1.0 : 0.0;
    if (!animate) {
        // Toggling auto-hide parks or restores the band immediately (the
        // reference never animates that switch).
        const bool stateChanged = Revealed() != revealed;
        const bool progressChanged = std::abs(revealProgress_ - target) > 0.0001;
        revealTimer_->stop();
        revealProgress_ = target;
        reveal_.from = target;
        reveal_.to = target;
        reveal_.phase = revealed ? RevealPhase::Shown : RevealPhase::Hidden;
        if (stateChanged) {
            emit RevealStateChanged(revealed);
        }
        UpdateToolbarGeometry();
        if (stateChanged || progressChanged) {
            emit RevealProgressChanged(revealProgress_);
        }
        update();
        return;
    }
    // A repeated request in the same direction must not restart the 120 ms
    // easing on every mouse move.
    if (Revealed() == revealed) {
        return;
    }
    // Elapsed-time driven (plan step 5.3): capture the current progress and ease
    // toward the target over the reference 120 ms, never blocking.  Retargeting
    // folds the pending frame into `from` without emitting a throwaway signal.
    if (revealTimer_->isActive()) {
        const qreal linear = std::clamp(static_cast<double>(revealClock_.elapsed()) / 120.0, 0.0, 1.0);
        const qreal eased = linear * linear * (3.0 - (2.0 * linear));
        revealProgress_ = reveal_.from + ((reveal_.to - reveal_.from) * eased);
    }
    reveal_.from = revealProgress_;
    reveal_.to = target;
    reveal_.phase = revealed ? RevealPhase::Showing : RevealPhase::Hiding;
    emit RevealStateChanged(revealed);
    if (std::abs(reveal_.from - reveal_.to) <= 0.001) {
        revealProgress_ = reveal_.to;
        reveal_.phase = revealed ? RevealPhase::Shown : RevealPhase::Hidden;
        revealTimer_->stop();
        UpdateToolbarGeometry();
        emit RevealProgressChanged(revealProgress_);
        update();
        return;
    }
    revealClock_.start();
    revealTimer_->start();
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
        hostPosition.y(), maximized_, Revealed(),
        interactionActive || AnyOverlayVisible() || zoomEdit_.active, frameThicknessX_);
    if (decision == qt::ToolbarAutoHide::Reveal) {
        SetRevealed(true);
    } else if (decision == qt::ToolbarAutoHide::Hide) {
        SetRevealed(false);
    }
}

void ToolbarWidget::UpdateAutoHideAtBoundary(const QPointF& hostPosition, bool interactionActive) {
    if (!settings_.autoHideToolbar) {
        return;
    }
    const QWidget* host = window();
    const QSizeF clientSize = host != nullptr ? QSizeF(host->size()) : QSizeF(size());
    const qt::ToolbarAutoHide decision = qt::ResolveToolbarAutoHideAtBoundary(
        hostPosition, clientSize, maximized_, interactionActive || AnyOverlayVisible() || zoomEdit_.active);
    if (decision == qt::ToolbarAutoHide::Reveal) {
        SetRevealed(true);
    } else if (decision == qt::ToolbarAutoHide::Hide) {
        SetRevealed(false);
    }
}

void ToolbarWidget::OnRevealTick() {
    const double elapsed = static_cast<double>(revealClock_.elapsed());
    const qreal linear = std::clamp(elapsed / 120.0, 0.0, 1.0);
    const qreal eased = linear * linear * (3.0 - (2.0 * linear));  // smoothstep, App.cpp:2501
    revealProgress_ = reveal_.from + ((reveal_.to - reveal_.from) * eased);
    if (linear >= 1.0) {
        revealProgress_ = reveal_.to;
        reveal_.phase = reveal_.to >= 1.0 ? RevealPhase::Shown : RevealPhase::Hidden;
        revealTimer_->stop();
    }
    UpdateToolbarGeometry();
    emit RevealProgressChanged(revealProgress_);
    update();
}

void ToolbarWidget::paintEvent(QPaintEvent*) {
    // Exactly the vertical-extent truth the shell's hit test consumes.
    if (VisibleToolbarHeight() <= 0.0) {
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
        { layout.latexToolRect, settings::Tool::Latex },
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
        if (zoomEdit_.active) {
            const QRectF entry = titleLayout.zoomRect.adjusted(0.0, 7.0, 0.0, -7.0);
            ChromeStrokeRoundedRect(painter, entry, ChromeColor(kChromeAccent), 1.0, 4.0);
            painter.setPen(ChromeColor(kChromeSelectionBorder));
            const QRectF textRect = entry.adjusted(6.0, 0.0, -6.0, 0.0);
            painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, zoomEdit_.text);
            const qreal advance = QFontMetricsF(ChromeFont(15)).horizontalAdvance(zoomEdit_.text);
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
        CloseOverlays();
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
    QWidget::leaveEvent(event);
}

void ToolbarWidget::keyPressEvent(QKeyEvent* event) {
    if (!zoomEdit_.active) {
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
        zoomEdit_.text.chop(1);
        update();
        return;
    }
    const QString text = event->text();
    if (!text.isEmpty() && (text.at(0).isDigit() || text.at(0) == QLatin1Char('%') || text.at(0) == QLatin1Char('.'))) {
        if (text.at(0) != QLatin1Char('%')) {
            zoomEdit_.text += text;
        }
        update();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ToolbarWidget::focusOutEvent(QFocusEvent* event) {
    // Leaving the toolbar commits like the reference's edit controls; the
    // hand-drawn editor otherwise stayed active forever and kept the auto-hide
    // guard raised.
    if (zoomEdit_.active) {
        CommitZoomEdit();
    }
    QWidget::focusOutEvent(event);
}

void ToolbarWidget::BeginZoomEdit() {
    zoomEdit_.active = true;
    zoomEdit_.text = zoomLabel_;
    setFocus(Qt::MouseFocusReason);
    update();
}

void ToolbarWidget::CancelZoomEdit() {
    if (!zoomEdit_.active) {
        return;
    }
    zoomEdit_.active = false;
    zoomEdit_.text.clear();
    update();
}

void ToolbarWidget::CommitZoomEdit() {
    if (!zoomEdit_.active) {
        return;
    }
    const QString text = zoomEdit_.text.remove(QLatin1Char('%')).trimmed();
    zoomEdit_.active = false;
    zoomEdit_.text.clear();
    update();
    if (!text.isEmpty()) {
        emit ZoomTextCommitted(text);
    }
}

void ToolbarWidget::ZoomCommitIfEditing() {
    CommitZoomEdit();
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
    if (activeOverlay_ == OverlayId::TextFormat && textFormatPopup_ != nullptr) {
        const QRectF canvas = HostCanvasRect();
        const QRectF editRect = textFormatEditRect_;
        const qt::TextFormatPopupLayout layout = qt::BuildTextFormatPopupLayout(
            canvas, editRect, textFormatPopup_->FontMenuOpen(), textFormatPopup_->SizeMenuOpen());
        textFormatPopup_->Configure(layout, qt::TextFormatPopupBounds(layout).toAlignedRect());
    }
    if (activeOverlay_ == OverlayId::PaletteWheel && paletteWheel_ != nullptr) {
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
    OpenOverlay(OverlayId::QuickSize);
}

void ToolbarWidget::HideQuickSizePopup() {
    CloseOverlay(OverlayId::QuickSize);
}

bool ToolbarWidget::QuickSizePopupVisible() const {
    return activeOverlay_ == OverlayId::QuickSize;
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
    OpenOverlay(OverlayId::TextFormat);
}

void ToolbarWidget::HideTextFormatPopup() {
    CloseOverlay(OverlayId::TextFormat);
}

bool ToolbarWidget::TextFormatPopupVisible() const {
    return activeOverlay_ == OverlayId::TextFormat;
}

QRect ToolbarWidget::TextFormatPopupBounds() const {
    return textFormatPopup_ != nullptr ? textFormatPopup_->geometry() : QRect();
}

void ToolbarWidget::TogglePaletteMenu() {
    EnsureOverlays();
    if (activeOverlay_ != OverlayId::PaletteMenu) {
        const qt::PaletteMenuLayout layout = qt::BuildPaletteMenuLayout(ComputeLayout(), settings_.palette.size());
        paletteMenuPopup_->SetState(settings_.palette, settings_.colorIndex);
        paletteMenuPopup_->Configure(layout, layout.panelRect.toAlignedRect());
    }
    ToggleOverlay(OverlayId::PaletteMenu);
}

void ToolbarWidget::HidePaletteMenu() {
    CloseOverlay(OverlayId::PaletteMenu);
}

bool ToolbarWidget::PaletteMenuVisible() const {
    return activeOverlay_ == OverlayId::PaletteMenu;
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
    QPointF anchor = host->mapFromGlobal(GlobalCursorPos());
    if (!ContainsPoint(HostClientRect(), anchor)) {
        const QPointF hostOrigin(mapTo(host, QPoint(0, 0)));
        const QRectF colorRect = ControlRect(ToolbarControl::CurrentColor).translated(hostOrigin);
        anchor = QPointF(colorRect.center().x(), colorRect.bottom() + kPaletteWheelSwatchDropDip);
    }
    TogglePaletteWheel(anchor);
}

void ToolbarWidget::TogglePaletteWheel(const QPointF& hostCenter) {
    EnsureOverlays();
    if (activeOverlay_ == OverlayId::PaletteWheel) {
        CloseOverlay(OverlayId::PaletteWheel);
        return;
    }
    wheelCenter_ = hostCenter;
    wheelHoverIndex_.reset();
    // Opening is the one deliberate reset of the wheel's presets/tools mode;
    // every other state refresh preserves the mode the user toggled.
    paletteWheel_->SetEntries(settings_.palette, PaletteWheelDisplayedTools(), settings_.colorIndex, false, settings_.zoomPresets);
    paletteWheel_->Configure(HostClientRect().toAlignedRect());
    // The reference seeds the hover slice under the cursor right after
    // opening (ToolbarState.cpp:145-147 UpdatePaletteWheelHover(cursor)).
    paletteWheel_->UpdateHover(hostCenter);
    OpenOverlay(OverlayId::PaletteWheel);
}

void ToolbarWidget::HidePaletteWheel() {
    CloseOverlay(OverlayId::PaletteWheel);
}

bool ToolbarWidget::PaletteWheelVisible() const {
    return activeOverlay_ == OverlayId::PaletteWheel;
}

void ToolbarWidget::OpenOverlay(OverlayId id) {
    if (id == OverlayId::None) {
        CloseOverlays();
        return;
    }
    QWidget* overlay = nullptr;
    switch (id) {
    case OverlayId::PaletteMenu: overlay = paletteMenuPopup_; break;
    case OverlayId::PaletteWheel: overlay = paletteWheel_; break;
    case OverlayId::QuickSize: overlay = quickSizePopup_; break;
    case OverlayId::TextFormat: overlay = textFormatPopup_; break;
    case OverlayId::None: break;
    }
    if (activeOverlay_ == id) {
        // Re-show/re-anchor an overlay that is already the truth (e.g. the
        // brush popup moved because the cursor moved).
        if (overlay != nullptr) {
            overlay->show();
            overlay->raise();
        }
        return;
    }
    if (activeOverlay_ != OverlayId::None) {
        CloseOverlay(activeOverlay_);
    }
    activeOverlay_ = id;
    if (overlay != nullptr) {
        overlay->show();
        overlay->raise();
    }
    if (id == OverlayId::PaletteMenu || id == OverlayId::PaletteWheel) {
        emit PaletteOverlayToggled(true);
    }
    update();
}

void ToolbarWidget::ToggleOverlay(OverlayId id) {
    if (id == OverlayId::None) {
        CloseOverlays();
        return;
    }
    if (activeOverlay_ == id) {
        CloseOverlay(id);
    } else {
        OpenOverlay(id);
    }
}

void ToolbarWidget::CloseOverlay(OverlayId id) {
    if (activeOverlay_ != id) {
        return;
    }
    activeOverlay_ = OverlayId::None;
    switch (id) {
    case OverlayId::QuickSize:
        if (quickSizePopup_ != nullptr) {
            quickSizePopup_->ResetInteraction();
            quickSizePopup_->hide();
        }
        break;
    case OverlayId::TextFormat:
        if (textFormatPopup_ != nullptr) {
            textFormatPopup_->ResetInteraction();
            textFormatPopup_->hide();
        }
        textFormatEditRect_ = {};
        break;
    case OverlayId::PaletteMenu:
        if (paletteMenuPopup_ != nullptr) {
            paletteMenuPopup_->hide();
        }
        emit PaletteOverlayToggled(false);
        break;
    case OverlayId::PaletteWheel:
        if (paletteWheel_ != nullptr) {
            paletteWheel_->hide();
        }
        wheelHoverIndex_.reset();
        emit PaletteOverlayToggled(false);
        break;
    case OverlayId::None:
        break;
    }
    update();
}

void ToolbarWidget::CloseOverlays() {
    // Closing chrome dismisses a hand-drawn zoom editor rather than committing
    // the half-typed value (Cancel is also what Escape does).
    CancelZoomEdit();
    CloseOverlay(activeOverlay_);
}

bool ToolbarWidget::AnyOverlayVisible() const {
    return activeOverlay_ != OverlayId::None;
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
            if (activeOverlay_ == OverlayId::PaletteMenu
                && !ContainsPixel(PaletteMenuBounds().toAlignedRect(), hostPosition)) {
                CloseOverlay(OverlayId::PaletteMenu);
                return true;
            }
            if (activeOverlay_ == OverlayId::PaletteWheel
                && (paletteWheel_ == nullptr || !paletteWheel_->geometry().contains(hostPosition))) {
                CloseOverlay(OverlayId::PaletteWheel);
                return true;
            }
            if (activeOverlay_ == OverlayId::QuickSize
                && !QuickSizePopupBounds().contains(hostPosition)) {
                CloseOverlay(OverlayId::QuickSize);
            }
            if (activeOverlay_ == OverlayId::TextFormat
                && !TextFormatPopupBounds().contains(hostPosition)) {
                CloseOverlay(OverlayId::TextFormat);
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

}  // namespace cnote::ui
