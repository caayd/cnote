#include "text/TextLayoutQt.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <memory>

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPalette>
#include <QString>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>

namespace cnote::text {

namespace {

// The stored font size is pixel-like at the 96-DPI reference scale; Qt wants
// points. Page zoom and DPR are applied by the caller, never baked in here.
constexpr float kReferenceDpi = 96.0f;
constexpr float kPointsPerPixel = 72.0f / kReferenceDpi;

QFont MakeFont(TextFontFamily family, float fontSize, bool bold, bool italic, bool underline) {
    QFont font;
    switch (family) {
    case TextFontFamily::Serif:
        font.setFamily(QStringLiteral("Times New Roman"));
        font.setStyleHint(QFont::Serif);
        break;
    case TextFontFamily::Monospace:
        font.setFamily(QStringLiteral("Consolas"));
        font.setStyleHint(QFont::Monospace);
        break;
    case TextFontFamily::Sans:
    default:
        font.setFamily(QStringLiteral("Segoe UI"));
        font.setStyleHint(QFont::SansSerif);
        break;
    }
    font.setPointSizeF(std::max(1.0f, fontSize) * kPointsPerPixel);
    font.setBold(bold);
    font.setItalic(italic);
    font.setUnderline(underline);
    return font;
}

Qt::Alignment AlignmentFor(TextAlignment alignment) {
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

std::u16string TrimTrailingLayoutWhitespace(std::u16string value) {
    while (!value.empty() && (value.back() == u' ' || value.back() == u'\t')) {
        value.pop_back();
    }
    return value;
}

// Normalizes CRLF/CR to a single paragraph separator while keeping a two-way
// offset map, so line starts and span ranges stay in the model's UTF-16 space.
struct NormalizedText {
    std::u16string text;
    std::vector<std::uint32_t> normalizedToOriginal;
    std::vector<std::uint32_t> originalToNormalized;
};

NormalizedText NormalizeParagraphs(std::u16string_view content) {
    NormalizedText result;
    result.text.reserve(content.size());
    result.normalizedToOriginal.reserve(content.size() + 1U);
    result.originalToNormalized.resize(content.size() + 1U);

    for (std::size_t index = 0; index < content.size();) {
        const std::uint32_t normalizedIndex = static_cast<std::uint32_t>(result.text.size());
        result.originalToNormalized[index] = normalizedIndex;
        if (content[index] == u'\r') {
            result.normalizedToOriginal.push_back(static_cast<std::uint32_t>(index));
            result.text.push_back(u'\n');
            if (index + 1U < content.size() && content[index + 1U] == u'\n') {
                result.originalToNormalized[index + 1U] = normalizedIndex;
                index += 2U;
            } else {
                index += 1U;
            }
        } else {
            result.normalizedToOriginal.push_back(static_cast<std::uint32_t>(index));
            result.text.push_back(content[index]);
            ++index;
        }
    }
    result.originalToNormalized[content.size()] = static_cast<std::uint32_t>(result.text.size());
    result.normalizedToOriginal.push_back(static_cast<std::uint32_t>(content.size()));
    return result;
}

struct BuiltDocument {
    std::unique_ptr<QTextDocument> document;
    NormalizedText normalized;
};

BuiltDocument BuildDocument(const PageText& text, float wrapWidth) {
    BuiltDocument built;
    built.normalized = NormalizeParagraphs(text.content);

    auto document = std::make_unique<QTextDocument>();
    document->setDocumentMargin(0.0);
    document->setDefaultFont(MakeFont(text.fontFamily, text.fontSize, text.bold, text.italic, text.underline));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setAlignment(AlignmentFor(text.alignment));
    document->setDefaultTextOption(option);
    document->setPlainText(QString::fromUtf16(
        built.normalized.text.data(),
        static_cast<qsizetype>(built.normalized.text.size())));

    const std::size_t normalizedSize = built.normalized.text.size();
    for (const PageTextStyleSpan& span : text.styleSpans) {
        const std::size_t originalStart = std::min<std::size_t>(span.start, text.content.size());
        const std::size_t originalEnd = std::min<std::size_t>(
            text.content.size(),
            static_cast<std::size_t>(span.start) + span.length);
        if (originalEnd <= originalStart) {
            continue;
        }
        const std::size_t start = std::min<std::size_t>(
            built.normalized.originalToNormalized[originalStart], normalizedSize);
        const std::size_t end = std::min<std::size_t>(
            built.normalized.originalToNormalized[originalEnd], normalizedSize);
        if (end <= start) {
            continue;
        }
        QTextCursor cursor(document.get());
        cursor.setPosition(static_cast<int>(start));
        cursor.setPosition(static_cast<int>(end), QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setFont(MakeFont(
            span.style.fontFamily,
            span.style.fontSize,
            span.style.bold,
            span.style.italic,
            span.style.underline));
        format.setForeground(QColor::fromRgba(span.style.color));
        cursor.mergeCharFormat(format);
    }

    document->setTextWidth(static_cast<qreal>(wrapWidth));
    // Force the layout so block/line geometry is available to callers.
    static_cast<void>(document->documentLayout()->documentSize());
    built.document = std::move(document);
    return built;
}

float ResolvedTextWidth(const PageText& text, float pageWidth) {
    static_cast<void>(pageWidth);
    if (text.width > 0.0f) {
        return std::max(1.0f, text.width);
    }

    const std::u16string measureText = TrimTrailingLayoutWhitespace(text.content);
    QTextDocument measure;
    measure.setDocumentMargin(0.0);
    measure.setDefaultFont(MakeFont(text.fontFamily, text.fontSize, text.bold, text.italic, text.underline));
    measure.setPlainText(QString::fromUtf16(measureText.data(), static_cast<qsizetype>(measureText.size())));
    return std::max(1.0f, static_cast<float>(measure.idealWidth()));
}

std::uint64_t HashValue(std::uint64_t hash, std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

struct CacheEntry {
    std::uint64_t signature = 0;
    QtTextLayout layout {};
};

std::deque<CacheEntry>& LayoutCache() {
    static std::deque<CacheEntry> cache;
    return cache;
}

std::uint64_t& LayoutMisses() {
    static std::uint64_t misses = 0;
    return misses;
}

constexpr std::size_t kMaxCachedLayouts = 128;

}  // namespace

std::uint64_t TextLayoutSignature(const PageText& text, float pageWidth) {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = HashValue(hash, std::bit_cast<std::uint32_t>(pageWidth));
    hash = HashValue(hash, std::bit_cast<std::uint32_t>(text.width));
    hash = HashValue(hash, std::bit_cast<std::uint32_t>(text.height));
    hash = HashValue(hash, std::bit_cast<std::uint32_t>(text.fontSize));
    hash = HashValue(hash, text.color);
    hash = HashValue(hash, static_cast<std::uint64_t>(text.alignment));
    hash = HashValue(hash, static_cast<std::uint64_t>(text.fontFamily));
    hash = HashValue(hash, text.bold);
    hash = HashValue(hash, text.italic);
    hash = HashValue(hash, text.underline);
    for (char16_t unit : text.content) {
        hash = HashValue(hash, static_cast<std::uint16_t>(unit));
    }
    for (const PageTextStyleSpan& span : text.styleSpans) {
        hash = HashValue(hash, span.start);
        hash = HashValue(hash, span.length);
        hash = HashValue(hash, std::bit_cast<std::uint32_t>(span.style.fontSize));
        hash = HashValue(hash, span.style.color);
        hash = HashValue(hash, static_cast<std::uint64_t>(span.style.fontFamily));
        hash = HashValue(hash, span.style.bold);
        hash = HashValue(hash, span.style.italic);
        hash = HashValue(hash, span.style.underline);
    }
    return hash;
}

QtTextLayout LayoutPageTextQt(const PageText& text, float pageWidth) {
    const std::uint64_t signature = TextLayoutSignature(text, pageWidth);
    std::deque<CacheEntry>& cache = LayoutCache();
    const auto existing = std::find_if(cache.begin(), cache.end(), [&](const CacheEntry& entry) {
        return entry.signature == signature;
    });
    if (existing != cache.end()) {
        CacheEntry entry = std::move(*existing);
        cache.erase(existing);
        cache.push_back(std::move(entry));
        return cache.back().layout;
    }

    ++LayoutMisses();

    const float wrapWidth = ResolvedTextWidth(text, pageWidth);
    BuiltDocument built = BuildDocument(text, wrapWidth);
    QTextDocument& document = *built.document;

    QtTextLayout layout;
    const QFontMetricsF defaultMetrics(document.defaultFont());
    layout.lineHeight = std::max(1, static_cast<int>(std::ceil(defaultMetrics.height())));
    for (const PageTextStyleSpan& span : text.styleSpans) {
        const QFontMetricsF spanMetrics(MakeFont(
            span.style.fontFamily,
            span.style.fontSize,
            span.style.bold,
            span.style.italic,
            span.style.underline));
        layout.lineHeight = std::max(layout.lineHeight, static_cast<int>(std::ceil(spanMetrics.height())));
    }

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QTextLayout* blockLayout = block.layout();
        if (blockLayout == nullptr) {
            continue;
        }
        for (int lineIndex = 0; lineIndex < blockLayout->lineCount(); ++lineIndex) {
            const QTextLine line = blockLayout->lineAt(lineIndex);
            const std::size_t normalizedStart = static_cast<std::size_t>(block.position() + line.textStart());
            const std::size_t normalizedEnd = normalizedStart + static_cast<std::size_t>(line.textLength());
            if (normalizedEnd >= built.normalized.normalizedToOriginal.size()) {
                continue;
            }
            QtTextLine out;
            out.start = built.normalized.normalizedToOriginal[normalizedStart];
            const std::uint32_t originalEnd = built.normalized.normalizedToOriginal[normalizedEnd];
            out.text = TrimTrailingLayoutWhitespace(text.content.substr(
                out.start,
                static_cast<std::size_t>(originalEnd - out.start)));
            layout.lines.push_back(std::move(out));
        }
    }

    layout.height = std::max(layout.lineHeight, static_cast<int>(layout.lines.size()) * layout.lineHeight);

    cache.push_back({ signature, layout });
    if (cache.size() > kMaxCachedLayouts) {
        cache.pop_front();
    }
    return layout;
}

QtTextBounds MeasurePageTextBoundsQt(const PageText& text, float pageWidth) {
    const QtTextLayout layout = LayoutPageTextQt(text, pageWidth);
    return {
        text.x,
        text.y,
        text.x + ResolvedTextWidth(text, pageWidth),
        text.y + std::max(static_cast<float>(layout.height), text.height),
    };
}

void DrawPageTextQt(QPainter& painter, const PageText& text, float pageWidth, float scale, float x, float y, float opacity) {
    if (text.content.empty()) {
        return;
    }

    const float wrapWidth = ResolvedTextWidth(text, pageWidth);
    BuiltDocument built = BuildDocument(text, wrapWidth);
    QTextDocument& document = *built.document;

    painter.save();
    painter.translate(x, y);
    painter.scale(scale, scale);
    painter.setOpacity(painter.opacity() * std::clamp(opacity, 0.0f, 1.0f));

    QAbstractTextDocumentLayout::PaintContext context;
    context.clip = QRectF(0.0, 0.0, document.textWidth(), document.size().height());
    context.palette.setColor(QPalette::Text, QColor::fromRgba(text.color));
    context.palette.setBrush(QPalette::Base, Qt::NoBrush);
    document.documentLayout()->draw(&painter, context);

    painter.restore();
}

std::uint64_t TextLayoutCacheMisses() {
    return LayoutMisses();
}

}  // namespace cnote::text
