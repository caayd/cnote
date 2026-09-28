// Deterministic offscreen canvas geometry/placement test (migration plan
// steps 3.1-3.3): widget geometry is Qt logical pixels, document geometry is
// page units, and render::PageRenderParams::devicePixelRatio is used only to
// allocate raster surfaces.
//
// Reported defect: "actions' preview ... and result ... ends up in the wrong
// area/size seemingly dependent on the canvas' zoom level (might also be dpi
// related)".  This test drives the real CanvasWidget + QtDocumentController
// through Qt input events at the DPR selected by QT_SCALE_FACTOR (the suite
// registers dpr 1.0 and 1.75 runs of the same binary) and at several zoom
// levels, asserting:
//   (a) a known widget point converts to the expected page coordinates,
//   (b) a committed stroke stores the page coordinates that were drawn
//       (tolerance: one logical pixel converted to page units),
//   (c) the transient live-stroke preview lands on the same device pixels as
//       the stroke after it has been composited into the cached page raster,
//   (d) the text/LaTeX edit overlay sits at the committed object's page rect.
//
// These four things are exactly what a DPR applied twice (or not at all) in
// the raster composite would break, so the test fails on the pre-fix code at
// non-1 DPR and passes afterwards.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>

#include "Document.h"
#include "latex/LatexEngine.h"
#include "qt/QtDocumentController.h"
#include "shell/AppManager.h"
#include "text/TextLayoutQt.h"
#include <QTextBlock>
#include "ui/CanvasWidget.h"

namespace {

namespace text = cnote::text;
namespace ui = cnote::ui;
using cnote::Vec2;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void CheckNear(bool condition, const std::string& what, double actual, double expected, double tolerance) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s (actual=%.4f expected=%.4f tol=%.4f)\n", what.c_str(), actual, expected, tolerance);
    }
}

void Pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

void SendMouse(
    QWidget& widget, QEvent::Type type, const QPointF& logical,
    Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, logical, widget.mapToGlobal(logical.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

struct InkBox {
    int x0 = 1 << 30;
    int y0 = 1 << 30;
    int x1 = -1;
    int y1 = -1;
    int count = 0;
    [[nodiscard]] bool valid() const { return count > 0; }
};

std::string FormatBox(const InkBox& box) {
    char buffer[80] = {};
    if (!box.valid()) {
        std::snprintf(buffer, sizeof(buffer), "<none>");
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "[%d,%d..%d,%d] ink=%d", box.x0, box.y0, box.x1, box.y1, box.count);
    return buffer;
}

// Ink of the pure-red test stroke on the white blank page.
InkBox RedInk(const QImage& image) {
    InkBox box;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) > 150 && qGreen(pixel) < 120 && qBlue(pixel) < 120) {
                box.x0 = std::min(box.x0, x);
                box.y0 = std::min(box.y0, y);
                box.x1 = std::max(box.x1, x);
                box.y1 = std::max(box.y1, y);
                ++box.count;
            }
        }
    }
    return box;
}

bool BoxNear(const InkBox& a, const InkBox& b, int tolerance) {
    return a.valid() && b.valid()
        && std::abs(a.x0 - b.x0) <= tolerance
        && std::abs(a.y0 - b.y0) <= tolerance
        && std::abs(a.x1 - b.x1) <= tolerance
        && std::abs(a.y1 - b.y1) <= tolerance;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    std::atomic<bool> appReady { false };
    std::thread watchdog([&appReady] {
        for (int attempt = 0; attempt < 300; ++attempt) {
            if (appReady.load()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(stderr, "canvas geometry: QApplication did not initialize within 30 s "
            "(missing Qt platform plugin? QT_PLUGIN_PATH=%s)\n",
            qgetenv("QT_PLUGIN_PATH").constData());
        std::fflush(stderr);
        std::_Exit(7);
    });
    watchdog.detach();

    QApplication app(argc, argv);
    appReady.store(true);

    if (!cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT))) {
        std::printf("FAIL: latex engine did not initialize\n");
        return 1;
    }

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.NewBlankDocument();

    ui::CanvasWidget canvas(controller);
    canvas.resize(1200, 900);
    canvas.show();
    QCoreApplication::processEvents();

    const double devicePixelRatio = canvas.devicePixelRatioF();
    const double requestedDpr = qgetenv("QT_SCALE_FACTOR").toDouble();
    std::printf("PLATFORM %s dpr=%.4f requestedDpr=%.4f\n",
                QApplication::platformName().toUtf8().constData(), devicePixelRatio, requestedDpr);
    if (requestedDpr > 0.0) {
        // Guard the regression value: if the platform ignored QT_SCALE_FACTOR
        // this run would silently be a dpr-1 run and could not catch the bug.
        CheckNear(std::abs(devicePixelRatio - requestedDpr) < 0.01,
                  "devicePixelRatioF matches QT_SCALE_FACTOR", devicePixelRatio, requestedDpr, 0.01);
    } else {
        CheckNear(std::abs(devicePixelRatio - 1.0) < 0.01,
                  "devicePixelRatioF is 1.0 without QT_SCALE_FACTOR", devicePixelRatio, 1.0, 0.01);
    }

    const cnote::Document& document = controller.DocumentRef();
    Check(document.PageCount() == 1U, "blank document has one page");
    const cnote::Page& page = document.PageAt(0);
    std::printf("PAGE %.0fx%.0f canvas=%dx%d logical\n", page.width, page.height, canvas.width(), canvas.height());

    // Thick pure-red stroke so the transient preview and the committed raster
    // can be compared pixel-by-pixel against the page backdrop.
    cnote::StrokeStyle strokeStyle;
    strokeStyle.color = 0xFFFF0000U;
    strokeStyle.width = 8.0f;
    controller.SetActiveStrokeStyle(strokeStyle);

    const double toleranceDevice = std::max(2.0, 3.0 * devicePixelRatio);

    // (e) Zoom-session snapshot blit.  While a zoom is in flight FindSurface
    // misses at the new scale and the newest raster (a snapshot at the previous
    // scale) is reused; it must be resampled into the new view rect.  Keying the
    // 1:1 blit on the device pixel ratio alone left the page at the stale scale
    // for the whole gesture ("wrong ... size seemingly dependent on the canvas'
    // zoom level", the blit-rect-vs-keys candidate).  A red marker committed at
    // the page centre shows the scale the snapshot is drawn at; the page centre
    // stays in view regardless of clipping.
    canvas.SetTool(ui::Tool::Draw);
    canvas.ResetView();
    Pump(160);
    canvas.CenterOnPage(0);
    canvas.repaint();

    const ui::CanvasPageView fitView = canvas.CalculatePageView(0);
    const Vec2 snapshotPageCenter { page.width * 0.5f, page.height * 0.5f };
    const QPointF centerLogical = canvas.PageToScreen(0, snapshotPageCenter);
    ui::InteractionModifiers snapshotModifiers = canvas.Router().Modifiers();
    snapshotModifiers.straightLine = true;
    canvas.SetInteractionModifiers(snapshotModifiers);
    SendMouse(canvas, QEvent::MouseButtonPress, centerLogical + QPointF(-30.0, 0.0), Qt::LeftButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseMove, centerLogical + QPointF(30.0, 0.0), Qt::NoButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseButtonRelease, centerLogical + QPointF(30.0, 0.0), Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    snapshotModifiers.straightLine = false;
    canvas.SetInteractionModifiers(snapshotModifiers);
    Check(controller.DocumentRef().PageAt(0).strokes.size() == 1U, "zoom-session marker stroke committed");
    canvas.repaint();

    canvas.SetZoom(1.8f);  // do not settle: the zoom session must stay active
    const ui::CanvasPageView snapshotView = canvas.CalculatePageView(0);
    const QImage snapshotFrame = canvas.grab().toImage();
    const InkBox snapshotMarker = RedInk(snapshotFrame);
    const double expectedMarkerX =
        (snapshotView.rect.left() + (static_cast<double>(snapshotPageCenter.x) * snapshotView.scale)) * devicePixelRatio;
    const double expectedMarkerY =
        (snapshotView.rect.top() + (static_cast<double>(snapshotPageCenter.y) * snapshotView.scale)) * devicePixelRatio;
    const double observedMarkerX = (snapshotMarker.x0 + snapshotMarker.x1) * 0.5;
    const double observedMarkerY = (snapshotMarker.y0 + snapshotMarker.y1) * 0.5;
    std::printf("SNAPSHOT fitScale=%.4f zoomScale=%.4f marker=%s expectedDevice=(%.1f,%.1f) observed=(%.1f,%.1f)\n",
                fitView.scale, snapshotView.scale, FormatBox(snapshotMarker).c_str(),
                expectedMarkerX, expectedMarkerY, observedMarkerX, observedMarkerY);
    Check(snapshotView.scale > fitView.scale, "the snapshot probe zooms past the cached raster scale");
    Check(snapshotMarker.valid(), "zoom-session frame shows the page marker");
    CheckNear(std::abs(observedMarkerX - expectedMarkerX) <= 6.0 * devicePixelRatio,
              "zoom-session snapshot is drawn at the new scale (x)",
              observedMarkerX, expectedMarkerX, 6.0 * devicePixelRatio);
    CheckNear(std::abs(observedMarkerY - expectedMarkerY) <= 6.0 * devicePixelRatio,
              "zoom-session snapshot is drawn at the new scale (y)",
              observedMarkerY, expectedMarkerY, 6.0 * devicePixelRatio);
    Pump(160);  // settle before the zoom matrix below
    Check(controller.Undo(), "undo removes the zoom-session marker");
    QCoreApplication::processEvents();
    Check(controller.DocumentRef().PageAt(0).strokes.empty(), "page has no stroke after the snapshot probe");

    const float zoomLevels[] = { 0.5f, 1.0f, 2.0f };

    for (const float zoom : zoomLevels) {
        canvas.SetTool(ui::Tool::Draw);
        canvas.SetZoom(zoom);
        Pump(160);  // let the zoom session settle so the raster is at this scale
        canvas.CenterOnPage(0);
        Pump(20);
        canvas.repaint();

        const ui::CanvasPageView view = canvas.CalculatePageView(0);
        Check(view.valid, "page view is valid at zoom " + std::to_string(zoom));
        if (!view.valid) {
            continue;
        }
        const float scale = view.scale;
        const double pageTolerance = 1.0 / static_cast<double>(scale);
        const QPointF center(canvas.rect().center());

        // (a) widget -> page -> widget through the real CanvasWidget path.
        const Vec2 expectedCenter {
            static_cast<float>((center.x() - view.rect.left()) / scale),
            static_cast<float>((center.y() - view.rect.top()) / scale),
        };
        const Vec2 mappedCenter = canvas.ScreenToPage(view, center, false);
        std::printf("MAP zoom=%.2f scale=%.4f widget=(%.2f,%.2f) page=(%.3f,%.3f) expected=(%.3f,%.3f)\n",
                    zoom, scale, center.x(), center.y(), mappedCenter.x, mappedCenter.y,
                    expectedCenter.x, expectedCenter.y);
        CheckNear(std::abs(mappedCenter.x - expectedCenter.x) <= pageTolerance,
                  "widget->page x at zoom " + std::to_string(zoom),
                  mappedCenter.x, expectedCenter.x, pageTolerance);
        CheckNear(std::abs(mappedCenter.y - expectedCenter.y) <= pageTolerance,
                  "widget->page y at zoom " + std::to_string(zoom),
                  mappedCenter.y, expectedCenter.y, pageTolerance);

        const Vec2 pageCenter { page.width * 0.5f, page.height * 0.5f };
        const QPointF screenCenter = canvas.PageToScreen(0, pageCenter);
        const QPointF expectedScreen(
            view.rect.left() + (pageCenter.x * scale),
            view.rect.top() + (pageCenter.y * scale));
        CheckNear(std::abs(screenCenter.x() - expectedScreen.x()) <= 1.0,
                  "page->widget x at zoom " + std::to_string(zoom),
                  screenCenter.x(), expectedScreen.x(), 1.0);
        CheckNear(std::abs(screenCenter.y() - expectedScreen.y()) <= 1.0,
                  "page->widget y at zoom " + std::to_string(zoom),
                  screenCenter.y(), expectedScreen.y(), 1.0);

        // Stroke endpoints stay inside both the page and the visible widget.
        const double offsetX = std::min(static_cast<double>(page.width) * scale * 0.25, canvas.width() * 0.4);
        const double offsetY = std::min(static_cast<double>(page.height) * scale * 0.25, canvas.height() * 0.4);
        const QPointF aLogical = center + QPointF(-offsetX, -offsetY);
        const QPointF bLogical = center + QPointF(offsetX, offsetY);
        const Vec2 expectedA = canvas.ScreenToPage(view, aLogical, true);
        const Vec2 expectedB = canvas.ScreenToPage(view, bLogical, true);

        // Straight-line modifier (the reference "A" chord) replaces the filtered
        // freehand points with the exact drawn segment, so the committed page
        // coordinates are the precise ones the pointer described.
        ui::InteractionModifiers modifiers = canvas.Router().Modifiers();
        modifiers.straightLine = true;
        canvas.SetInteractionModifiers(modifiers);

        SendMouse(canvas, QEvent::MouseButtonPress, aLogical, Qt::LeftButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseMove, bLogical, Qt::NoButton, Qt::LeftButton);
        QCoreApplication::processEvents();
        const QImage preview = canvas.grab().toImage();

        SendMouse(canvas, QEvent::MouseButtonRelease, bLogical, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
        const QImage committed = canvas.grab().toImage();

        modifiers.straightLine = false;
        canvas.SetInteractionModifiers(modifiers);

        // (b) the committed stroke stores the page coordinates that were drawn.
        const cnote::Page& pageAfter = controller.DocumentRef().PageAt(0);
        Check(pageAfter.strokes.size() == 1U, "one stroke committed at zoom " + std::to_string(zoom));
        if (pageAfter.strokes.size() == 1U) {
            const cnote::Stroke& stroke = pageAfter.strokes.front();
            Check(stroke.points.size() >= 2U, "committed stroke has two endpoints at zoom " + std::to_string(zoom));
            const Vec2 first = stroke.points.front();
            const Vec2 last = stroke.points.back();
            std::printf(
                "COMMIT zoom=%.2f points=%zu first=(%.3f,%.3f) expected=(%.3f,%.3f) last=(%.3f,%.3f) expected=(%.3f,%.3f) tol=%.3f\n",
                zoom, stroke.points.size(), first.x, first.y, expectedA.x, expectedA.y,
                last.x, last.y, expectedB.x, expectedB.y, pageTolerance);
            CheckNear(std::abs(first.x - expectedA.x) <= pageTolerance, "stroke start x at zoom " + std::to_string(zoom), first.x, expectedA.x, pageTolerance);
            CheckNear(std::abs(first.y - expectedA.y) <= pageTolerance, "stroke start y at zoom " + std::to_string(zoom), first.y, expectedA.y, pageTolerance);
            CheckNear(std::abs(last.x - expectedB.x) <= pageTolerance, "stroke end x at zoom " + std::to_string(zoom), last.x, expectedB.x, pageTolerance);
            CheckNear(std::abs(last.y - expectedB.y) <= pageTolerance, "stroke end y at zoom " + std::to_string(zoom), last.y, expectedB.y, pageTolerance);
        }

        // (c) the transient preview and the committed raster must land on the
        // same device pixels: the preview is painted through the widget painter
        // (logical coordinates) while the committed stroke is composited into
        // the cached page raster (device pixel ratio stamped on the QImage).
        const InkBox previewBox = RedInk(preview);
        const InkBox committedBox = RedInk(committed);
        std::printf("PIXELS zoom=%.2f preview=%s committed=%s tolDevice=%.1f\n",
                    zoom, FormatBox(previewBox).c_str(), FormatBox(committedBox).c_str(), toleranceDevice);
        Check(previewBox.valid(), "live stroke preview has ink at zoom " + std::to_string(zoom));
        Check(committedBox.valid(), "committed stroke raster has ink at zoom " + std::to_string(zoom));
        Check(BoxNear(previewBox, committedBox, static_cast<int>(std::lround(toleranceDevice))),
              "committed stroke lands on the preview pixels at zoom " + std::to_string(zoom));
        CheckNear(std::abs(previewBox.count - committedBox.count) <= std::max(64, previewBox.count / 4),
                  "committed stroke ink area matches the preview at zoom " + std::to_string(zoom),
                  committedBox.count, previewBox.count, std::max(64, previewBox.count / 4));

        Check(controller.Undo(), "undo removes the geometry-test stroke at zoom " + std::to_string(zoom));
        QCoreApplication::processEvents();
        Check(controller.DocumentRef().PageAt(0).strokes.empty(), "page has no stroke after undo at zoom " + std::to_string(zoom));

        // (d) text edit overlay position vs the committed object's page rect.
        canvas.SetTool(ui::Tool::Text);
        const QPointF textLogical = center;
        SendMouse(canvas, QEvent::MouseButtonPress, textLogical, Qt::LeftButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseButtonRelease, textLogical, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
        Check(canvas.EditOverlayActive(), "text overlay opens at zoom " + std::to_string(zoom));
        const cnote::PageText* editingText = controller.EditingTextObject();
        Check(editingText != nullptr, "controller reports the edited text at zoom " + std::to_string(zoom));
        if (editingText != nullptr && canvas.TextEditor() != nullptr && canvas.TextEditor()->isVisible()) {
            const QPointF expectedOverlay(
                view.rect.left() + (static_cast<double>(editingText->x) * scale),
                view.rect.top() + (static_cast<double>(editingText->y) * scale));
            const QPointF overlay(canvas.TextEditor()->geometry().topLeft());
            std::printf("TEXT zoom=%.2f object=(%.3f,%.3f) overlay=(%.1f,%.1f) expected=(%.1f,%.1f)\n",
                        zoom, editingText->x, editingText->y, overlay.x(), overlay.y(),
                        expectedOverlay.x(), expectedOverlay.y());
            CheckNear(std::abs(overlay.x() - expectedOverlay.x()) <= 1.0,
                      "text overlay x at the object page rect at zoom " + std::to_string(zoom),
                      overlay.x(), expectedOverlay.x(), 1.0);
            CheckNear(std::abs(overlay.y() - expectedOverlay.y()) <= 1.0,
                      "text overlay y at the object page rect at zoom " + std::to_string(zoom),
                      overlay.y(), expectedOverlay.y(), 1.0);
            const Vec2 expectedText = canvas.ScreenToPage(view, textLogical, true);
            CheckNear(std::abs(editingText->x - expectedText.x) <= pageTolerance,
                      "created text page x at zoom " + std::to_string(zoom), editingText->x, expectedText.x, pageTolerance);
            CheckNear(std::abs(editingText->y - expectedText.y) <= pageTolerance,
                      "created text page y at zoom " + std::to_string(zoom), editingText->y, expectedText.y, pageTolerance);

            canvas.TextEditor()->setPlainText(QStringLiteral("placement probe"));
            canvas.CommitActiveEdit();
            QCoreApplication::processEvents();
            const cnote::Page& pageWithText = controller.DocumentRef().PageAt(0);
            Check(!pageWithText.texts.empty(), "committed text object exists at zoom " + std::to_string(zoom));
            if (!pageWithText.texts.empty()) {
                const cnote::PageText& committedText = pageWithText.texts.back();
                CheckNear(std::abs(committedText.x - expectedText.x) <= pageTolerance,
                          "committed text page x at zoom " + std::to_string(zoom), committedText.x, expectedText.x, pageTolerance);
                CheckNear(std::abs(committedText.y - expectedText.y) <= pageTolerance,
                          "committed text page y at zoom " + std::to_string(zoom), committedText.y, expectedText.y, pageTolerance);
                Check(committedText.content == u"placement probe", "committed text content kept at zoom " + std::to_string(zoom));
            }
            Check(controller.Undo(), "undo removes the geometry-test text at zoom " + std::to_string(zoom));
            QCoreApplication::processEvents();
        } else {
            // Close the overlay once so the next iteration starts clean.
            canvas.CancelActiveEdit();
        }

        // (d) LaTeX edit overlay position vs the committed object's page rect.
        canvas.SetTool(ui::Tool::Latex);
        const QPointF latexLogical = center;
        SendMouse(canvas, QEvent::MouseButtonPress, latexLogical, Qt::LeftButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseButtonRelease, latexLogical, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
        const cnote::PageLatex* editingLatex = controller.EditingLatexObject();
        Check(editingLatex != nullptr, "controller reports the edited LaTeX at zoom " + std::to_string(zoom));
        if (editingLatex != nullptr && canvas.LatexEditor() != nullptr && canvas.LatexEditor()->isVisible()) {
            const double expectedLeft = view.rect.left() + (static_cast<double>(editingLatex->x) * scale);
            const double overlayLeft = canvas.LatexEditor()->geometry().left();
            const Vec2 expectedLatex = canvas.ScreenToPage(view, latexLogical, true);
            std::printf("LATEX zoom=%.2f object=(%.3f,%.3f) sourceLeft=%.1f expectedLeft=%.1f\n",
                        zoom, editingLatex->x, editingLatex->y, overlayLeft, expectedLeft);
            CheckNear(std::abs(overlayLeft - expectedLeft) <= 1.0,
                      "LaTeX source box x aligns with the object at zoom " + std::to_string(zoom),
                      overlayLeft, expectedLeft, 1.0);
            CheckNear(std::abs(editingLatex->x - expectedLatex.x) <= pageTolerance,
                      "created LaTeX page x at zoom " + std::to_string(zoom), editingLatex->x, expectedLatex.x, pageTolerance);
            CheckNear(std::abs(editingLatex->y - expectedLatex.y) <= pageTolerance,
                      "created LaTeX page y at zoom " + std::to_string(zoom), editingLatex->y, expectedLatex.y, pageTolerance);
        }
        canvas.CancelActiveEdit();
        QCoreApplication::processEvents();
        Check(controller.DocumentRef().PageAt(0).latexes.empty(), "cancelled LaTeX leaves no object at zoom " + std::to_string(zoom));
    }

    // (f) Pending-erase feedback (App.cpp:172 kPendingEraseObjectOpacity,
    // App.cpp:9453-9568): while the eraser drag has marked an object, the page
    // skips the cached surface and repaints it at 0.5 opacity; clearing the
    // mark restores full opacity.  The probe stroke is pure red, so the faded
    // frame keeps red high while green/blue rise to ~128 over the white page.
    canvas.SetTool(ui::Tool::Draw);
    canvas.SetZoom(1.0f);
    Pump(160);
    canvas.CenterOnPage(0);
    Pump(20);
    canvas.repaint();
    {
        const QPointF center(canvas.rect().center());
        ui::InteractionModifiers modifiers = canvas.Router().Modifiers();
        modifiers.straightLine = true;
        canvas.SetInteractionModifiers(modifiers);
        SendMouse(canvas, QEvent::MouseButtonPress, center + QPointF(-40.0, 0.0), Qt::LeftButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseMove, center + QPointF(40.0, 0.0), Qt::NoButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseButtonRelease, center + QPointF(40.0, 0.0), Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
        modifiers.straightLine = false;
        canvas.SetInteractionModifiers(modifiers);
    }
    Check(controller.DocumentRef().PageAt(0).strokes.size() == 1U, "pending-erase probe stroke committed");
    const QImage fullFrame = canvas.grab().toImage();
    const InkBox fullInk = RedInk(fullFrame);
    Check(fullInk.valid(), "pending-erase probe stroke has ink");
    if (fullInk.valid()) {
        const QPoint inkCenter((fullInk.x0 + fullInk.x1) / 2, (fullInk.y0 + fullInk.y1) / 2);
        const QRgb fullPixel = fullFrame.pixel(inkCenter);

        canvas.UpdatePendingErase({ ui::ObjectRef { 0U, ui::ObjectKind::Stroke, 0U } });
        const QImage fadedFrame = canvas.grab().toImage();
        const QRgb fadedPixel = fadedFrame.pixel(inkCenter);
        std::printf("PENDING-ERASE full=(%d,%d,%d) faded=(%d,%d,%d)\n",
                    qRed(fullPixel), qGreen(fullPixel), qBlue(fullPixel),
                    qRed(fadedPixel), qGreen(fadedPixel), qBlue(fadedPixel));
        Check(qRed(fadedPixel) > 200, "the pending stroke keeps its red channel");
        Check(qGreen(fadedPixel) > 90 && qGreen(fadedPixel) < 170,
              "the pending stroke fades to 0.5 opacity over the white page");
        Check(qGreen(fadedPixel) > qGreen(fullPixel) + 60,
              "the pending stroke is visibly lighter than the committed one");

        canvas.UpdatePendingErase({});
        const QImage restoredFrame = canvas.grab().toImage();
        const QRgb restoredPixel = restoredFrame.pixel(inkCenter);
        Check(qRed(restoredPixel) > 200 && qGreen(restoredPixel) < 60,
              "clearing the pending mark restores full opacity");
    }
    Check(controller.Undo(), "undo removes the pending-erase probe stroke");
    QCoreApplication::processEvents();

    // (g) Move-gesture rendering: dragging a selection must not re-rasterize
    // the page per pointer sample.  The gesture paints the committed raster,
    // a once-per-gesture patch over the vacated region and the dragged objects
    // on top, so the surface render count stays flat across the drag and the
    // original position shows no ghost ink.
    canvas.SetTool(ui::Tool::Draw);
    canvas.SetZoom(1.0f);
    Pump(160);
    canvas.CenterOnPage(0);
    Pump(20);
    canvas.repaint();
    {
        // A diagonal stroke gives the frame a centre far from every edge
        // handle, so pressing it starts a move, not a resize.
        const QPointF center(canvas.rect().center());
        ui::InteractionModifiers modifiers = canvas.Router().Modifiers();
        modifiers.straightLine = true;
        canvas.SetInteractionModifiers(modifiers);
        SendMouse(canvas, QEvent::MouseButtonPress, center + QPointF(-50.0, -50.0), Qt::LeftButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseMove, center + QPointF(50.0, 50.0), Qt::NoButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseButtonRelease, center + QPointF(50.0, 50.0), Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
        modifiers.straightLine = false;
        canvas.SetInteractionModifiers(modifiers);
    }
    Check(controller.DocumentRef().PageAt(0).strokes.size() == 1U, "move-gesture probe stroke committed");
    canvas.repaint();

    // Select the stroke and drag it with the Select tool through the real
    // router path (press inside the selection frame starts a move).
    {
        cnote::ui::PageSelection selection;
        selection.strokes.push_back(0);
        controller.SelectObjects(0, selection);
    }
    canvas.SetTool(ui::Tool::Select);
    canvas.repaint();
    const QPointF center(canvas.rect().center());
    const QImage beforeDrag = canvas.grab().toImage();
    const InkBox beforeInk = RedInk(beforeDrag);
    Check(beforeInk.valid(), "selected stroke has ink before the drag");

    const std::uint64_t gestureRendersBefore = canvas.SurfaceRenderCount();
    SendMouse(canvas, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    Check(canvas.ActiveInteraction() == ui::InteractionKind::MoveSelection,
          "pressing inside the selection starts a move");
    for (int step = 1; step <= 10; ++step) {
        SendMouse(canvas, QEvent::MouseMove, center + QPointF(step * 10.0, step * 6.0), Qt::NoButton, Qt::LeftButton);
        canvas.repaint();
    }
    const std::uint64_t gestureRendersMid = canvas.SurfaceRenderCount();
    Check(gestureRendersMid - gestureRendersBefore <= 2U,
          "a move gesture renders at most the patch and the item rasters once");

    const QImage midDrag = canvas.grab().toImage();
    const InkBox midInk = RedInk(midDrag);
    Check(midInk.valid(), "the dragged stroke still has ink mid-gesture");
    if (midInk.valid() && beforeInk.valid()) {
        const int movedX = midInk.x0 - beforeInk.x0;
        const int movedY = midInk.y0 - beforeInk.y0;
        std::printf("MOVE-DRAG ink moved by (%d,%d) device px\n", movedX, movedY);
        Check(movedX > 50, "the dragged stroke follows the pointer horizontally");
        Check(movedY > 30, "the dragged stroke follows the pointer vertically");
    }
    // No ghost: the stroke's original position shows page white, not red ink.
    if (beforeInk.valid()) {
        const QPoint originCenter((beforeInk.x0 + beforeInk.x1) / 2, (beforeInk.y0 + beforeInk.y1) / 2);
        const QRgb originPixel = midDrag.pixel(originCenter);
        Check(qGreen(originPixel) > 200 && qBlue(originPixel) > 200,
              "the vacated region is repainted without the dragged stroke");
    }

    SendMouse(canvas, QEvent::MouseButtonRelease, center + QPointF(100.0, 60.0), Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    canvas.repaint();
    const QImage afterDrag = canvas.grab().toImage();
    const InkBox afterInk = RedInk(afterDrag);
    std::printf("MOVE-COMMIT mid=%s after=%s\n", FormatBox(midInk).c_str(), FormatBox(afterInk).c_str());
    Check(BoxNear(afterInk, midInk, static_cast<int>(4 * devicePixelRatio) + 2),
          "the committed frame matches the last drag frame");
    Check(controller.Undo(), "undo restores the probe stroke position");
    controller.ClearSelection();
    Check(controller.Undo(), "undo removes the move-gesture probe stroke");
    QCoreApplication::processEvents();

    Check(canvas.SurfaceRenderCount() > 0U, "the cached page surface was rendered at least once");


    // (h) Text box resize -> reopen: the editor wraps at the object's pixel
    // width (FixedPixelWidth + lineWrapColumnOrWidth). Regression: the column
    // width was never set, so a fixed-width object wrapped one char per line.
    canvas.SetTool(ui::Tool::Text);
    SendMouse(canvas, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    if (canvas.TextEditor() != nullptr && canvas.TextEditor()->isVisible()) {
        canvas.TextEditor()->setPlainText(QStringLiteral("wrap me after a resize"));
        canvas.CommitActiveEdit();
        QCoreApplication::processEvents();
    }
    Check(controller.DocumentRef().PageAt(0).texts.size() == 1U, "wrap probe text committed");
    if (!controller.DocumentRef().PageAt(0).texts.empty()) {
        // Drive the same code path the resize handle uses: an absolute
        // SelectionTransform that narrows the box horizontally.
        const cnote::PageText& original = controller.DocumentRef().PageAt(0).texts[0];
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(original, controller.DocumentRef().PageAt(0).width);
        ui::SelectionTransform transform;
        transform.from.valid = true;
        transform.from.left = bounds.left;
        transform.from.top = bounds.top;
        transform.from.right = bounds.right;
        transform.from.bottom = bounds.bottom;
        transform.to = transform.from;
        transform.to.right = transform.from.left + (transform.from.right - transform.from.left) * 0.25f;
        {
            ui::PageSelection sel;
            sel.texts.push_back(0);
            controller.SelectObjects(0, sel);
        }
        controller.BeginPageGesture(0);
        controller.TransformSelection(0, transform);
        controller.CommitPageGesture();
        canvas.repaint();
        const cnote::PageText& resized = controller.DocumentRef().PageAt(0).texts[0];
        Check(resized.width > 0.0f, "resize gave the text a fixed width");

        // Reopen the editor on the object.
        const QPointF textPt = canvas.PageToScreen(0, { resized.x + resized.width * 0.5f, resized.y + 8.0f });
        SendMouse(canvas, QEvent::MouseButtonPress, textPt, Qt::LeftButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseButtonRelease, textPt, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
        Check(canvas.EditOverlayActive(), "text editor reopens on the resized object");
        if (canvas.EditOverlayActive() && canvas.TextEditor() != nullptr) {
            QTextEdit* editor = canvas.TextEditor();
            Check(editor->lineWrapMode() == QTextEdit::FixedPixelWidth,
                  "fixed-width text uses pixel-width wrapping");
            Check(editor->lineWrapColumnOrWidth() > 0,
                  "wrap width is set, not the 0 that wraps one char per line");
            const QTextBlock block = editor->document()->begin();
            const int lines = block.layout() ? block.layout()->lineCount() : 0;
            std::printf("WRAP lines=%d colOrWidth=%d\n", lines, editor->lineWrapColumnOrWidth());
            Check(lines < static_cast<int>(resized.content.size()),
                  "the editor does not wrap one char per line");
            canvas.CancelActiveEdit();
            QCoreApplication::processEvents();
        }
        controller.ClearSelection();
        controller.Undo();
        controller.Undo();
        QCoreApplication::processEvents();
        Check(controller.DocumentRef().PageAt(0).texts.empty(), "wrap probe cleaned up");
    }

    cnote::latex::Shutdown();
    std::printf(g_failures == 0 ? "ALL CANVAS GEOMETRY CHECKS PASSED\n" : "%d CANVAS GEOMETRY CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
