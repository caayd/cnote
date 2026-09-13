#include "text/TextEditingQt.h"

#include <algorithm>
#include <cmath>

#include <QColor>
#include <QFont>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QtGui/qtextobject.h>

namespace cnote::text {

namespace {

// The stored font size is pixel-like at the 96-DPI reference scale; Qt wants
// points. Page zoom and DPR are applied by the caller, never baked in here.
constexpr float kReferenceDpi = 96.0f;
constexpr float kPointsPerPixel = 72.0f / kReferenceDpi;

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

}  // namespace

QFont FontForStyle(const PageTextStyle& style) {
    QFont font;
    switch (style.fontFamily) {
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
    font.setPointSizeF(std::max(1.0f, style.fontSize) * kPointsPerPixel);
    font.setBold(style.bold);
    font.setItalic(style.italic);
    font.setUnderline(style.underline);
    return font;
}

QTextCharFormat CharFormatForStyle(const PageTextStyle& style) {
    QTextCharFormat format;
    format.setFont(FontForStyle(style));
    format.setForeground(QColor::fromRgba(style.color));
    return format;
}

PageTextStyle StyleForCharFormat(const QTextCharFormat& format, const PageTextStyle& fallback) {
    PageTextStyle style = fallback;
    const QFont font = format.font();
    if (font.pointSizeF() > 0.0) {
        style.fontSize = static_cast<float>(font.pointSizeF()) / kPointsPerPixel;
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
    } else if (family == QStringLiteral("Segoe UI")) {
        style.fontFamily = TextFontFamily::Sans;
    }
    style.bold = font.bold();
    style.italic = font.italic();
    style.underline = font.underline();
    return style;
}

void ApplyPageTextFormatting(QTextDocument& document, const PageText& text) {
    const PageTextStyle defaultStyle = DefaultStyleFor(text);
    document.setDefaultFont(FontForStyle(defaultStyle));

    QTextCursor cursor(&document);
    cursor.select(QTextCursor::Document);
    cursor.setCharFormat(CharFormatForStyle(defaultStyle));

    const TextOffsetMap map = BuildTextOffsetMap(text.content);
    for (const PageTextStyleSpan& span : text.styleSpans) {
        const std::uint32_t start = DocumentOffsetForModel(map, span.start);
        const std::uint32_t end = DocumentOffsetForModel(map, span.start + span.length);
        if (end <= start) {
            continue;
        }
        QTextCursor spanCursor(&document);
        spanCursor.setPosition(static_cast<int>(start));
        spanCursor.setPosition(static_cast<int>(end), QTextCursor::KeepAnchor);
        spanCursor.mergeCharFormat(CharFormatForStyle(span.style));
    }
}

std::vector<PageTextStyleSpan> CapturePageTextFormatting(const QTextDocument& document, const PageText& text) {
    const PageTextStyle defaultStyle = DefaultStyleFor(text);
    const TextOffsetMap map = BuildTextOffsetMap(text.content);
    std::vector<PageTextStyleSpan> spans;
    std::optional<PageTextStyleSpan> active;

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid() || fragment.length() == 0) {
                continue;
            }
            const PageTextStyle style = StyleForCharFormat(fragment.charFormat(), defaultStyle);
            const std::uint32_t documentStart = static_cast<std::uint32_t>(fragment.position());
            const std::uint32_t documentEnd = documentStart + static_cast<std::uint32_t>(fragment.length());
            const std::uint32_t modelStart = ModelOffsetForDocument(map, documentStart);
            const std::uint32_t modelEnd = ModelOffsetForDocument(map, documentEnd);
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

TextOffsetMap BuildTextOffsetMap(std::u16string_view content) {
    TextOffsetMap map;
    map.normalized.reserve(content.size());
    map.documentToModel.reserve(content.size() + 1U);
    map.modelToDocument.resize(content.size() + 1U);

    for (std::size_t index = 0; index < content.size();) {
        const std::uint32_t documentIndex = static_cast<std::uint32_t>(map.normalized.size());
        map.modelToDocument[index] = documentIndex;
        if (content[index] == u'\r') {
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
