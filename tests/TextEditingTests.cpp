// Self-contained Qt editing-services test (migration plan steps 3.6/5.5/5.6).
//
// Exercises the UTF-16 <-> QTextDocument offset mapping, style conversion,
// commit normalization and LaTeX editing decisions with the plan's real cases.

#include <cstdio>
#include <string>
#include <vector>

#include <QGuiApplication>
#include <QString>
#include <QTextCharFormat>
#include <QTextDocument>

#include "Document.h"
#include "text/TextEditingQt.h"
#include "text/TextLayoutQt.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

cnote::PageText MakeUnicodeText() {
    cnote::PageText text {};
    text.id = 1;
    text.x = 10.0f;
    text.y = 20.0f;
    text.width = 180.0f;
    text.height = 60.0f;
    text.fontSize = 20.0f;
    text.color = 0xFF0000FFU;
    text.content = u"A\U0001F600B\r\n\u03B1";
    text.styleSpans.push_back({ 3U, 1U, { 20.0f, 0xFFFF0000U, cnote::TextFontFamily::Sans, false, false, false } });
    return text;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    // --- style conversion round trip ----------------------------------------
    cnote::PageTextStyle style {};
    style.fontSize = 18.0f;
    style.color = 0xFF123456U;
    style.fontFamily = cnote::TextFontFamily::Serif;
    style.bold = true;
    style.italic = true;
    style.underline = true;
    const cnote::PageTextStyle roundTripped = cnote::text::StyleForCharFormat(
        cnote::text::CharFormatForStyle(style),
        cnote::PageTextStyle {});
    Check(roundTripped.fontFamily == style.fontFamily
            && roundTripped.bold == style.bold
            && roundTripped.italic == style.italic
            && roundTripped.underline == style.underline
            && roundTripped.color == style.color
            && std::abs(roundTripped.fontSize - style.fontSize) < 0.01f,
        "style survives the QTextCharFormat round trip");

    // --- UTF-16 offsets with a surrogate pair and CRLF ----------------------
    const cnote::PageText unicodeText = MakeUnicodeText();
    Check(unicodeText.content.size() == 7U, "unicode content is 7 UTF-16 code units");
    const cnote::text::TextOffsetMap map = cnote::text::BuildTextOffsetMap(unicodeText.content);
    Check(map.normalized == u"A\U0001F600B\n\u03B1", "CRLF maps to a single paragraph separator");
    Check(cnote::text::DocumentOffsetForModel(map, 3U) == 3U, "span start maps to document offset 3");
    Check(cnote::text::DocumentOffsetForModel(map, 4U) == 4U, "CRLF start maps to the separator");
    Check(cnote::text::DocumentOffsetForModel(map, 6U) == 5U, "alpha maps past the separator");
    Check(cnote::text::ModelOffsetForDocument(map, 5U) == 6U, "document offset maps back to the model");

    // --- commit normalization remaps spans together -------------------------
    cnote::PageText afterCrlf = unicodeText;
    afterCrlf.styleSpans.push_back({ 6U, 1U, { 20.0f, 0xFF00FF00U, cnote::TextFontFamily::Sans, false, false, false } });
    const cnote::PageText committed = cnote::text::NormalizeCommittedText(afterCrlf);
    Check(committed.content == u"A\U0001F600B\n\u03B1", "commit writes normalized LF");
    Check(committed.styleSpans.size() == 2U
            && committed.styleSpans[0].start == 3U && committed.styleSpans[0].length == 1U
            && committed.styleSpans[1].start == 5U && committed.styleSpans[1].length == 1U,
        "commit remaps every span into normalized offsets");
    Check(unicodeText.content == u"A\U0001F600B\r\n\u03B1",
        "model content keeps its CRLF until commit");

    // --- formatting capture matches the applied spans -----------------------
    {
        QTextDocument document;
        document.setDocumentMargin(0.0);
        document.setPlainText(QString::fromUtf16(map.normalized.data(), static_cast<qsizetype>(map.normalized.size())));
        cnote::text::ApplyPageTextFormatting(document, unicodeText);
        const std::vector<cnote::PageTextStyleSpan> captured = cnote::text::CapturePageTextFormatting(document, unicodeText);
        Check(captured.size() == 1U && captured.front().start == 3U && captured.front().length == 1U
                && captured.front().style.color == 0xFFFF0000U,
            "captured spans match the applied span");
    }

    // --- empty paragraph preservation ---------------------------------------
    {
        cnote::PageText paragraphs {};
        paragraphs.width = 180.0f;
        paragraphs.fontSize = 20.0f;
        paragraphs.content = u"a\r\n\r\nb";
        const cnote::text::QtTextLayout layout = cnote::text::LayoutPageTextQt(paragraphs, 200.0f);
        Check(layout.lines.size() == 3U, "empty paragraph is preserved as its own line");
        Check(layout.lines.size() == 3U && layout.lines[1].text.empty(), "the middle line is empty");
    }

    // --- empty-object deletion ----------------------------------------------
    Check(cnote::text::ShouldDeleteEmptyObject(u""), "empty content deletes the object");
    Check(!cnote::text::ShouldDeleteEmptyObject(u" "), "whitespace content keeps the object");

    // --- Ctrl+Backspace word deletion ---------------------------------------
    const cnote::text::TextRange deletion = cnote::text::WordDeletionRange(u"hello world  ", 13U);
    Check(deletion.start == 6U && deletion.end == 13U, "word deletion covers the trailing word and spaces");
    const cnote::text::TextRange atStart = cnote::text::WordDeletionRange(u"hello", 0U);
    Check(atStart.start == 0U && atStart.end == 0U, "word deletion at the start is empty");

    // --- LaTeX auto-pair insertion and skip ---------------------------------
    const auto inserted = cnote::text::ApplyLatexAutoPair(u"", 0U, 0U, u'{');
    Check(inserted.has_value() && inserted->content == u"{}" && inserted->cursor == 1U,
        "auto-pair inserts the closing character");
    const auto wrapped = cnote::text::ApplyLatexAutoPair(u"ab", 0U, 2U, u'(');
    Check(wrapped.has_value() && wrapped->content == u"(ab)" && wrapped->cursor == 3U,
        "auto-pair wraps the selection");
    const auto skipped = cnote::text::ApplyLatexAutoPair(u"{}", 1U, 1U, u'}');
    Check(skipped.has_value() && skipped->content == u"{}" && skipped->cursor == 2U,
        "auto-pair skips over an existing closing character");
    Check(!cnote::text::ApplyLatexAutoPair(u"{}", 0U, 0U, u'}').has_value(),
        "auto-pair does not skip when the next character differs");

    // --- LaTeX completion decisions -----------------------------------------
    const cnote::text::LatexCompletionDecision decision = cnote::text::LatexCompletionFor(u"x + \\fr", 7U);
    Check(decision.show && !decision.suggestions.empty()
            && decision.suggestions.front().command == u"\\frac"
            && decision.start == 4U && decision.end == 7U,
        "completion context and suggestions use UTF-16 offsets");
    const auto applied = cnote::text::ApplyLatexCompletion(u"x + \\fr", 7U, decision.suggestions.front());
    Check(applied.has_value() && applied->content == u"x + \\frac{}{}" && applied->cursor == 10U,
        "applying a completion inserts the command and places the cursor");
    Check(!cnote::text::LatexCompletionFor(u"x + \\\\fr", 8U).show,
        "escaped backslashes do not open completion");

    Check(cnote::text::LatexSuggestionActionForKey(Qt::Key_Tab) == cnote::text::LatexSuggestionKeyAction::Accept,
        "Tab accepts the suggestion");
    Check(cnote::text::LatexSuggestionActionForKey(Qt::Key_Return) == cnote::text::LatexSuggestionKeyAction::Accept,
        "Enter accepts the suggestion");
    Check(cnote::text::LatexSuggestionActionForKey(Qt::Key_Down) == cnote::text::LatexSuggestionKeyAction::SelectNext,
        "Down moves the suggestion selection");
    Check(cnote::text::LatexSuggestionActionForKey(Qt::Key_Escape) == cnote::text::LatexSuggestionKeyAction::Dismiss,
        "Escape dismisses the suggestion");

    std::printf(g_failures == 0 ? "ALL TEXT EDITING CHECKS PASSED\n" : "%d TEXT EDITING CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
