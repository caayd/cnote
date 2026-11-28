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

#include "latex/LatexEngine.h"
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

std::uint64_t& DocumentBuilds() {
    static std::uint64_t builds = 0;
    return builds;
}

// Builds the document exactly like the live editor configures its own at
// view zoom `layoutScale` (CanvasWidget::ShowTextOverlay): fonts scaled by
// it, word-or-anywhere wrapping for fixed-width objects, no wrapping for
// auto-width ones, inline math rendered.  The text width is left unset.
// The offset map is built once and handed to the formatting pass, which
// also uses it to place spans and formula placeholders.
BuiltDocument BuildFormattedDocument(const PageText& text, float layoutScale) {
    BuiltDocument built;
    built.map = BuildTextOffsetMap(text, InlineMathMode::Rendered);

    auto document = std::make_unique<QTextDocument>();
    ++DocumentBuilds();
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

    ApplyPageTextFormatting(*document, text, built.map, layoutScale);
    built.document = std::move(document);
    return built;
}

std::uint64_t HashValue(std::uint64_t hash, std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

// Everything a layout, a resolved auto-width or a drawn document depends on
// except the stored width (which only selects the wrap mode and width below).
std::uint64_t TextIdentityHash(const PageText& text) {
    std::uint64_t hash = 14695981039346656037ULL;
    // Inline formulas are laid out into the document, so the layout identity
    // includes the engine lifetime it was produced in.
    hash = HashValue(hash, latex::EngineGeneration());
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

std::uint64_t LayoutSignature(const PageText& text) {
    return HashValue(TextIdentityHash(text), std::bit_cast<std::uint32_t>(text.width));
}

// Resolved widths in page units for auto-width text, keyed by content and
// style only: the stored width is zero on every path that measures.
struct WidthEntry {
    std::uint64_t signature = 0;
    float width = 0.0f;
};

std::deque<WidthEntry>& WidthCache() {
    static std::deque<WidthEntry> cache;
    return cache;
}

// A dense page resolves one width per text object.  The entries are tiny (a
// hash and a float plus deque bookkeeping), so the byte budget keeps several
// thousand: 512 KiB at the conservative 64 bytes charged per entry.
constexpr std::size_t kWidthEntryBytes = 64;
constexpr std::size_t kMaxWidthCacheBytes = 512U * 1024U;
constexpr std::size_t kMaxCachedWidths = kMaxWidthCacheBytes / kWidthEntryBytes;

float MeasureAutoTextWidth(const PageText& text) {
    const std::uint64_t key = TextIdentityHash(text);
    std::deque<WidthEntry>& cache = WidthCache();
    const auto existing = std::find_if(cache.begin(), cache.end(), [&](const WidthEntry& entry) {
        return entry.signature == key;
    });
    if (existing != cache.end()) {
        WidthEntry entry = *existing;
        cache.erase(existing);
        cache.push_back(entry);
        return cache.back().width;
    }

    // Measured on the formatted document, so style spans and rendered
    // formulas count; trailing blanks do not.
    PageText measured = text;
    measured.content = TrimTrailingLayoutWhitespace(text.content);
    const BuiltDocument built = BuildFormattedDocument(measured, 1.0f);
    const float width = std::max(1.0f, static_cast<float>(built.document->idealWidth()));
    cache.push_back({ key, width });
    while (cache.size() > 1U && cache.size() > kMaxCachedWidths) {
        cache.pop_front();
    }
    return width;
}

float ResolvedTextWidth(const PageText& text) {
    if (text.width > 0.0f) {
        return std::max(1.0f, text.width);
    }
    return MeasureAutoTextWidth(text);
}

BuiltDocument BuildDocument(const PageText& text, float layoutScale, float resolvedWidth) {
    BuiltDocument built = BuildFormattedDocument(text, layoutScale);
    QTextDocument& document = *built.document;
    // Auto-width text never wraps; the width only anchors paragraph alignment.
    document.setTextWidth(text.width > 0.0f
        ? static_cast<qreal>(TextWrapWidthPx(text, layoutScale))
        : static_cast<qreal>(resolvedWidth * layoutScale));
    // Force the layout so block/line geometry is available to callers.
    static_cast<void>(document.documentLayout()->documentSize());
    return built;
}

struct CacheEntry {
    std::uint64_t signature = 0;
    QtTextLayout layout {};
    std::size_t bytes = 0;
};

struct LayoutCacheState {
    std::deque<CacheEntry> entries;
    std::size_t bytes = 0;
};

LayoutCacheState& LayoutCache() {
    static LayoutCacheState cache;
    return cache;
}

std::uint64_t& LayoutMisses() {
    static std::uint64_t misses = 0;
    return misses;
}

// A page may draw every text object in order on each redraw, so the cache has
// to hold a dense page rather than a fixed number of entries.  The estimate
// charges the line strings and vectors conservatively; the budget keeps
// thousands of ordinary text objects while still bounding long documents.
constexpr std::size_t kMaxLayoutCacheBytes = 8U * 1024U * 1024U;
constexpr std::size_t kLayoutEntryOverheadBytes = 2048U;
constexpr std::size_t kLayoutBytesPerUnit = 4U;
constexpr std::size_t kLayoutBytesPerSpan = 32U;
constexpr std::size_t kLayoutBytesPerLine = 64U;

std::size_t EstimateLayoutBytes(const PageText& text, const QtTextLayout& layout) {
    return kLayoutEntryOverheadBytes
        + text.content.size() * kLayoutBytesPerUnit
        + text.styleSpans.size() * kLayoutBytesPerSpan
        + layout.lines.size() * kLayoutBytesPerLine;
}

// The cached layout for `text`, with its resolved width.  Never copies the
// line strings; callers that need a copy (the public API) make one.
const QtTextLayout& CachedLayout(const PageText& text) {
    // TextIdentityHash includes the engine generation, so a layout built
    // while the engine was down (formulas shown as source) is unreachable
    // after a recovery or re-initialization.
    const std::uint64_t key = LayoutSignature(text);
    LayoutCacheState& cache = LayoutCache();
    const auto existing = std::find_if(cache.entries.begin(), cache.entries.end(), [&](const CacheEntry& entry) {
        return entry.signature == key;
    });
    if (existing != cache.entries.end()) {
        CacheEntry entry = std::move(*existing);
        cache.entries.erase(existing);
        cache.entries.push_back(std::move(entry));
        return cache.entries.back().layout;
    }

    ++LayoutMisses();

    QtTextLayout layout;
    layout.width = ResolvedTextWidth(text);
    BuiltDocument built = BuildDocument(text, 1.0f, layout.width);
    QTextDocument& document = *built.document;

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QTextLayout* blockLayout = block.layout();
        if (blockLayout == nullptr) {
            continue;
        }
        // A line's pitch on the page: its own height (the tallest font or
        // formula on it) plus the block's scale-linear line distance.
        const QTextBlockFormat blockFormat = block.blockFormat();
        const float lineDistance = blockFormat.lineHeightType() == QTextBlockFormat::LineDistanceHeight
            ? static_cast<float>(blockFormat.lineHeight())
            : 0.0f;
        for (int lineIndex = 0; lineIndex < blockLayout->lineCount(); ++lineIndex) {
            const QTextLine line = blockLayout->lineAt(lineIndex);
            layout.lineHeight = std::max(layout.lineHeight, static_cast<float>(line.height()) + lineDistance);
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

    // Exactly what the text covers: the fonts are scale-linear, so this
    // page-unit height is the drawn height at every zoom.
    layout.height = std::max(layout.lineHeight, static_cast<float>(document.size().height()));

    const std::size_t bytes = EstimateLayoutBytes(text, layout);
    cache.entries.push_back({ key, std::move(layout), bytes });
    cache.bytes += bytes;
    while (cache.entries.size() > 1U && cache.bytes > kMaxLayoutCacheBytes) {
        cache.bytes -= cache.entries.front().bytes;
        cache.entries.pop_front();
    }
    return cache.entries.back().layout;
}

// Prepared documents for drawing, keyed by layout identity and the layout
// scale (fonts and wrap width divided by the pixel ratio).  A redraw at the
// same scale reuses the formatted document instead of rebuilding it.
struct DrawEntry {
    std::uint64_t signature = 0;
    std::uint32_t layoutScaleBits = 0;
    std::unique_ptr<QTextDocument> document;
    std::size_t bytes = 0;
};

struct DrawCacheState {
    std::deque<DrawEntry> entries;
    std::size_t bytes = 0;
};

DrawCacheState& DrawCache() {
    static DrawCacheState cache;
    return cache;
}

// Sized for a dense page redrawn in object order: every text box's prepared
// document has to survive until the next redraw, or the LRU becomes cyclic
// and each draw both misses and evicts.  The estimate is a fixed per-document
// cost plus the text and spans; the budget holds a few thousand small boxes
// or a few hundred long ones.
constexpr std::size_t kMaxDrawCacheBytes = 8U * 1024U * 1024U;
constexpr std::size_t kDrawDocumentOverheadBytes = 4096U;
constexpr std::size_t kDrawBytesPerUnit = 8U;
constexpr std::size_t kDrawBytesPerSpan = 64U;

std::size_t EstimateDrawDocumentBytes(const PageText& text) {
    return kDrawDocumentOverheadBytes
        + text.content.size() * kDrawBytesPerUnit
        + text.styleSpans.size() * kDrawBytesPerSpan;
}

QTextDocument& PreparedDrawDocument(const PageText& text, float layoutScale) {
    const std::uint64_t signature = LayoutSignature(text);
    const std::uint32_t layoutScaleBits = std::bit_cast<std::uint32_t>(layoutScale);
    DrawCacheState& cache = DrawCache();
    const auto existing = std::find_if(cache.entries.begin(), cache.entries.end(), [&](const DrawEntry& entry) {
        return entry.signature == signature && entry.layoutScaleBits == layoutScaleBits;
    });
    if (existing != cache.entries.end()) {
        DrawEntry entry = std::move(*existing);
        cache.entries.erase(existing);
        cache.entries.push_back(std::move(entry));
        return *cache.entries.back().document;
    }

    BuiltDocument built = BuildDocument(text, layoutScale, ResolvedTextWidth(text));
    const std::size_t bytes = EstimateDrawDocumentBytes(text);
    cache.entries.push_back({ signature, layoutScaleBits, std::move(built.document), bytes });
    cache.bytes += bytes;
    // At least one document stays: a single text object larger than the
    // budget is the minimum working set for drawing it.
    while (cache.entries.size() > 1U && cache.bytes > kMaxDrawCacheBytes) {
        cache.bytes -= cache.entries.front().bytes;
        cache.entries.pop_front();
    }
    return *cache.entries.back().document;
}

}  // namespace

std::uint64_t TextLayoutSignature(const PageText& text) {
    return LayoutSignature(text);
}

QtTextLayout LayoutPageTextQt(const PageText& text, float pageWidth) {
    // The page width does not affect how text lays out (auto-width text never
    // wraps); it is accepted for callers that reason in page coordinates.
    static_cast<void>(pageWidth);
    return CachedLayout(text);
}

QtTextBounds MeasurePageTextBoundsQt(const PageText& text, float pageWidth) {
    static_cast<void>(pageWidth);
    const QtTextLayout& layout = CachedLayout(text);
    return {
        text.x,
        text.y,
        text.x + layout.width,
        text.y + layout.height,
    };
}

int TextWrapWidthPx(const PageText& text, float layoutScale) {
    return std::max(1, static_cast<int>(std::lround(text.width * layoutScale)));
}

float MeasureUnwrappedTextWidth(const PageText& text) {
    // Auto-width text measures the same with or without its stored width, and
    // the bounds query already laid it out: reuse that entry.
    if (text.width <= 0.0f) {
        return CachedLayout(text).width;
    }
    PageText unwrapped = text;
    unwrapped.width = 0.0f;
    return CachedLayout(unwrapped).width;
}

float MaxAutoTextWidth(float viewWidthPx, float scale) {
    if (scale <= 0.0f) {
        return kTextMaxWrapWidth;
    }
    const float viewWidth = viewWidthPx - 2.0f * kTextViewportPaddingPx;
    const float capped = scale < 1.0f
        ? std::min(kTextMaxWrapWidth / scale, kTextMaxWrapWidthZoomedOut)
        : kTextMaxWrapWidth;
    return std::max(kMinTextWidth, viewWidth > 0.0f ? std::min(viewWidth / scale, capped) : capped);
}

PageText FitTextBox(const PageText& current, PageText next, float maxAutoWidth) {
    const QtTextBounds before = MeasurePageTextBoundsQt(current, 0.0f);
    const float beforeWidth = before.right - before.left;
    const float beforeHeight = before.bottom - before.top;
    // A text already wider than the limit (zoomed in since it grew) keeps
    // growing, as in Excalidraw's refreshTextDimensions.
    if (current.width <= 0.0f && next.width <= 0.0f && maxAutoWidth > 0.0f
        && beforeWidth <= maxAutoWidth && MeasureUnwrappedTextWidth(next) > maxAutoWidth) {
        next.width = std::max(kMinTextWidth, maxAutoWidth);
    }
    const QtTextBounds after = MeasurePageTextBoundsQt(next, 0.0f);
    const float afterWidth = after.right - after.left;
    const float afterHeight = after.bottom - after.top;

    // The anchor is the point of the top edge the alignment pins: left end,
    // middle or right end.  The text rotates about its box centre, which
    // moves as the box grows, so the anchor is held fixed on the page (in
    // rotated space) and the new top-left solved from it.
    const float ratio = next.alignment == TextAlignment::Center ? 0.5f
        : (next.alignment == TextAlignment::Right ? 1.0f : 0.0f);
    const float radians = next.rotationDegrees * 0.017453292519943295f;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const auto rotate = [cosine, sine](float x, float y) {
        return Vec2 { x * cosine - y * sine, x * sine + y * cosine };
    };
    // The anchor's offset from the top-left, before and after; the box moves
    // by their difference, which is exactly zero when its size did not
    // change (an unchanged commit must leave the object bit-identical).
    const auto anchorOffset = [&](float width, float height) {
        const Vec2 fromCentre = rotate(width * (ratio - 0.5f), -height * 0.5f);
        return Vec2 { width * 0.5f + fromCentre.x, height * 0.5f + fromCentre.y };
    };
    const Vec2 anchorBefore = anchorOffset(beforeWidth, beforeHeight);
    const Vec2 anchorAfter = anchorOffset(afterWidth, afterHeight);
    next.x = current.x + (anchorBefore.x - anchorAfter.x);
    next.y = current.y + (anchorBefore.y - anchorAfter.y);
    return next;
}

void DrawPageTextQt(QPainter& painter, const PageText& text, float pageWidth, float scale, float pixelRatio,
    float x, float y, float opacity) {
    static_cast<void>(pageWidth);
    if (text.content.empty()) {
        return;
    }

    const float ratio = std::max(0.0001f, pixelRatio);
    QTextDocument& document = PreparedDrawDocument(text, std::max(0.01f, scale / ratio));

    painter.save();
    painter.translate(x, y);
    painter.scale(ratio, ratio);
    painter.setOpacity(painter.opacity() * std::clamp(opacity, 0.0f, 1.0f));

    QAbstractTextDocumentLayout::PaintContext context;
    // Scale-linear fonts keep lines to the page-unit width to well under a
    // pixel; what remains may run a hair past it, so it is not clipped.
    context.clip = QRectF(0.0, 0.0, std::max(document.textWidth(), document.idealWidth()), document.size().height());
    context.palette.setColor(QPalette::Text, QColor::fromRgba(text.color));
    context.palette.setBrush(QPalette::Base, Qt::NoBrush);
    document.documentLayout()->draw(&painter, context);

    painter.restore();
}

std::uint64_t TextLayoutCacheMisses() {
    return LayoutMisses();
}

std::uint64_t TextDocumentBuilds() {
    return DocumentBuilds();
}

}  // namespace cnote::text
