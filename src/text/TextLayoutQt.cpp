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
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>

#include "text/TextEditingQt.h"

namespace cnote::text {

namespace {

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

struct BuiltDocument {
    std::unique_ptr<QTextDocument> document;
    TextOffsetMap map;
};

// Builds the document exactly like the live editor configures its own at
// view zoom `layoutScale` (CanvasWidget::ShowTextOverlay): fonts scaled by
// it, word-or-anywhere wrapping for fixed-width objects, no wrapping for
// auto-width ones, inline math rendered.  The text width is left unset.
BuiltDocument BuildFormattedDocument(const PageText& text, float layoutScale) {
    BuiltDocument built;
    built.map = BuildTextOffsetMap(text, InlineMathMode::Rendered);

    auto document = std::make_unique<QTextDocument>();
    document->setDocumentMargin(0.0);
    QTextOption option;
    option.setWrapMode(text.width > 0.0f
        ? QTextOption::WrapAtWordBoundaryOrAnywhere
        : QTextOption::NoWrap);
    option.setAlignment(AlignmentFor(text.alignment));
    document->setDefaultTextOption(option);
    document->setPlainText(QString::fromUtf16(
        built.map.normalized.data(),
        static_cast<qsizetype>(built.map.normalized.size())));

    ApplyPageTextFormatting(*document, text, layoutScale, InlineMathMode::Rendered);
    built.document = std::move(document);
    return built;
}

float ResolvedTextWidth(const PageText& text, float pageWidth) {
    static_cast<void>(pageWidth);
    if (text.width > 0.0f) {
        return std::max(1.0f, text.width);
    }

    // Measured on the formatted document, so style spans and rendered
    // formulas count; trailing blanks do not.
    PageText measured = text;
    measured.content = TrimTrailingLayoutWhitespace(text.content);
    const BuiltDocument built = BuildFormattedDocument(measured, 1.0f);
    return std::max(1.0f, static_cast<float>(built.document->idealWidth()));
}

BuiltDocument BuildDocument(const PageText& text, float layoutScale) {
    BuiltDocument built = BuildFormattedDocument(text, layoutScale);
    QTextDocument& document = *built.document;
    // Auto-width text never wraps; the width only anchors paragraph alignment.
    document.setTextWidth(text.width > 0.0f
        ? static_cast<qreal>(TextWrapWidthPx(text, layoutScale))
        : static_cast<qreal>(ResolvedTextWidth(text, 0.0f) * layoutScale));
    // Force the layout so block/line geometry is available to callers.
    static_cast<void>(document.documentLayout()->documentSize());
    return built;
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

    BuiltDocument built = BuildDocument(text, 1.0f);
    QTextDocument& document = *built.document;

    QtTextLayout layout;
    const QFontMetricsF defaultMetrics(document.defaultFont());
    layout.lineHeight = std::max(1, static_cast<int>(std::ceil(defaultMetrics.height())));
    for (const PageTextStyleSpan& span : text.styleSpans) {
        const QFontMetricsF spanMetrics(FontForStyle(span.style));
        layout.lineHeight = std::max(layout.lineHeight, static_cast<int>(std::ceil(spanMetrics.height())));
    }

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QTextLayout* blockLayout = block.layout();
        if (blockLayout == nullptr) {
            continue;
        }
        for (int lineIndex = 0; lineIndex < blockLayout->lineCount(); ++lineIndex) {
            const QTextLine line = blockLayout->lineAt(lineIndex);
            const std::size_t documentStart = static_cast<std::size_t>(block.position() + line.textStart());
            const std::size_t documentEnd = documentStart + static_cast<std::size_t>(line.textLength());
            if (documentEnd >= built.map.documentToModel.size()) {
                continue;
            }
            QtTextLine out;
            out.start = built.map.documentToModel[documentStart];
            const std::uint32_t modelEnd = built.map.documentToModel[documentEnd];
            out.text = TrimTrailingLayoutWhitespace(text.content.substr(
                out.start,
                static_cast<std::size_t>(modelEnd - out.start)));
            layout.lines.push_back(std::move(out));
        }
    }

    // Lines holding a tall inline formula grow past the font line height;
    // the laid-out document height covers them.
    layout.height = std::max({
        layout.lineHeight,
        static_cast<int>(layout.lines.size()) * layout.lineHeight,
        static_cast<int>(std::ceil(document.size().height())),
    });

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

int TextWrapWidthPx(const PageText& text, float layoutScale) {
    return std::max(1, static_cast<int>(std::lround(text.width * layoutScale)));
}

void DrawPageTextQt(QPainter& painter, const PageText& text, float pageWidth, float scale, float pixelRatio,
    float x, float y, float opacity) {
    static_cast<void>(pageWidth);
    if (text.content.empty()) {
        return;
    }

    const float ratio = std::max(0.0001f, pixelRatio);
    BuiltDocument built = BuildDocument(text, std::max(0.01f, scale / ratio));
    QTextDocument& document = *built.document;

    painter.save();
    painter.translate(x, y);
    painter.scale(ratio, ratio);
    painter.setOpacity(painter.opacity() * std::clamp(opacity, 0.0f, 1.0f));

    QAbstractTextDocumentLayout::PaintContext context;
    // Auto-width lines may run a little past the page-unit width once fonts
    // are rounded at the layout scale; the editor shows them, so do we.
    context.clip = QRectF(0.0, 0.0, std::max(document.textWidth(), document.idealWidth()), document.size().height());
    context.palette.setColor(QPalette::Text, QColor::fromRgba(text.color));
    context.palette.setBrush(QPalette::Base, Qt::NoBrush);
    document.documentLayout()->draw(&painter, context);

    painter.restore();
}

std::uint64_t TextLayoutCacheMisses() {
    return LayoutMisses();
}

}  // namespace cnote::text
