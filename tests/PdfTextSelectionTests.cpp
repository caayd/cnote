// Offscreen PDF text selection test: opens a generated PDF through the real
// QtDocumentController + CanvasWidget, drags across the baked-in text and
// asserts the character boxes highlight, the selection commits as pdfText and
// the copied text is readable - the restored reference behavior
// (Selection.cpp:1360-1452, Clipboard.cpp:73-113).

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QImage>
#include <QMouseEvent>

#include "qt/QtDocumentController.h"
#include "shell/AppManager.h"
#include "document/SelectionClipboardQt.h"
#include "ui/CanvasWidget.h"

namespace {

namespace ui = cnote::ui;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// A single-page PDF with two text lines, same shape as the background tests.
std::vector<std::uint8_t> BuildTextPdf() {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [4 0 R] /Count 1 >>");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    const std::string content =
        "BT /F1 24 Tf 72 720 Td (Hello pdf world) Tj ET\n"
        "BT /F1 24 Tf 72 660 Td (Second line here) Tj ET\n";
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 5 0 R"
                      " /Resources << /Font << /F1 3 0 R >> >> >>");
    objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");

    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets(1, 0);
    for (std::size_t index = 0; index < objects.size(); ++index) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(index + 1U) + " 0 obj\n" + objects[index] + "\nendobj\n";
    }
    const std::size_t xrefOffset = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1U) + "\n0000000000 65535 f \n";
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        std::string offset = std::to_string(offsets[index]);
        pdf += std::string(10U - offset.size(), '0') + offset + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1U) + " /Root 1 0 R >>\nstartxref\n";
    pdf += std::to_string(xrefOffset) + "\n%%EOF\n";
    return std::vector<std::uint8_t>(pdf.begin(), pdf.end());
}

void SendMouse(
    QWidget& widget, QEvent::Type type, const QPointF& logical,
    Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, logical, widget.mapToGlobal(logical.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

// Translucent blue highlight pixels over the white page are (64,150,255)@96.
int HighlightPixels(const QImage& image) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            const int r = qRed(pixel);
            const int g = qGreen(pixel);
            const int b = qBlue(pixel);
            if (b > 170 && b >= r + 20 && g > r && g < b) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    const std::filesystem::path pdfPath = std::filesystem::temp_directory_path() / "cnote-pdf-text-test.pdf";
    {
        const std::vector<std::uint8_t> bytes = BuildTextPdf();
        FILE* file = std::fopen(pdfPath.string().c_str(), "wb");
        if (file == nullptr) {
            std::printf("FAIL: cannot write pdf fixture\n");
            return 1;
        }
        std::fwrite(bytes.data(), 1U, bytes.size(), file);
        std::fclose(file);
    }

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    std::string error;
    Check(controller.OpenFile(pdfPath, &error), "generated PDF opens through the controller");
    if (!error.empty()) {
        std::printf("open error: %s\n", error.c_str());
    }
    const cnote::Document& document = controller.DocumentRef();
    Check(document.PageCount() == 1U, "the pdf has one page");
    Check(document.PageAt(0).sourcePdfPageIndex >= 0, "page 0 is backed by a pdf source");

    ui::CanvasWidget canvas(controller);
    canvas.SetPdfBackgroundProvider(controller.PdfBackground());
    canvas.resize(1200, 900);
    canvas.show();
    QCoreApplication::processEvents();
    canvas.SetTool(ui::Tool::Select);
    canvas.SetZoom(1.0f);
    QCoreApplication::processEvents();
    canvas.CenterOnPage(0);
    canvas.repaint();

    const ui::CanvasPageView view = canvas.CalculatePageView(0);
    Check(view.valid, "page view is valid");
    Check(controller.PdfTextCharBoxesFor(0) != nullptr
              && !controller.PdfTextCharBoxesFor(0)->empty(),
          "char boxes extract for page 0");
    if (!view.valid) {
        std::printf("%d PDF TEXT CHECK(S) FAILED\n", g_failures);
        return 1;
    }

    const std::vector<cnote::PdfTextCharBox>& boxes = *controller.PdfTextCharBoxesFor(0);
    const cnote::PdfTextCharBox& first = boxes.front();
    const QPointF textPoint(
        view.rect.left() + ((first.left + first.right) * 0.5f * view.scale),
        view.rect.top() + ((first.top + first.bottom) * 0.5f * view.scale));
    const QPointF blankPoint(
        view.rect.left() + (view.scale * (document.PageAt(0).width * 0.5f)),
        view.rect.top() + (view.scale * 80.0f));

    // Hit test: the text char reports the page, blank paper does not.
    Check(canvas.PageWithPdfTextAt(textPoint).has_value(), "cursor on a char box reports pdf text");
    Check(!canvas.PageWithPdfTextAt(blankPoint).has_value(), "cursor on blank paper reports no pdf text");

    // Drag from the first character into the second line: a pdf text range
    // commits, not an object selection.
    const std::size_t lastIndex = boxes.size() - 1U;
    const cnote::PdfTextCharBox& last = boxes[lastIndex];
    const QPointF dragEnd(
        view.rect.left() + ((last.left + last.right) * 0.5f * view.scale),
        view.rect.top() + ((last.top + last.bottom) * 0.5f * view.scale));

    SendMouse(canvas, QEvent::MouseButtonPress, textPoint, Qt::LeftButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseMove, textPoint + QPointF(30.0, 20.0), Qt::NoButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    canvas.repaint();
    const int livePixels = HighlightPixels(canvas.grab().toImage());
    std::printf("LIVE-HIGHLIGHT pixels=%d\n", livePixels);
    Check(livePixels > 0, "the drag highlights covered pdf characters live");

    SendMouse(canvas, QEvent::MouseMove, dragEnd, Qt::NoButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseButtonRelease, dragEnd, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    canvas.repaint();

    const ui::PageSelection& selection = controller.SelectionFor(0);
    std::printf("PDF-SELECT chars=%zu strokes=%zu texts=%zu\n",
                selection.pdfText.size(), selection.strokes.size(), selection.texts.size());
    Check(selection.pdfText.size() == boxes.size(), "the whole text range is selected");
    Check(selection.strokes.empty() && selection.texts.empty()
              && selection.images.empty() && selection.latexes.empty(),
          "pdf text selection carries no object indices");

    const int committedPixels = HighlightPixels(canvas.grab().toImage());
    Check(committedPixels > 0, "the committed selection paints the highlight");

    // Copy path: the selected text assembles in document order with a line
    // break between the two lines.
    const QString copied = controller.SelectedPdfText();
    std::printf("PDF-TEXT copied='%s'\n", copied.toUtf8().constData());
    Check(copied.contains(QStringLiteral("Hello pdf world")), "copied text contains line one");
    Check(copied.contains(QStringLiteral("Second line here")), "copied text contains line two");
    Check(copied.contains(QLatin1Char('\n')), "a line break separates the two lines");
    Check(controller.HasSelectedPdfText(), "the selection reports as pdf text");

    // The clipboard branch publishes plain text (offscreen QClipboard mirrors
    // the app clipboard in-process).
    Check(cnote::clipboard::WritePlainText(copied),
          "WritePlainText accepts the pdf selection");
    if (QClipboard* clip = QGuiApplication::clipboard()) {
        Check(clip->text().contains(QStringLiteral("Hello pdf world")),
              "the clipboard holds the pdf text");
    }

    // Delete clears a pdf selection without recording an undo step.
    const bool undoBefore = controller.CanUndo();
    Check(controller.RemoveSelection(), "RemoveSelection clears the pdf selection");
    Check(!controller.HasSelectedPdfText() && controller.SelectionFor(0).pdfText.empty(),
          "the pdf selection is gone");
    Check(controller.CanUndo() == undoBefore, "clearing pdf text records no undo step");
    canvas.repaint();
    Check(HighlightPixels(canvas.grab().toImage()) == 0, "the highlight is gone after clearing");

    // A drag that starts on blank paper keeps the object path (no pdfText).
    SendMouse(canvas, QEvent::MouseButtonPress, blankPoint, Qt::LeftButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseMove, blankPoint + QPointF(60.0, 60.0), Qt::NoButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseButtonRelease, blankPoint + QPointF(60.0, 60.0), Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    Check(controller.SelectionFor(0).pdfText.empty(),
          "a drag off the text is an ordinary (empty) object selection");

    std::error_code ec;
    std::filesystem::remove(pdfPath, ec);
    std::printf(g_failures == 0 ? "ALL PDF TEXT CHECKS PASSED\n" : "%d PDF TEXT CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
