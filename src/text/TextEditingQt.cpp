#include "text/TextEditingQt.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QHash>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QtGui/qtextobject.h>

#include "text/InlineMathQt.h"

// qt_add_resources initializer for the bundled text fonts (cnote_services is a
// static library, so the resource must be pulled in explicitly - same pattern
// as qInitResources_cnote_chrome_assets in ui/ChromeAssetsQt.cpp).
extern int qInitResources_cnote_text_fonts();
namespace cnote::text {

namespace {

// Points-per-page-unit lives in the header (kPointsPerPageUnit); callers like
// CanvasWidget convert the pending popup size with the same factor.
constexpr float kPointsPerPixel = kPointsPerPageUnit;

// Tab stops are page geometry like the font sizes: a fixed distance in page
// units, scaled with them.  Qt's default is a fixed 80 device-independent
// pixels whatever the zoom, so a tab measured a different share of the text
// at every zoom, and the page-unit layout (bounds, wrapping, PDF export)
// disagreed with the editor and the canvas.
constexpr qreal kTabStopPageUnits = 80.0;

PageTextStyle DefaultStyleFor(const PageText& text) {
    return {
        text.fontSize,
        text.color,
        text.fontFamily,
        text.bold,
        text.italic,
        text.underline,
    };
}

bool IsSpace(char16_t character) {
    return character == u' ' || character == u'\t' || character == u'\n' || character == u'\r';
}

// Registers the bundled Shantell Sans (SIL OFL 1.1, third_party/shantell-sans)
// with the application font database so the family resolves identically on
// machines without it installed.  addApplicationFont needs a QGuiApplication,
// so registration retries until one exists rather than latching a failure.
void EnsureTextFonts() {
    static bool registered = false;
    if (registered || QGuiApplication::instance() == nullptr) {
        return;
    }
    qInitResources_cnote_text_fonts();
    QFontDatabase::addApplicationFont(QStringLiteral(":/cnote/fonts/ShantellSans.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/cnote/fonts/ShantellSans-Italic.ttf"));
    registered = true;
}

}  // namespace

QString TextFontFamilyName(TextFontFamily family) {
    switch (family) {
    case TextFontFamily::Serif:
        return QStringLiteral("Times New Roman");
    case TextFontFamily::Monospace:
        return QStringLiteral("Consolas");
    case TextFontFamily::ShantellSans:
        EnsureTextFonts();
        return QStringLiteral("Shantell Sans");
    case TextFontFamily::Sans:
    default:
        return QStringLiteral("Segoe UI");
    }
}

QFont FontForStyle(const PageTextStyle& style, float displayScale) {
    QFont font;
    font.setFamily(TextFontFamilyName(style.fontFamily));
    switch (style.fontFamily) {
    case TextFontFamily::Serif:
        font.setStyleHint(QFont::Serif);
        break;
    case TextFontFamily::Monospace:
        font.setStyleHint(QFont::Monospace);
        break;
    case TextFontFamily::Sans:
    case TextFontFamily::ShantellSans:
    default:
        font.setStyleHint(QFont::SansSerif);
        break;
    }
    SetScaleLinearPointSize(font, std::max(1.0f, style.fontSize * std::max(0.01f, displayScale)) * kPointsPerPixel);
    font.setBold(style.bold);
    font.setItalic(style.italic);
    font.setUnderline(style.underline);
    // Committed text renders into transparent page surfaces, where Qt always
    // antialiases in grayscale.  Without this the editor (an opaque widget)
    // gets subpixel/LCD antialiasing wherever fontconfig enables it, so text
    // would visibly change colour fringes when an edit is committed.
    font.setStyleStrategy(QFont::NoSubpixelAntialias);
    return font;
}

QTextCharFormat CharFormatForStyle(const PageTextStyle& style, float displayScale) {
    QTextCharFormat format;
    format.setFont(FontForStyle(style, displayScale));
    format.setForeground(QColor::fromRgba(style.color));
    return format;
}

namespace {

// Requested over actual point size: the factor the rounded font's advances
// are off by.
double SizeRoundingFactor(const QFont& font, double pointSize) {
    const double actual = QFontInfo(font).pointSizeF();
    return actual > 0.0 ? pointSize / actual : 1.0;
}

// The height Qt lays one line of `font` out at.
double LaidOutLineHeight(const QFont& font) {
    QTextDocument document;
    document.setDocumentMargin(0.0);
    document.setDefaultFont(font);
    document.setPlainText(QStringLiteral("x"));
    static_cast<void>(document.size());
    const QTextLayout* layout = document.begin().layout();
    return layout != nullptr && layout->lineCount() > 0 ? layout->lineAt(0).height() : 0.0;
}

}  // namespace

void SetScaleLinearPointSize(QFont& font, double pointSize) {
    font.setPointSizeF(pointSize);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setLetterSpacing(QFont::PercentageSpacing, 100.0 * SizeRoundingFactor(font, pointSize));
}

void SetScaleLinearPointSize(QTextCharFormat& format, double pointSize) {
    // The rounding depends on the size alone, not the family.
    QFont probe;
    probe.setPointSizeF(pointSize);
    format.setFontPointSize(pointSize);
    format.setFontHintingPreference(QFont::PreferNoHinting);
    format.setFontLetterSpacingType(QFont::PercentageSpacing);
    format.setFontLetterSpacing(100.0 * SizeRoundingFactor(probe, pointSize));
}

double ScaleLinearLineDistance(const QFont& font) {
    // Measured once per font: every layout of the object asks again.
    static QHash<QString, double> cache;
    const QString key = font.key();
    if (const auto found = cache.constFind(key); found != cache.constEnd()) {
        return *found;
    }
    // A 1000 px reference rounds by at most 0.05%: scaled down, its line
    // height is the requested size's.
    QFont reference = font;
    reference.setPixelSize(1000);
    const double referencePoints = QFontInfo(reference).pointSizeF();
    const double exact = referencePoints > 0.0
        ? LaidOutLineHeight(reference) * font.pointSizeF() / referencePoints
        : LaidOutLineHeight(font);
    const double distance = exact - LaidOutLineHeight(font);
    cache.insert(key, distance);
    return distance;
}

PageTextStyle StyleForCharFormat(const QTextCharFormat& format, const PageTextStyle& fallback, float displayScale) {
    PageTextStyle style = fallback;
    const QFont font = format.font();
    const bool placeholderSize = format.objectType() == kInlineMathObjectType
        && format.hasProperty(kInlineMathFontSizeProperty)
        && std::abs(font.pointSizeF() - format.doubleProperty(kInlineMathPlaceholderPointSizeProperty)) < 1e-3;
    if (placeholderSize) {
        // A formula placeholder's font may be enlarged to reserve line space;
        // the run's real text size travels in its own property.
        style.fontSize = static_cast<float>(format.doubleProperty(kInlineMathFontSizeProperty));
    } else if (font.pointSizeF() > 0.0) {
        style.fontSize = static_cast<float>(font.pointSizeF()) / (kPointsPerPixel * std::max(0.01f, displayScale));
    }
    if (format.hasProperty(QTextFormat::ForegroundBrush)) {
        const QColor color = format.foreground().color();
        if (color.isValid()) {
            style.color = color.rgba();
        }
    }
    const QString family = font.family();
    if (family == QStringLiteral("Times New Roman")) {
        style.fontFamily = TextFontFamily::Serif;
    } else if (family == QStringLiteral("Consolas")) {
        style.fontFamily = TextFontFamily::Monospace;
    } else if (family == QStringLiteral("Shantell Sans")) {
        style.fontFamily = TextFontFamily::ShantellSans;
    } else if (family == QStringLiteral("Segoe UI")) {
        style.fontFamily = TextFontFamily::Sans;
    }
    style.bold = font.bold();
    style.italic = font.italic();
    style.underline = font.underline();
    return style;
}

void ApplyPageTextFormatting(QTextDocument& document, const PageText& text, float displayScale, InlineMathMode mathMode) {
    const PageTextStyle defaultStyle = DefaultStyleFor(text);
    document.setDefaultFont(FontForStyle(defaultStyle, displayScale));
    QTextOption option = document.defaultTextOption();
    option.setTabStopDistance(kTabStopPageUnits * std::max(0.01f, displayScale));
    document.setDefaultTextOption(option);

    QTextCursor cursor(&document);
    cursor.select(QTextCursor::Document);
    const QTextCharFormat defaultFormat = CharFormatForStyle(defaultStyle, displayScale);
    // Empty blocks (an emptied editor, a fresh line) take their typing format
    // from the block char format; without it Qt falls back to an unstyled
    // format and new text loses the object's colour, size and family.
    // Order matters: setBlockCharFormat clears the first block's layout
    // without scheduling a relayout, so a reopened editor painted nothing
    // until the next keystroke.  The whole-document setCharFormat after it
    // relayouts every block.
    cursor.setBlockCharFormat(defaultFormat);
    // Lines of the base font at the requested size's height (see
    // SetScaleLinearPointSize); Enter carries the block format to new lines.
    QTextBlockFormat lines;
    lines.setLineHeight(ScaleLinearLineDistance(FontForStyle(defaultStyle, displayScale)),
        QTextBlockFormat::LineDistanceHeight);
    cursor.mergeBlockFormat(lines);
    cursor.setCharFormat(defaultFormat);

    const TextOffsetMap map = BuildTextOffsetMap(text, mathMode);
    for (const PageTextStyleSpan& span : text.styleSpans) {
        const std::uint32_t start = DocumentOffsetForModel(map, span.start);
        const std::uint32_t end = DocumentOffsetForModel(map, span.start + span.length);
        if (end <= start) {
            continue;
        }
        QTextCursor spanCursor(&document);
        spanCursor.setPosition(static_cast<int>(start));
        spanCursor.setPosition(static_cast<int>(end), QTextCursor::KeepAnchor);
        spanCursor.mergeCharFormat(CharFormatForStyle(span.style, displayScale));
    }

    if (map.math.empty()) {
        return;
    }
    RegisterInlineMathHandler(document);
    for (const InlineMathRange& range : map.math) {
        const std::uint32_t placeholder = DocumentOffsetForModel(map, range.start);
        QTextCursor objectCursor(&document);
        objectCursor.setPosition(static_cast<int>(placeholder));
        objectCursor.setPosition(static_cast<int>(placeholder) + 1, QTextCursor::KeepAnchor);
        QTextCharFormat format = objectCursor.charFormat();
        MakeInlineMathFormat(format,
            std::u16string_view(text.content).substr(range.SourceStart(), range.SourceLength()),
            displayScale);
        objectCursor.setCharFormat(format);
    }
}

namespace {

// Shared span capture.  The public whole-document call passes the whole range;
// the editor's local update passes the inserted fragment range.  Keeping one
// loop is what makes the incremental result provably the same as a full
// capture (the editor also asserts that parity in its tests).
std::vector<PageTextStyleSpan> CaptureFormatting(const QTextDocument& document, const PageText& text,
    const TextOffsetMap& map, std::uint32_t rangeStart, std::uint32_t rangeEnd, float displayScale) {
    const PageTextStyle defaultStyle = DefaultStyleFor(text);
    std::vector<PageTextStyleSpan> spans;
    std::optional<PageTextStyleSpan> active;

    // Start at the first block the range touches: an edit near the end of a
    // long document must not walk every earlier block.
    QTextBlock block = document.findBlock(static_cast<int>(std::min<std::uint32_t>(
        rangeStart, static_cast<std::uint32_t>(document.characterCount()))));
    for (; block.isValid(); block = block.next()) {
        const std::uint32_t blockStart = static_cast<std::uint32_t>(block.position());
        if (blockStart >= rangeEnd) {
            break;
        }
        for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid() || fragment.length() == 0) {
                continue;
            }
            const std::uint32_t documentStart = static_cast<std::uint32_t>(fragment.position());
            const std::uint32_t documentEnd = documentStart + static_cast<std::uint32_t>(fragment.length());
            const std::uint32_t from = std::max(documentStart, rangeStart);
            const std::uint32_t to = std::min(documentEnd, rangeEnd);
            if (to <= from) {
                continue;
            }
            const PageTextStyle style = StyleForCharFormat(fragment.charFormat(), defaultStyle, displayScale);
            const std::uint32_t modelStart = ModelOffsetForDocument(map, from);
            const std::uint32_t modelEnd = ModelOffsetForDocument(map, to);
            if (modelEnd <= modelStart) {
                continue;
            }
            if (PageTextStyleEqual(style, defaultStyle)) {
                if (active.has_value()) {
                    spans.push_back(*active);
                    active.reset();
                }
                continue;
            }
            if (active.has_value() && PageTextStyleEqual(active->style, style)
                && active->start + active->length == modelStart) {
                active->length += modelEnd - modelStart;
            } else {
                if (active.has_value()) {
                    spans.push_back(*active);
                }
                active = PageTextStyleSpan { modelStart, modelEnd - modelStart, style };
            }
        }
    }
    if (active.has_value()) {
        spans.push_back(*active);
    }
    return spans;
}

}  // namespace

std::vector<PageTextStyleSpan> CapturePageTextFormatting(const QTextDocument& document, const PageText& text, float displayScale) {
    return CapturePageTextFormatting(document, text, ReadDocumentContent(document).map, displayScale);
}

std::vector<PageTextStyleSpan> CapturePageTextFormatting(const QTextDocument& document, const PageText& text,
    const TextOffsetMap& map, float displayScale) {
    return CaptureFormatting(document, text, map, 0U, std::numeric_limits<std::uint32_t>::max(), displayScale);
}

std::vector<PageTextStyleSpan> CapturePageTextFormattingRange(const QTextDocument& document, const PageText& text,
    const TextOffsetMap& map, std::uint32_t documentStart, std::uint32_t documentEnd, float displayScale) {
    return CaptureFormatting(document, text, map, documentStart, documentEnd, displayScale);
}

DocumentContent ReadDocumentContent(const QTextDocument& document) {
    DocumentContent read;
    TextOffsetMap& map = read.map;
    const auto appendModel = [&](char16_t character, std::uint32_t documentIndex) {
        read.content.push_back(character);
        map.modelToDocument.push_back(documentIndex);
    };
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid()) {
                continue;
            }
            const QTextCharFormat format = fragment.charFormat();
            const bool isMath = format.objectType() == kInlineMathObjectType;
            const std::u16string source = isMath
                ? format.stringProperty(kInlineMathSourceProperty).toStdU16String()
                : std::u16string();
            const QString text = fragment.text();
            for (qsizetype offset = 0; offset < text.size(); ++offset) {
                const std::uint32_t documentIndex = static_cast<std::uint32_t>(fragment.position() + offset);
                const char16_t character = text.at(offset).unicode();
                map.normalized.push_back(character);
                map.documentToModel.push_back(static_cast<std::uint32_t>(read.content.size()));
                if (isMath && character == u'\uFFFC') {
                    const std::uint32_t start = static_cast<std::uint32_t>(read.content.size());
                    appendModel(u'$', documentIndex);
                    for (const char16_t unit : source) {
                        appendModel(unit, documentIndex);
                    }
                    appendModel(u'$', documentIndex);
                    map.math.push_back({ start, static_cast<std::uint32_t>(read.content.size()) });
                } else {
                    // Same conversions as QTextDocument::toPlainText.
                    appendModel(character == u'\u00A0' ? u' ' : (character == u'\u2028' ? u'\n' : character), documentIndex);
                }
            }
        }
        if (block.next().isValid()) {
            const std::uint32_t separator = static_cast<std::uint32_t>(block.position() + block.length() - 1);
            map.normalized.push_back(u'\n');
            map.documentToModel.push_back(static_cast<std::uint32_t>(read.content.size()));
            appendModel(u'\n', separator);
        }
    }
    map.modelToDocument.push_back(static_cast<std::uint32_t>(map.normalized.size()));
    map.documentToModel.push_back(static_cast<std::uint32_t>(read.content.size()));
    return read;
}

std::u16string ReadDocumentTextRange(const QTextDocument& document,
    std::uint32_t documentStart, std::uint32_t documentEnd) {
    std::u16string content;
    if (documentEnd <= documentStart) {
        return content;
    }
    content.reserve(documentEnd - documentStart);
    const auto appendConverted = [&](char16_t character) {
        // Same conversions as QTextDocument::toPlainText.
        content.push_back(character == u'\u00A0' ? u' ' : (character == u'\u2028' ? u'\n' : character));
    };
    QTextBlock block = document.findBlock(static_cast<int>(std::min<std::uint32_t>(
        documentStart, static_cast<std::uint32_t>(document.characterCount()))));
    for (; block.isValid(); block = block.next()) {
        const std::uint32_t blockStart = static_cast<std::uint32_t>(block.position());
        if (blockStart >= documentEnd) {
            break;
        }
        for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid()) {
                continue;
            }
            const std::uint32_t fragmentStart = static_cast<std::uint32_t>(fragment.position());
            const std::uint32_t fragmentEnd = fragmentStart + static_cast<std::uint32_t>(fragment.length());
            const std::uint32_t from = std::max(fragmentStart, documentStart);
            const std::uint32_t to = std::min(fragmentEnd, documentEnd);
            if (to <= from) {
                continue;
            }
            const QTextCharFormat format = fragment.charFormat();
            const bool isMath = format.objectType() == kInlineMathObjectType;
            const std::u16string source = isMath
                ? format.stringProperty(kInlineMathSourceProperty).toStdU16String()
                : std::u16string();
            const QString text = fragment.text();
            for (std::uint32_t documentIndex = from; documentIndex < to; ++documentIndex) {
                const qsizetype offset = static_cast<qsizetype>(documentIndex - fragmentStart);
                if (offset >= text.size()) {
                    continue;
                }
                const char16_t character = text.at(offset).unicode();
                if (isMath && character == u'\uFFFC') {
                    content.push_back(u'$');
                    content.append(source);
                    content.push_back(u'$');
                } else {
                    appendConverted(character);
                }
            }
        }
        if (block.next().isValid()) {
            const std::uint32_t separator = blockStart + static_cast<std::uint32_t>(block.length()) - 1U;
            if (separator >= documentStart && separator < documentEnd) {
                content.push_back(u'\n');
            }
        }
    }
    return content;
}

TextOffsetMap BuildTextOffsetMap(std::u16string_view content) {
    return BuildTextOffsetMap(content, {});
}

TextOffsetMap BuildTextOffsetMap(std::u16string_view content, std::vector<InlineMathRange> math) {
    TextOffsetMap map;
    map.math = std::move(math);
    map.normalized.reserve(content.size());
    map.documentToModel.reserve(content.size() + 1U);
    map.modelToDocument.resize(content.size() + 1U);

    auto nextMath = map.math.cbegin();
    for (std::size_t index = 0; index < content.size();) {
        const std::uint32_t documentIndex = static_cast<std::uint32_t>(map.normalized.size());
        map.modelToDocument[index] = documentIndex;
        if (nextMath != map.math.cend() && nextMath->start == index) {
            // The whole formula collapses to one object character.
            map.documentToModel.push_back(static_cast<std::uint32_t>(index));
            map.normalized.push_back(u'\uFFFC');
            for (std::size_t inside = index + 1U; inside < nextMath->end; ++inside) {
                map.modelToDocument[inside] = documentIndex;
            }
            index = nextMath->end;
            ++nextMath;
        } else if (content[index] == u'\r') {
            map.documentToModel.push_back(static_cast<std::uint32_t>(index));
            map.normalized.push_back(u'\n');
            if (index + 1U < content.size() && content[index + 1U] == u'\n') {
                map.modelToDocument[index + 1U] = documentIndex;
                index += 2U;
            } else {
                index += 1U;
            }
        } else {
            map.documentToModel.push_back(static_cast<std::uint32_t>(index));
            map.normalized.push_back(content[index]);
            ++index;
        }
    }
    map.modelToDocument[content.size()] = static_cast<std::uint32_t>(map.normalized.size());
    map.documentToModel.push_back(static_cast<std::uint32_t>(content.size()));
    return map;
}

TextOffsetMap BuildTextOffsetMap(const PageText& text, InlineMathMode mode) {
    if (mode == InlineMathMode::Source) {
        return BuildTextOffsetMap(text.content);
    }
    return BuildTextOffsetMap(text.content, RenderableInlineMath(text.content, text.fontSize));
}

std::uint32_t DocumentOffsetForModel(const TextOffsetMap& map, std::uint32_t modelOffset) {
    if (map.modelToDocument.empty()) {
        return 0;
    }
    return map.modelToDocument[std::min<std::size_t>(modelOffset, map.modelToDocument.size() - 1U)];
}

std::uint32_t ModelOffsetForDocument(const TextOffsetMap& map, std::uint32_t documentOffset) {
    if (map.documentToModel.empty()) {
        return 0;
    }
    return map.documentToModel[std::min<std::size_t>(documentOffset, map.documentToModel.size() - 1U)];
}

PageText NormalizeCommittedText(const PageText& text) {
    const TextOffsetMap map = BuildTextOffsetMap(text.content);
    if (map.normalized == text.content) {
        return text;
    }

    PageText normalized = text;
    normalized.content = map.normalized;
    normalized.styleSpans.clear();
    for (const PageTextStyleSpan& span : text.styleSpans) {
        const std::uint32_t start = DocumentOffsetForModel(map, span.start);
        const std::uint32_t end = DocumentOffsetForModel(map, span.start + span.length);
        if (end <= start) {
            continue;
        }
        normalized.styleSpans.push_back({ start, end - start, span.style });
    }
    return normalized;
}

bool ShouldDeleteEmptyObject(std::u16string_view content) {
    return content.empty();
}

TextRange WordDeletionRange(std::u16string_view content, std::uint32_t cursor) {
    cursor = std::min<std::uint32_t>(cursor, static_cast<std::uint32_t>(content.size()));
    std::uint32_t start = cursor;
    while (start > 0U && IsSpace(content[start - 1U])) {
        --start;
    }
    while (start > 0U && !IsSpace(content[start - 1U])) {
        --start;
    }
    return { start, cursor };
}

std::optional<LatexEditResult> ApplyLatexAutoPair(
    std::u16string_view content,
    std::uint32_t selectionStart,
    std::uint32_t selectionEnd,
    char16_t typed) {
    const std::uint32_t size = static_cast<std::uint32_t>(content.size());
    selectionStart = std::min(selectionStart, size);
    selectionEnd = std::min(selectionEnd, size);
    if (selectionEnd < selectionStart) {
        std::swap(selectionStart, selectionEnd);
    }

    char16_t closing = u'\0';
    switch (typed) {
    case u'{':
        closing = u'}';
        break;
    case u'(':
        closing = u')';
        break;
    case u'[':
        closing = u']';
        break;
    case u'}':
    case u')':
    case u']':
        if (selectionStart == selectionEnd && selectionStart < size && content[selectionStart] == typed) {
            return LatexEditResult { std::u16string(content), selectionStart + 1U };
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }

    std::u16string result(content);
    result.insert(selectionStart, 1U, typed);
    result.insert(selectionEnd + 1U, 1U, closing);
    return LatexEditResult { std::move(result), selectionStart + 1U + (selectionEnd - selectionStart) };
}

LatexSuggestionKeyAction LatexSuggestionActionForKey(Qt::Key key) {
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab:
        return LatexSuggestionKeyAction::Accept;
    case Qt::Key_Up:
        return LatexSuggestionKeyAction::SelectPrevious;
    case Qt::Key_Down:
        return LatexSuggestionKeyAction::SelectNext;
    case Qt::Key_PageUp:
        return LatexSuggestionKeyAction::PageUp;
    case Qt::Key_PageDown:
        return LatexSuggestionKeyAction::PageDown;
    case Qt::Key_Escape:
        return LatexSuggestionKeyAction::Dismiss;
    default:
        return LatexSuggestionKeyAction::None;
    }
}

LatexCompletionDecision LatexCompletionFor(std::u16string_view content, std::uint32_t cursor) {
    LatexCompletionDecision decision;
    const auto context = latex::FindCompletionContext(content, cursor);
    if (!context.has_value()) {
        return decision;
    }
    decision.suggestions = latex::SuggestionsFor(context->prefix);
    if (decision.suggestions.empty()) {
        return decision;
    }
    decision.show = true;
    decision.start = static_cast<std::uint32_t>(context->start);
    decision.end = static_cast<std::uint32_t>(context->end);
    return decision;
}

std::optional<LatexEditResult> ApplyLatexCompletion(
    std::u16string_view content,
    std::uint32_t cursor,
    const latex::CompletionItem& item) {
    const auto context = latex::FindCompletionContext(content, cursor);
    if (!context.has_value()) {
        return std::nullopt;
    }
    const std::u16string insertion = item.insertion.empty() ? item.command : item.insertion;
    std::u16string result(content);
    result.replace(
        static_cast<std::size_t>(context->start),
        static_cast<std::size_t>(context->end - context->start),
        insertion);
    const std::uint32_t newCursor = static_cast<std::uint32_t>(context->start)
        + std::min<std::uint32_t>(static_cast<std::uint32_t>(item.cursorOffset), static_cast<std::uint32_t>(insertion.size()));
    return LatexEditResult { std::move(result), newCursor };
}

}  // namespace cnote::text
