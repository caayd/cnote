#include "text/InlineMathQt.h"

#include <algorithm>
#include <cstddef>
#include <deque>
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

// Measured placements are reused by the layout's intrinsic-size query, its
// draw callback and the format that installs the placeholder, so one formula
// at one layout size parses and measures once per engine lifetime.  Bounded:
// a text object with many formulas cannot grow this without limit.
struct PlacementEntry {
    QString source;
    float fontSize = 0.0f;
    float scale = 0.0f;
    MathPlacement placement;
};

std::deque<PlacementEntry>& PlacementCache() {
    static std::deque<PlacementEntry> cache;
    return cache;
}

std::uint64_t& PlacementCacheGeneration() {
    static std::uint64_t generation = 0;
    return generation;
}

constexpr std::size_t kMaxCachedPlacements = 64;

// Parsed at the layout size (text size * display scale): MicroTeX reports
// whole-unit metrics, so this keeps rounding below one layout pixel at
// every zoom.
MathPlacement MeasureInlineMath(const QString& source, float fontSize, float scale) {
    MathPlacement placement;
    if (source.isEmpty() || fontSize <= 0.0f || scale <= 0.0f) {
        return placement;
    }

    std::deque<PlacementEntry>& cache = PlacementCache();
    const std::uint64_t generation = latex::EngineGeneration();
    if (PlacementCacheGeneration() != generation) {
        // Parsed formulae belong to one engine lifetime.
        cache.clear();
        PlacementCacheGeneration() = generation;
    }
    const auto existing = std::find_if(cache.begin(), cache.end(), [&](const PlacementEntry& entry) {
        return entry.fontSize == fontSize && entry.scale == scale && entry.source == source;
    });
    if (existing != cache.end()) {
        PlacementEntry entry = std::move(*existing);
        cache.erase(existing);
        cache.push_back(std::move(entry));
        return cache.back().placement;
    }

    placement.formula = latex::Parse(InlineSource(source.toStdU16String()), fontSize * scale, false);
    if (placement.formula == nullptr) {
        // Not cached: a missing engine or a failed parse may recover.
        return placement;
    }
    const latex::FormulaMetrics metrics = latex::Measure(placement.formula);
    placement.width = static_cast<qreal>(metrics.width);
    placement.ascent = static_cast<qreal>(metrics.height - metrics.depth);
    placement.depth = static_cast<qreal>(metrics.depth);

    cache.push_back({ source, fontSize, scale, placement });
    if (cache.size() > kMaxCachedPlacements) {
        cache.pop_front();
    }
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
        // Formula rules (fraction bars, radicals) are filled shapes: draw them
        // antialiased whatever the host painter's hints, so the editor and
        // the page surface rasterize them identically.
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        latex::Draw(placement.formula, *painter,
            static_cast<float>(rect.left()),
            static_cast<float>(rect.top()),
            1.0f,
            color.isValid() ? color.rgba() : 0xFF000000U);
        painter->restore();
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
    const QString formulaSource =
        QString::fromUtf16(source.data(), static_cast<qsizetype>(source.size()));
    // Parse before turning the run into an object. A failed parse (the engine
    // is uninitialized or failed) leaves the run unchanged instead of
    // installing a zero-size object whose formula-less layout Qt would cache
    // and reuse after the engine recovers.
    const MathPlacement placement = MeasureInlineMath(formulaSource, style.fontSize, displayScale);
    if (placement.formula == nullptr) {
        return;
    }
    format.setObjectType(kInlineMathObjectType);
    format.setProperty(kInlineMathSourceProperty, formulaSource);
    format.setProperty(kInlineMathScaleProperty, static_cast<double>(displayScale));
    format.setProperty(kInlineMathFontSizeProperty, static_cast<double>(style.fontSize));
    format.setVerticalAlignment(QTextCharFormat::AlignBaseline);

    // A formula deeper than the font's descent (fractions, nested
    // subscripts) would hang into the next line.  The placeholder draws no
    // glyph, so its font only reserves line space: grow it until its descent
    // covers the formula's depth.
    QFont font = format.font();
    qreal descent = QFontMetricsF(font).descent();
    for (int attempt = 0; attempt < 8 && descent < placement.depth; ++attempt) {
        font.setPointSizeF(font.pointSizeF() * std::max(1.02, placement.depth / std::max(descent, 0.5)));
        descent = QFontMetricsF(font).descent();
    }
    format.setFontPointSize(font.pointSizeF());
    format.setProperty(kInlineMathPlaceholderPointSizeProperty, font.pointSizeF());
}

}  // namespace cnote::text

#include "InlineMathQt.moc"
