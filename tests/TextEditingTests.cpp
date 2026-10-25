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
#include <QTextCursor>
#include <QTextDocument>

#include "Document.h"
#include "Unicode.h"
#include "latex/LatexEngine.h"
#include "text/TextEditingQt.h"
#include "text/TextLayoutQt.h"
#include "text/InlineMath.h"

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

    // --- inline math detection (Pandoc tex_math_dollars rules) --------------
    {
        using Ranges = std::vector<cnote::text::InlineMathRange>;
        struct MathCase {
            const char16_t* content;
            Ranges expected;
        };
        const MathCase cases[] = {
            { u"area is $x^2$ here", { { 8U, 13U } } },
            { u"$a$ and $b$", { { 0U, 3U }, { 8U, 11U } } },
            { u"$a$b$", { { 0U, 3U } } },
            { u"$5 and $10", {} },                       // prices
            { u"costs $5, maybe $6.", {} },              // closer before a digit
            { u"$x$5", {} },                             // closer followed by a digit
            { u"$ x$", {} },                             // space after the opener
            { u"$x $", {} },                             // space before the closer
            { u"$x $y$", { { 0U, 6U } } },               // skips an invalid closer
            { u"\\$x$", {} },                            // escaped opener
            { u"$a\\$b$", { { 0U, 6U } } },              // escaped dollar inside
            { u"\\\\$x$", { { 2U, 5U } } },              // escaped backslash, then math
            { u"$x\ny$", {} },                           // never spans a line break
            { u"$x\r\ny$ $z$", { { 7U, 10U } } },
            { u"$$x$$", {} },                            // display math is literal for now
            { u"$a$$b$", {} },                           // a $$ run ends the search
            { u"$", {} },
            { u"$$", {} },
            { u"lone $ sign", {} },
            { u"$\U0001F600$", { { 0U, 4U } } },         // surrogate pair inside
            { u"", {} },
        };
        for (const MathCase& test : cases) {
            const Ranges found = cnote::text::FindInlineMath(test.content);
            std::string description;
            for (const auto& range : found) {
                description += " [" + std::to_string(range.start) + "," + std::to_string(range.end) + ")";
            }
            Check(found == test.expected, "inline math in \"" + cnote::Utf16ToUtf8(test.content) + "\" found:" + description);
        }
        const Ranges source = cnote::text::FindInlineMath(u"see $\\frac{a}{b}$");
        Check(source.size() == 1U && source.front().SourceStart() == 5U && source.front().SourceLength() == 11U,
            "the formula source excludes both delimiters");
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

    // --- LaTeX engine: empty math groups must not crash ---------------------
    // MicroTeX's MathAtom dereferenced a null base for empty math groups, so
    // the live preview crashed the whole app on "$", whitespace-only source
    // or "$ $" mid-typing.  These must all parse to a (possibly empty)
    // formula instead of aborting.
    Check(cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT)),
        "latex engine initializes for the parse checks");
    if (cnote::latex::IsInitialized()) {
        for (const char16_t* source : { u"$", u" ", u"$ $", u"\\[$\\]", u"  $  " }) {
            Check(cnote::latex::Parse(std::u16string_view(source), 28.0f, true) != nullptr,
                std::string("empty math group parses without crashing: ") + cnote::Utf16ToUtf8(source));
        }
        Check(cnote::latex::Parse(u"\\frac{1}{2}", 28.0f, true) != nullptr,
            "a real formula still parses");

        // --- documents with formula placeholders read back to the model -----
        cnote::PageText mathText {};
        mathText.fontSize = 20.0f;
        mathText.color = 0xFF000000U;
        mathText.content = u"Area $x^2$ is\r\nbold $\\frac{a}{b}$ end";
        mathText.styleSpans.push_back({ 15U, 5U, { 20.0f, 0xFF000000U, cnote::TextFontFamily::ShantellSans, true, false, false } });
        mathText.styleSpans.push_back({ 20U, 13U, { 36.0f, 0xFF000000U, cnote::TextFontFamily::ShantellSans, true, false, false } });
        const cnote::PageText normalizedMath = cnote::text::NormalizeCommittedText(mathText);
        const auto spansMatch = [](const std::vector<cnote::PageTextStyleSpan>& got, const std::vector<cnote::PageTextStyleSpan>& want) {
            if (got.size() != want.size()) {
                return false;
            }
            for (std::size_t index = 0; index < got.size(); ++index) {
                if (got[index].start != want[index].start || got[index].length != want[index].length
                    || std::abs(got[index].style.fontSize - want[index].style.fontSize) > 0.01f
                    || got[index].style.bold != want[index].style.bold) {
                    return false;
                }
            }
            return true;
        };
        for (const float scale : { 1.0f, 1.5f }) {
            const std::string at = " at scale " + std::to_string(scale);
            const cnote::text::TextOffsetMap rendered =
                cnote::text::BuildTextOffsetMap(mathText, cnote::text::InlineMathMode::Rendered);
            Check(rendered.math.size() == 2U, "both formulas render" + at);
            QTextDocument mathDocument;
            mathDocument.setPlainText(QString::fromUtf16(rendered.normalized.data(), static_cast<qsizetype>(rendered.normalized.size())));
            cnote::text::ApplyPageTextFormatting(mathDocument, mathText, scale, cnote::text::InlineMathMode::Rendered);

            const cnote::text::DocumentContent read = cnote::text::ReadDocumentContent(mathDocument);
            Check(read.content == normalizedMath.content, "placeholders read back as their $...$ source" + at);
            Check(read.map.math.size() == 2U && read.map.math[1].start == 19U && read.map.math[1].end == 32U,
                "read map records formula ranges in model offsets" + at);
            Check(spansMatch(cnote::text::CapturePageTextFormatting(mathDocument, mathText, scale), normalizedMath.styleSpans),
                "spans over and around formulas capture back unchanged (placeholder size ignored)" + at);

            // The live editor will show the caret's formula as source: a mix
            // of expanded and rendered formulas must read back identically.
            QTextCursor expand(&mathDocument);
            expand.setPosition(5);
            expand.setPosition(6, QTextCursor::KeepAnchor);
            QTextCursor plainFormat(&mathDocument);
            plainFormat.setPosition(4);
            expand.insertText(QStringLiteral("$x^2$"), plainFormat.charFormat());
            const cnote::text::DocumentContent mixed = cnote::text::ReadDocumentContent(mathDocument);
            Check(mixed.content == normalizedMath.content, "mixed source and rendered formulas read back" + at);
            Check(mixed.map.math.size() == 1U && cnote::text::ModelOffsetForDocument(mixed.map, 19U) == 19U
                    && cnote::text::ModelOffsetForDocument(mixed.map, 20U) == 32U
                    && cnote::text::DocumentOffsetForModel(mixed.map, 25U) == 19U
                    && cnote::text::DocumentOffsetForModel(mixed.map, 32U) == 20U,
                "mixed document offsets map through the remaining placeholder" + at);
            Check(spansMatch(cnote::text::CapturePageTextFormatting(mathDocument, mathText, scale), normalizedMath.styleSpans),
                "mixed document spans capture back unchanged" + at);
        }
        cnote::latex::Shutdown();
    }

    std::printf(g_failures == 0 ? "ALL TEXT EDITING CHECKS PASSED\n" : "%d TEXT EDITING CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
