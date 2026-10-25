// Inline $...$ math in the text tool's editor (ui::InlineMathTextEdit).
//
// Drives real key events into the editor offscreen and asserts what the user
// sees and gets back: formulas render except the one the caret touches,
// undo/redo walk the typed source and never stop at a formula rendering or
// expanding, copy exports `$...$` source, prices stay text, and toolbar
// styling reaches rendered formulas.

#include <cstdio>
#include <filesystem>
#include <string>

#include <QApplication>
#include <QClipboard>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QImage>
#include <QTextDocument>
#include <QTextOption>
#include <QtTest/QTest>

#include "Document.h"
#include "Unicode.h"
#include "latex/LatexEngine.h"
#include "text/InlineMathQt.h"
#include "text/TextEditingQt.h"
#include "ui/InlineMathTextEdit.h"
#include "render/PageRenderer.h"
#include "text/TextLayoutQt.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

cnote::PageText MakeText(const std::u16string& content) {
    cnote::PageText text {};
    text.fontSize = 20.0f;
    text.color = 0xFF000000U;
    text.fontFamily = cnote::TextFontFamily::Sans;
    text.content = content;
    return text;
}

std::size_t RenderedCount(const cnote::ui::InlineMathTextEdit& editor) {
    return cnote::text::ReadDocumentContent(*editor.document()).map.math.size();
}

std::u16string Content(const cnote::ui::InlineMathTextEdit& editor) {
    return editor.CapturePageText().content;
}

std::string Quote(const std::u16string& value) {
    return "\"" + cnote::Utf16ToUtf8(value) + "\"";
}

void Press(cnote::ui::InlineMathTextEdit& editor, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::keyClick(&editor, key, modifiers);
    QApplication::processEvents();
}

void Type(cnote::ui::InlineMathTextEdit& editor, const QString& text) {
    QTest::keyClicks(&editor, text);
    QApplication::processEvents();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (!cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT))) {
        std::printf("FAIL: latex engine did not initialize\n");
        return 1;
    }

    cnote::ui::InlineMathTextEdit editor;
    editor.setAcceptRichText(false);
    editor.resize(700, 200);
    editor.show();
    editor.setFocus();
    QApplication::processEvents();

    // --- loading renders formulas away from the caret ------------------------
    editor.LoadPageText(MakeText(u"Area $x^2$ end"), 1.0f);
    Check(RenderedCount(editor) == 1U, "a loaded formula away from the caret renders");
    Check(Content(editor) == u"Area $x^2$ end", "capture returns the stored source, got " + Quote(Content(editor)));

    // --- typing: the formula being written stays source until left -----------
    Type(editor, QStringLiteral(" $y$"));
    Check(Content(editor) == u"Area $x^2$ end $y$", "typed formula is captured as source, got " + Quote(Content(editor)));
    Check(RenderedCount(editor) == 1U, "the formula the caret touches stays source");
    Press(editor, Qt::Key_Space);
    Check(RenderedCount(editor) == 2U, "moving past the closing $ renders the formula");

    // --- caret entering a rendered formula shows its source ------------------
    Press(editor, Qt::Key_Left);
    Check(RenderedCount(editor) == 1U, "arrowing onto a rendered formula expands it");
    Press(editor, Qt::Key_Left);
    Type(editor, QStringLiteral("^2"));
    Check(Content(editor) == u"Area $x^2$ end $y^2$ ", "edits inside an expanded formula land in its source, got " + Quote(Content(editor)));
    Press(editor, Qt::Key_End);
    Check(RenderedCount(editor) == 2U, "leaving the formula renders it again");

    // --- undo/redo walk typed source only ------------------------------------
    // Steps: " $y$" (one word), " " (word boundary), "^2" (new position).
    // The arrow-key expansions and collapses in between are not steps.
    const std::u16string expectedUndo[] = {
        u"Area $x^2$ end $y$ ",
        u"Area $x^2$ end $y$",
        u"Area $x^2$ end",
    };
    for (const std::u16string& expected : expectedUndo) {
        Press(editor, Qt::Key_Z, Qt::ControlModifier);
        Check(Content(editor) == expected, "undo restores " + Quote(expected) + ", got " + Quote(Content(editor)));
    }
    Check(!editor.CanUndoEdit(), "three undo steps reach the loaded text (formula rendering adds none)");
    Check(RenderedCount(editor) == 1U, "the undone text renders its formula again");
    for (int step = 0; step < 3; ++step) {
        Press(editor, Qt::Key_Y, Qt::ControlModifier);
    }
    Check(Content(editor) == u"Area $x^2$ end $y^2$ ", "redo replays every typed step, got " + Quote(Content(editor)));
    Check(!editor.CanRedoEdit(), "redo stops at the latest edit");

    // --- copy exports source --------------------------------------------------
    editor.selectAll();
    editor.copy();
    QApplication::processEvents();
    const std::u16string copied = QApplication::clipboard()->text().toStdU16String();
    Check(copied == u"Area $x^2$ end $y^2$ ", "copy exports $...$ source, got " + Quote(copied));

    // --- prices and other prose stay text -------------------------------------
    editor.LoadPageText(MakeText(u"costs"), 1.0f);
    Type(editor, QStringLiteral(" $5 and $10 today"));
    Check(RenderedCount(editor) == 0U && Content(editor) == u"costs $5 and $10 today",
        "prices never render as math, got " + Quote(Content(editor)));

    // --- toolbar styling reaches rendered formulas ----------------------------
    editor.LoadPageText(MakeText(u"a $x$ b"), 1.0f);
    editor.selectAll();
    const cnote::PageTextStyle bigBold { 30.0f, 0xFF000000U, cnote::TextFontFamily::Sans, true, false, false };
    editor.textCursor().mergeCharFormat(cnote::text::CharFormatForStyle(bigBold));
    QApplication::processEvents();
    const cnote::PageText styled = editor.CapturePageText();
    Check(styled.styleSpans.size() == 1U && styled.styleSpans.front().start == 0U
            && styled.styleSpans.front().length == styled.content.size()
            && std::abs(styled.styleSpans.front().style.fontSize - 30.0f) < 0.01f
            && styled.styleSpans.front().style.bold,
        "a style applied over a rendered formula captures as one span");
    double formulaSize = 0.0;
    QTextCursor probe(editor.document());
    for (int position = 1; position < editor.document()->characterCount(); ++position) {
        probe.setPosition(position);
        if (probe.charFormat().objectType() == cnote::text::kInlineMathObjectType) {
            formulaSize = probe.charFormat().doubleProperty(cnote::text::kInlineMathFontSizeProperty);
        }
    }
    Check(std::abs(formulaSize - 30.0) < 0.01, "the rendered formula follows the new text size");

    // --- the editor shows exactly what the commit renders ---------------------
    // Formula widths must count for line breaking, rules must antialias the
    // same way, and glyphs must rasterize at the device size wherever the DPR
    // lives (widget device transform vs. page-surface world transform).
    {
        cnote::Page page {};
        page.width = 420.0f;
        page.height = 140.0f;
        cnote::PageText note = MakeText(
            u"Area $x^2$ and $\\frac{a}{b}$ then $\\sum_{i=0}^n i$ more words wrap here.");
        note.fontSize = 18.0f;
        note.width = 400.0f;
        page.texts = { note };
        for (const float zoom : { 1.0f, 1.37f, 2.1f }) {
            cnote::ui::InlineMathTextEdit view;
            view.setFrameShape(QFrame::NoFrame);
            view.setStyleSheet(QStringLiteral("QTextEdit { background: white; border: none; }"));
            view.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            view.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            view.document()->setDocumentMargin(0.0);
            QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            view.document()->setDefaultTextOption(option);
            view.setLineWrapMode(QTextEdit::FixedPixelWidth);
            view.setLineWrapColumnOrWidth(cnote::text::TextWrapWidthPx(note, zoom));
            view.LoadPageText(note, zoom);
            view.resize(static_cast<int>(page.width * zoom), static_cast<int>(page.height * zoom));
            view.show();
            view.setReadOnly(true);  // read-only editors draw no caret in the grab
            QApplication::processEvents();
            const QImage edited = view.grab().toImage().convertToFormat(QImage::Format_ARGB32);

            cnote::render::PageRenderParams params;
            params.scale = zoom;
            params.devicePixelRatio = static_cast<float>(view.devicePixelRatioF());
            params.textPixelRatio = 1.0f;
            const QImage committed = cnote::render::RenderPageSurface(page, params).convertToFormat(QImage::Format_ARGB32);
            int worst = 0;
            for (int y = 0; y < std::min(edited.height(), committed.height()); ++y) {
                for (int x = 0; x < std::min(edited.width(), committed.width()); ++x) {
                    if (qAlpha(committed.pixel(x, y)) == 255) {
                        worst = std::max(worst, std::abs(qGray(committed.pixel(x, y)) - qGray(edited.pixel(x, y))));
                    }
                }
            }
            Check(cnote::text::ReadDocumentContent(*view.document()).map.math.size() == 3U,
                "every formula renders in the parity probe at zoom " + std::to_string(zoom));
            Check(worst <= 8, "the editor matches the committed render pixel for pixel at zoom " + std::to_string(zoom)
                + " dpr " + std::to_string(view.devicePixelRatioF()) + " (worst gray delta " + std::to_string(worst) + ")");
        }
    }

    cnote::latex::Shutdown();
    std::printf(g_failures == 0 ? "ALL INLINE MATH EDITOR CHECKS PASSED\n" : "%d INLINE MATH EDITOR CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
