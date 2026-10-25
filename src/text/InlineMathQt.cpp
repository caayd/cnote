#include "text/InlineMathQt.h"

#include <algorithm>
#include <string>

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QObject>
#include <QPainter>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextObjectInterface>

#include "latex/LatexEngine.h"
#include "text/TextEditingQt.h"

namespace cnote::text {

namespace {

// Inline formulas use text style (smaller fractions and limits beside
// operators); MicroTeX lays everything out in display style otherwise.
std::u16string InlineSource(std::u16string_view source) {
    std::u16string wrapped = u"\\textstyle ";
    wrapped.append(source);
    return wrapped;
}

struct MathPlacement {
    latex::FormulaHandle formula;
    qreal width = 0.0;
    qreal ascent = 0.0;
    qreal depth = 0.0;
};

// Parsed at the layout size (text size * display scale): MicroTeX reports
// whole-unit metrics, so this keeps rounding below one layout pixel at
// every zoom.
MathPlacement MeasureInlineMath(const QString& source, float fontSize, float scale) {
    MathPlacement placement;
    if (source.isEmpty() || fontSize <= 0.0f || scale <= 0.0f) {
        return placement;
    }
    placement.formula = latex::Parse(InlineSource(source.toStdU16String()), fontSize * scale, false);
    if (placement.formula == nullptr) {
        return placement;
    }
    const latex::FormulaMetrics metrics = latex::Measure(placement.formula);
    placement.width = static_cast<qreal>(metrics.width);
    placement.ascent = static_cast<qreal>(metrics.height - metrics.depth);
    placement.depth = static_cast<qreal>(metrics.depth);
    return placement;
}

MathPlacement PlaceInlineMath(const QTextCharFormat& format) {
    return MeasureInlineMath(
        format.stringProperty(kInlineMathSourceProperty),
        static_cast<float>(format.doubleProperty(kInlineMathFontSizeProperty)),
        static_cast<float>(format.doubleProperty(kInlineMathScaleProperty)));
}

// Vertical placement: with AlignBaseline, QTextDocumentLayout gives an inline
// object descent = the placeholder font's descent and ascent = height -
// descent, and QTextLine draws it at baseline - ascent.  Reporting
// height = formula ascent + font descent therefore puts the formula baseline
// on the text baseline.  (AlignMiddle cannot be used: QTextLine re-centres
// such objects in the line box.)
class InlineMathHandler final : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    using QObject::QObject;

    QSizeF intrinsicSize(QTextDocument*, int, const QTextFormat& format) override {
        const QTextCharFormat charFormat = format.toCharFormat();
        const MathPlacement placement = PlaceInlineMath(charFormat);
        if (placement.formula == nullptr) {
            return {};
        }
        return { placement.width, placement.ascent + QFontMetricsF(charFormat.font()).descent() };
    }

    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument*, int, const QTextFormat& format) override {
        const QTextCharFormat charFormat = format.toCharFormat();
        const MathPlacement placement = PlaceInlineMath(charFormat);
        if (painter == nullptr || placement.formula == nullptr) {
            return;
        }
        const QColor color = charFormat.foreground().color();
        latex::Draw(placement.formula, *painter,
            static_cast<float>(rect.left()),
            static_cast<float>(rect.top()),
            1.0f,
            color.isValid() ? color.rgba() : 0xFF000000U);
    }
};

}  // namespace

std::vector<InlineMathRange> RenderableInlineMath(std::u16string_view content, float fontSize) {
    std::vector<InlineMathRange> ranges = FindInlineMath(content);
    std::erase_if(ranges, [&](const InlineMathRange& range) {
        return latex::Parse(InlineSource(content.substr(range.SourceStart(), range.SourceLength())), fontSize, false) == nullptr;
    });
    return ranges;
}

void RegisterInlineMathHandler(QTextDocument& document) {
    // One handler serves every document; registering again just replaces the
    // layout's entry.  Created lazily on the UI thread like the documents.
    static InlineMathHandler* handler = new InlineMathHandler();
    document.documentLayout()->registerHandler(kInlineMathObjectType, handler);
}

void MakeInlineMathFormat(QTextCharFormat& format, std::u16string_view source, float displayScale) {
    // The run's text size, captured before the placeholder font is touched.
    const PageTextStyle style = StyleForCharFormat(format, PageTextStyle {}, displayScale);
    format.setObjectType(kInlineMathObjectType);
    format.setProperty(kInlineMathSourceProperty, QString::fromUtf16(source.data(), static_cast<qsizetype>(source.size())));
    format.setProperty(kInlineMathScaleProperty, static_cast<double>(displayScale));
    format.setProperty(kInlineMathFontSizeProperty, static_cast<double>(style.fontSize));
    format.setVerticalAlignment(QTextCharFormat::AlignBaseline);

    // A formula deeper than the font's descent (fractions, nested
    // subscripts) would hang into the next line.  The placeholder draws no
    // glyph, so its font only reserves line space: grow it until its descent
    // covers the formula's depth.
    const MathPlacement placement = PlaceInlineMath(format);
    QFont font = format.font();
    qreal descent = QFontMetricsF(font).descent();
    for (int attempt = 0; attempt < 8 && placement.formula != nullptr && descent < placement.depth; ++attempt) {
        font.setPointSizeF(font.pointSizeF() * std::max(1.02, placement.depth / std::max(descent, 0.5)));
        descent = QFontMetricsF(font).descent();
    }
    format.setFontPointSize(font.pointSizeF());
}

}  // namespace cnote::text

#include "InlineMathQt.moc"
