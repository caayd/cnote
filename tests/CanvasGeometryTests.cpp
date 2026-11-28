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
#include <QPointingDevice>
#include <QTabletEvent>
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
#include <QKeyEvent>
#include "ui/CanvasWidget.h"
#include "ui/PageDefaultsQt.h"

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

void TypeText(QWidget& widget, const QString& text) {
    for (const QChar character : text) {
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(character));
        QApplication::sendEvent(&widget, &press);
        QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(character));
        QApplication::sendEvent(&widget, &release);
    }
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

        // (c2) Freehand ink renders from raw samples through the stroke's
        // recorded stabilization; the live preview must draw that same
        // stabilized curve, or the ink jumps when the stroke commits.  Heavy
        // stabilization makes the stabilized tail lag the pointer by far more
        // than the pixel tolerance, so a preview drawn from the raw samples
        // fails here.
        canvas.Router().SetTuning(ui::InteractionTuning { 0.85f, 0.8f });
        // The path spans the middle 60% of the visible page width at every zoom.
        const double span = std::min(static_cast<double>(page.width) * scale, canvas.width() * 0.8);
        const QPointF wiggleStart = center + QPointF(-0.3 * span, 0.0);
        SendMouse(canvas, QEvent::MouseButtonPress, wiggleStart, Qt::LeftButton, Qt::LeftButton);
        QPointF wigglePoint = wiggleStart;
        for (int step = 1; step <= 40; ++step) {
            wigglePoint = wiggleStart + QPointF(0.015 * span * step, 0.06 * span * std::sin(step * 0.35));
            SendMouse(canvas, QEvent::MouseMove, wigglePoint, Qt::NoButton, Qt::LeftButton);
        }
        QCoreApplication::processEvents();
        const QImage freehandPreview = canvas.grab().toImage();
        SendMouse(canvas, QEvent::MouseButtonRelease, wigglePoint, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
        const QImage freehandCommitted = canvas.grab().toImage();
        canvas.Router().SetTuning(ui::InteractionTuning {});

        const InkBox freehandPreviewBox = RedInk(freehandPreview);
        const InkBox freehandCommittedBox = RedInk(freehandCommitted);
        std::printf("FREEHAND zoom=%.2f preview=%s committed=%s\n",
                    zoom, FormatBox(freehandPreviewBox).c_str(), FormatBox(freehandCommittedBox).c_str());
        Check(BoxNear(freehandPreviewBox, freehandCommittedBox, static_cast<int>(std::lround(toleranceDevice))),
              "committed freehand stroke lands on the preview pixels at zoom " + std::to_string(zoom));
        CheckNear(std::abs(freehandPreviewBox.count - freehandCommittedBox.count) <= std::max(64, freehandPreviewBox.count / 10),
                  "committed freehand ink area matches the preview at zoom " + std::to_string(zoom),
                  freehandCommittedBox.count, freehandPreviewBox.count, std::max(64, freehandPreviewBox.count / 10));
        Check(controller.Undo(), "undo removes the freehand stroke at zoom " + std::to_string(zoom));
        QCoreApplication::processEvents();

        // (c3) Pen ink: tablet samples carry their pressure into the stroke,
        // which renders as a filled pressure-width outline; the live layer's
        // incremental outline must land on the committed pixels too.
        {
            static const QPointingDevice stylus(QStringLiteral("geometry test stylus"), 4242,
                QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
            const auto sendTablet = [&canvas](QEvent::Type type, const QPointF& position, qreal pressure,
                                        Qt::MouseButton button, Qt::MouseButtons buttons) {
                QTabletEvent event(type, &stylus, position, canvas.mapToGlobal(position), pressure,
                    0.0f, 0.0f, 0.0f, 0.0, 0.0f, Qt::NoModifier, button, buttons);
                QApplication::sendEvent(&canvas, &event);
                return event.isAccepted();
            };
            const QPointF penStart = center + QPointF(-0.3 * span, 0.0);
            bool accepted = sendTablet(QEvent::TabletPress, penStart, 0.1, Qt::LeftButton, Qt::LeftButton);
            QPointF penPoint = penStart;
            for (int step = 1; step <= 40; ++step) {
                penPoint = penStart + QPointF(0.015 * span * step, 0.06 * span * std::sin(step * 0.35));
                accepted = sendTablet(QEvent::TabletMove, penPoint, 0.1 + (0.85 * step / 40.0), Qt::NoButton,
                    Qt::LeftButton) && accepted;
            }
            QCoreApplication::processEvents();
            const QImage penPreview = canvas.grab().toImage();
            accepted = sendTablet(QEvent::TabletRelease, penPoint, 0.0, Qt::LeftButton, Qt::NoButton) && accepted;
            QCoreApplication::processEvents();
            const QImage penCommitted = canvas.grab().toImage();

            const cnote::Page& penPage = controller.DocumentRef().PageAt(0);
            Check(accepted, "the canvas accepts tablet events at zoom " + std::to_string(zoom));
            Check(penPage.strokes.size() == 1U && !penPage.strokes.front().pressures.empty()
                    && penPage.strokes.front().pressures.size() == penPage.strokes.front().points.size(),
                "a pen stroke stores a pressure per point at zoom " + std::to_string(zoom));
            const InkBox penPreviewBox = RedInk(penPreview);
            const InkBox penCommittedBox = RedInk(penCommitted);
            std::printf("PEN zoom=%.2f preview=%s committed=%s\n",
                        zoom, FormatBox(penPreviewBox).c_str(), FormatBox(penCommittedBox).c_str());
            Check(BoxNear(penPreviewBox, penCommittedBox, static_cast<int>(std::lround(toleranceDevice))),
                  "committed pen stroke lands on the preview pixels at zoom " + std::to_string(zoom));
            CheckNear(std::abs(penPreviewBox.count - penCommittedBox.count) <= std::max(64, penPreviewBox.count / 10),
                      "committed pen ink area matches the preview at zoom " + std::to_string(zoom),
                      penCommittedBox.count, penPreviewBox.count, std::max(64, penPreviewBox.count / 10));
            Check(controller.Undo(), "undo removes the pen stroke at zoom " + std::to_string(zoom));
            QCoreApplication::processEvents();
        }

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

            // The source box must stay clear of the live preview as the typed
            // formula grows (a new object starts empty, so measuring the
            // object as it was when the edit began put the box over it).
            const QString tallSource = QStringLiteral("\\sum_{i=0}^{n}\\frac{\\frac{a_i}{b_i}}{\\frac{c_i}{d_i}}");
            canvas.LatexEditor()->setPlainText(tallSource);
            canvas.repaint();
            QCoreApplication::processEvents();
            const cnote::latex::FormulaMetrics tall = cnote::latex::Measure(
                cnote::latex::Parse(tallSource.toStdU16String(), editingLatex->fontSize, true));
            Check(tall.height > 0.0f, "tall probe formula parses at zoom " + std::to_string(zoom));
            const QRectF previewRect(
                view.rect.left() + static_cast<double>(editingLatex->x) * scale,
                view.rect.top() + static_cast<double>(editingLatex->y) * scale,
                static_cast<double>(tall.width) * scale,
                static_cast<double>(tall.height) * scale);
            const QRect tallBox = canvas.LatexEditor()->geometry();
            std::printf("LATEX zoom=%.2f preview=(%.1f,%.1f %.1fx%.1f) box=(%d,%d %dx%d)\n", zoom,
                previewRect.left(), previewRect.top(), previewRect.width(), previewRect.height(),
                tallBox.left(), tallBox.top(), tallBox.width(), tallBox.height());
            Check(!QRectF(tallBox).intersects(previewRect),
                "LaTeX source box does not cover the live preview at zoom " + std::to_string(zoom));

            // Half-typed source that does not parse keeps the box in place.
            canvas.LatexEditor()->setPlainText(tallSource + QStringLiteral("+\\frac{}{}"));
            canvas.repaint();
            QCoreApplication::processEvents();
            Check(canvas.LatexEditor()->geometry() == tallBox,
                "unparsable LaTeX source keeps the source box in place at zoom " + std::to_string(zoom));
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

    // (f2) Eraser trail: an erase drag leaves a translucent gray comet behind
    // the pointer (Excalidraw's EraserTrail) that fades out after release and
    // leaves no pixels behind.
    canvas.SetTool(ui::Tool::Erase);
    {
        const QPointF center(canvas.rect().center());
        const double dpr = canvas.devicePixelRatioF();
        const QRect area = QRectF((center + QPointF(-130.0, -40.0)) * dpr, (center + QPointF(130.0, 40.0)) * dpr)
            .toAlignedRect();
        // 20% black over the white page is a neutral light gray.
        const auto trailPixels = [&area](const QImage& image) {
            int count = 0;
            const QRect clipped = area.intersected(image.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
                for (int x = clipped.left(); x <= clipped.right(); ++x) {
                    const QRgb pixel = image.pixel(x, y);
                    const int r = qRed(pixel);
                    count += (r >= 170 && r <= 235 && std::abs(r - qGreen(pixel)) < 6 && std::abs(r - qBlue(pixel)) < 6) ? 1 : 0;
                }
            }
            return count;
        };
        Check(trailPixels(canvas.grab().toImage()) == 0, "no eraser trail before erasing");
        SendMouse(canvas, QEvent::MouseButtonPress, center + QPointF(-100.0, 0.0), Qt::LeftButton, Qt::LeftButton);
        for (int step = 1; step <= 20; ++step) {
            SendMouse(canvas, QEvent::MouseMove, center + QPointF(-100.0 + (10.0 * step), 0.0), Qt::NoButton, Qt::LeftButton);
        }
        const int duringDrag = trailPixels(canvas.grab().toImage());
        SendMouse(canvas, QEvent::MouseButtonRelease, center + QPointF(100.0, 0.0), Qt::LeftButton, Qt::NoButton);
        Pump(350);
        const int afterRelease = trailPixels(canvas.grab().toImage());
        std::printf("ERASER-TRAIL during=%d after=%d\n", duringDrag, afterRelease);
        Check(duringDrag > 100, "an erase drag paints its trail");
        Check(afterRelease == 0, "the eraser trail fades out after release");
    }
    canvas.SetTool(ui::Tool::Draw);

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
    const cnote::Stroke originalStroke = controller.DocumentRef().PageAt(0).strokes[0];

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
    // The move is one ReplaceObjects action carrying only the dragged stroke;
    // undo/redo must restore its exact state without a whole-page snapshot.
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    Check(!session->undoActions.empty()
            && session->undoActions.back().type == cnote::HistoryActionType::ReplaceObjects,
          "a move records one object-delta history action");
    if (!session->undoActions.empty()) {
        const cnote::HistoryAction& moveAction = session->undoActions.back();
        Check(moveAction.pages.empty() && moveAction.replacementPages.empty(),
              "the move history keeps no whole-page snapshot");
        Check(moveAction.beforeObjects.size() == 1U
                && moveAction.afterObjects.size() == 1U
                && moveAction.pageIndices == std::vector<std::size_t> { 0U }
                && moveAction.beforeObjects.front().strokes.size() == 1U
                && moveAction.afterObjects.front().strokes.size() == 1U
                && moveAction.beforeObjects.front().strokeIndices == std::vector<std::size_t> { 0U },
              "the move history retains only the dragged stroke");
    }
    const cnote::Stroke movedStroke = controller.DocumentRef().PageAt(0).strokes[0];
    Check(!cnote::StrokeEqual(originalStroke, movedStroke), "the drag changed the stroke position");
    Check(controller.Undo(), "undo restores the probe stroke position");
    Check(cnote::StrokeEqual(controller.DocumentRef().PageAt(0).strokes[0], originalStroke),
          "undo restores the moved stroke exactly");
    Check(controller.Redo(), "redo reapplies the move");
    Check(cnote::StrokeEqual(controller.DocumentRef().PageAt(0).strokes[0], movedStroke),
          "redo restores the moved stroke exactly");
    controller.ClearSelection();
    Check(controller.Undo(), "undo restores the probe stroke position again");
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

    // (j) Reopening an existing text keeps it visible while editing.  The
    // committed object is excluded from the cached raster during the edit and
    // the transparent editor draws it instead; the reopened editor must show
    // the object's glyphs (and fresh keystrokes), not a blank page.
    {
        ui::CanvasInputSettings input = controller.InputSettings();
        const std::uint32_t savedColor = input.objectColor;
        input.objectColor = 0xFFFF0000U;
        controller.SetInputSettings(input);
        canvas.SetTool(ui::Tool::Text);
        SendMouse(canvas, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
        Check(canvas.TextEditor() != nullptr && canvas.TextEditor()->isVisible(), "reopen probe editor opens");
        if (canvas.TextEditor() != nullptr && canvas.TextEditor()->isVisible()) {
            TypeText(*canvas.TextEditor(), QStringLiteral("WWWW MMMM"));
            QCoreApplication::processEvents();
            const InkBox typing = RedInk(canvas.grab().toImage());
            canvas.CommitActiveEdit();
            QCoreApplication::processEvents();
            canvas.repaint();
            const InkBox committed = RedInk(canvas.grab().toImage());

            const QPointF textPt = canvas.PageToScreen(0, {
                controller.DocumentRef().PageAt(0).texts.back().x + 4.0f,
                controller.DocumentRef().PageAt(0).texts.back().y + 4.0f });
            SendMouse(canvas, QEvent::MouseButtonPress, textPt, Qt::LeftButton, Qt::LeftButton);
            SendMouse(canvas, QEvent::MouseButtonRelease, textPt, Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents();
            Check(controller.EditingTextIndex().has_value(), "clicking the committed text reopens it");
            const InkBox reopened = RedInk(canvas.grab().toImage());
            TypeText(*canvas.TextEditor(), QStringLiteral(" WWWW"));
            QCoreApplication::processEvents();
            const InkBox edited = RedInk(canvas.grab().toImage());
            std::printf("REOPEN typing=%s committed=%s reopened=%s edited=%s\n",
                        FormatBox(typing).c_str(), FormatBox(committed).c_str(),
                        FormatBox(reopened).c_str(), FormatBox(edited).c_str());
            Check(typing.valid() && committed.valid(), "new text is visible while typing and after commit");
            Check(BoxNear(reopened, committed, static_cast<int>(3 * devicePixelRatio) + 2),
                  "the reopened editor shows the text where it was committed");
            Check(edited.valid() && edited.x1 > reopened.x1, "keystrokes in the reopened editor are visible");
            canvas.CancelActiveEdit();
            QCoreApplication::processEvents();
            Check(controller.Undo(), "undo removes the reopen probe text");
            QCoreApplication::processEvents();
        }
        input.objectColor = savedColor;
        controller.SetInputSettings(input);
    }

    // (k) EditOverlayActive follows the controller, not the canvas cache: a
    // controller-side cancel makes the canvas report inactive immediately and
    // SyncEditOverlay hides the now-stale editor.
    {
        canvas.SetTool(ui::Tool::Text);
        const QPointF at(canvas.rect().center());
        SendMouse(canvas, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        SendMouse(canvas, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::processEvents();
        Check(canvas.EditOverlayActive(), "edit overlay reports active while the controller edits");
        Check(controller.EditingTextIndex().has_value(), "the controller owns the live text edit");

        controller.CancelTextEdit();
        Check(!canvas.EditOverlayActive(), "EditOverlayActive follows the controller, not the cached key");
        canvas.SyncEditOverlay();
        Check(canvas.TextEditor() == nullptr || !canvas.TextEditor()->isVisible(),
              "SyncEditOverlay hides an editor the controller no longer owns");
        Check(controller.DocumentRef().PageAt(0).texts.empty(), "a cancelled text creation removes its placeholder");
        canvas.NotifyDocumentChanged();
        QCoreApplication::processEvents();
    }

    // (i) Precision touchpad routing over overlay chrome.  The sidebar is a
    // child of the main window layered on top of the canvas, so a cursor over
    // it is still "inside the canvas rect"; the Win32 pointer-frame hook used
    // to consume touchpad scrolls anyway and suppress the wheel event the
    // sidebar's own wheelEvent needs.  The route check must walk the window's
    // widget tree like wheel dispatch does.
    QWidget host;
    host.resize(1200, 900);
    ui::CanvasWidget hosted(controller);
    hosted.setParent(&host);
    hosted.setGeometry(0, 0, 1200, 900);
    QWidget overlay(&host);
    overlay.setGeometry(20, 20, 200, 400);
    host.show();
    hosted.show();
    overlay.show();
    QCoreApplication::processEvents();
    const QPoint overOverlay = hosted.mapToGlobal(QPoint(80, 60));
    Check(hosted.rect().contains(hosted.mapFromGlobal(overOverlay)),
          "the overlay sits inside the canvas rect (same overlap as the sidebar)");
    Check(hosted.RoutePrecisionInput(overOverlay) == ui::PrecisionInputRoute::PassThrough,
          "a gesture over the overlay is not claimed by the canvas");
    const QPoint overCanvas = hosted.mapToGlobal(hosted.rect().bottomRight() - QPoint(20, 20));
    Check(hosted.RoutePrecisionInput(overCanvas) == ui::PrecisionInputRoute::Canvas,
          "a gesture over uncovered canvas still routes to the canvas");
    overlay.hide();
    host.hide();
    QCoreApplication::processEvents();



    // (i) Raster budget at max zoom and high DPR.  A page whose full raster
    // exceeds the per-surface budget renders visible tiles (bounded bytes, and
    // at most one paint's worth of them per frame so a zoom settle spreads
    // across frames).  A nearby page that leaves the viewport stays cached, so
    // scrolling back blits instead of re-rendering.
    {
        cnote::AppManager budgetManager;
        cnote::qt::QtDocumentController budgetController(budgetManager);
        budgetController.NewBlankDocument();
        ui::CanvasWidget budgetCanvas(budgetController);
        budgetCanvas.resize(1400, 1400);
        budgetCanvas.SetTool(ui::Tool::Draw);
        budgetCanvas.SetZoom(ui::CanvasWidget::kMaxZoom);
        budgetCanvas.show();
        budgetCanvas.CenterOnPage(0);
        Pump(300);
        budgetCanvas.repaint();
        const std::size_t tiledCount = budgetCanvas.CachedTileCount();
        const std::size_t tiledBytes = budgetCanvas.CachedSurfaceBytes();
        const std::size_t lastPaintBytes = budgetCanvas.LastPaintSurfaceBytes();
        std::printf("RASTER-BUDGET dpr=%.2f zoom=%.2f tiles=%zu bytes=%zu peak=%zu lastPaint=%zu peakPaint=%zu renders=%llu\n",
            devicePixelRatio, static_cast<double>(ui::CanvasWidget::kMaxZoom),
            tiledCount, tiledBytes, budgetCanvas.PeakSurfaceBytes(), lastPaintBytes,
            budgetCanvas.PeakPaintSurfaceBytes(),
            static_cast<unsigned long long>(budgetCanvas.SurfaceRenderCount()));
        Check(tiledCount > 0U, "the max-zoom page renders as tiles above the surface budget");
        Check(budgetCanvas.PeakPaintSurfaceBytes() <= cnote::render::kSurfaceRenderBytesPerPaint,
            "every paint's raster bytes stay inside the per-paint budget");
        Check(tiledBytes <= cnote::render::kCanvasSurfaceCacheBytes,
            "the settled raster cache stays inside the canvas budget");
        Check(budgetCanvas.PeakSurfaceBytes()
                <= cnote::render::kCanvasSurfaceCacheBytes + cnote::render::kSurfaceRenderBytesPerPaint,
            "peak raster bytes stay bounded at max zoom");
        if (devicePixelRatio > 1.5) {
            // The visible tiles need more bytes than one paint may render, so
            // they only all exist because deferred frames filled in the rest.
            Check(tiledBytes > cnote::render::kSurfaceRenderBytesPerPaint,
                "the tile set spans more than one paint's budget (spread across frames)");
        }

        // Tiles draw the real page content at the right place: a red stroke
        // committed at the page centre shows up in the grab.  The commit fast
        // path must composite into the cached tiles (no page re-render) and the
        // composited tiles must match a fresh tiled render of the same page.
        const cnote::Page& page = budgetController.DocumentRef().PageAt(0);
        cnote::StrokeStyle budgetStyle;
        budgetStyle.color = 0xFFFF0000U;
        budgetStyle.width = 8.0f;
        budgetController.SetActiveStrokeStyle(budgetStyle);
        const QPointF center = budgetCanvas.PageToScreen(0, { page.width * 0.5f, page.height * 0.5f });

        // Draw a few strokes first so the page is not trivially sparse when the
        // timing below re-renders it.
        for (int index = 0; index < 12; ++index) {
            const QPointF offset(0.0, -40.0 + (static_cast<double>(index) * 6.0));
            SendMouse(budgetCanvas, QEvent::MouseButtonPress, center + offset + QPointF(-18.0, 0.0),
                Qt::LeftButton, Qt::LeftButton);
            SendMouse(budgetCanvas, QEvent::MouseMove, center + offset + QPointF(18.0, 0.0),
                Qt::NoButton, Qt::LeftButton);
            SendMouse(budgetCanvas, QEvent::MouseButtonRelease, center + offset + QPointF(18.0, 0.0),
                Qt::LeftButton, Qt::NoButton);
        }
        QCoreApplication::processEvents();
        Pump(300);
        budgetCanvas.repaint();
        const std::size_t tilesBeforeCommit = budgetCanvas.CachedTileCount();
        const std::uint64_t rendersBeforeCommit = budgetCanvas.SurfaceRenderCount();

        // A stroke spanning a tile edge (tile origins are multiples of
        // render::PageTileSize's aligned grid in device pixels).
        const ui::CanvasPageView commitView = budgetCanvas.CalculatePageView(0);
        cnote::render::PageRenderParams tileParams;
        tileParams.scale = commitView.scale;
        tileParams.devicePixelRatio = static_cast<float>(devicePixelRatio);
        tileParams.textPixelRatio = 1.0f;
        const int tileSize = cnote::render::PageTileSize(tileParams);
        const double deviceScale = static_cast<double>(commitView.scale) * devicePixelRatio;
        const double centerDeviceX = (static_cast<double>(page.width) * 0.5) * deviceScale;
        const int boundary = std::max(1, static_cast<int>(std::llround(centerDeviceX / tileSize))) * tileSize;
        const double boundaryPageX = boundary / deviceScale;
        const QPointF spanCenter = budgetCanvas.PageToScreen(
            0, { static_cast<float>(boundaryPageX), page.height * 0.5f });

        QElapsedTimer commitClock;
        commitClock.start();
        SendMouse(budgetCanvas, QEvent::MouseButtonPress, spanCenter + QPointF(-8.0, 0.0),
            Qt::LeftButton, Qt::LeftButton);
        SendMouse(budgetCanvas, QEvent::MouseMove, spanCenter + QPointF(8.0, 0.0),
            Qt::NoButton, Qt::LeftButton);
        SendMouse(budgetCanvas, QEvent::MouseButtonRelease, spanCenter + QPointF(8.0, 0.0),
            Qt::LeftButton, Qt::NoButton);
        const qint64 commitMicros = commitClock.nsecsElapsed() / 1000;
        QCoreApplication::processEvents();
        budgetCanvas.repaint();
        const std::uint64_t rendersAfterCommit = budgetCanvas.SurfaceRenderCount();
        const QImage composited = budgetCanvas.grab().toImage();
        const InkBox tiledInk = RedInk(composited);
        std::printf("RASTER-BUDGET commit us=%lld rendersBefore=%llu rendersAfter=%llu tiles=%zu->%zu tiledInk=%s\n",
            static_cast<long long>(commitMicros),
            static_cast<unsigned long long>(rendersBeforeCommit),
            static_cast<unsigned long long>(rendersAfterCommit),
            tilesBeforeCommit, budgetCanvas.CachedTileCount(), FormatBox(tiledInk).c_str());
        Check(tiledInk.valid(), "the tiled surface paints committed ink at max zoom");
        Check(rendersAfterCommit == rendersBeforeCommit,
            "committing a stroke on a tiled page composites instead of re-rendering it");
        Check(budgetCanvas.CachedTileCount() >= tilesBeforeCommit,
            "committing keeps the cached tiles");

        // Reference: a fresh tiled render of the same page.  Undo then redo
        // bumps the epoch twice, invalidating the tiles; the next paints
        // rasterize them from the page content (what the old commit fallback
        // forced).  Time that re-render for the latency comparison.
        QElapsedTimer rerenderClock;
        rerenderClock.start();
        Check(budgetController.Undo(), "tiled-commit probe: undo removes the stroke");
        Check(budgetController.Redo(), "tiled-commit probe: redo restores the stroke");
        std::size_t rerenderTiles = budgetCanvas.CachedTileCount();
        for (int attempt = 0; attempt < 12; ++attempt) {
            Pump(20);
            budgetCanvas.repaint();
            const std::size_t now = budgetCanvas.CachedTileCount();
            if (now == rerenderTiles && attempt > 0) {
                break;
            }
            rerenderTiles = now;
        }
        const qint64 rerenderMicros = rerenderClock.nsecsElapsed() / 1000;
        const QImage fresh = budgetCanvas.grab().toImage();
        const std::uint64_t rendersAfterFresh = budgetCanvas.SurfaceRenderCount();
        int differingPixels = 0;
        int maxChannelDelta = 0;
        for (int y = 0; y < fresh.height(); ++y) {
            if (std::memcmp(fresh.constScanLine(y), composited.constScanLine(y),
                    static_cast<std::size_t>(fresh.width()) * 4U) == 0) {
                continue;
            }
            for (int x = 0; x < fresh.width(); ++x) {
                const QRgb a = composited.pixel(x, y);
                const QRgb b = fresh.pixel(x, y);
                if (a != b) {
                    ++differingPixels;
                    for (int shift = 0; shift <= 24; shift += 8) {
                        maxChannelDelta = std::max(maxChannelDelta,
                            std::abs((static_cast<int>((a >> shift) & 0xFFU))
                                - static_cast<int>((b >> shift) & 0xFFU)));
                    }
                }
            }
        }
        std::printf("TILED-COMMIT freshRender us=%lld freshRenders=%llu differingPixels=%d maxChannelDelta=%d\n",
            static_cast<long long>(rerenderMicros),
            static_cast<unsigned long long>(rendersAfterFresh - rendersAfterCommit),
            differingPixels, maxChannelDelta);
        Check(rendersAfterFresh > rendersAfterCommit,
            "tiled-commit probe: the epoch bump really re-rendered the tiles");
        Check(differingPixels == 0,
            "composited tiles match a fresh tiled render (no seam at the tile edge)");
        Check(budgetCanvas.PeakSurfaceBytes()
                <= cnote::render::kCanvasSurfaceCacheBytes + cnote::render::kSurfaceRenderBytesPerPaint,
            "committed-ink repaint keeps peak raster bytes bounded");
    }
    {
        cnote::AppManager nearbyManager;
        cnote::qt::QtDocumentController nearbyController(nearbyManager);
        nearbyController.NewBlankDocument();
        ui::CanvasWidget nearbyCanvas(nearbyController);
        nearbyCanvas.resize(1200, 900);
        nearbyCanvas.SetTool(ui::Tool::Select);
        nearbyCanvas.show();
        QCoreApplication::processEvents();
        ui::NewPageDraft draft {};
        Check(nearbyController.InsertPages(1, 1, draft) == 1U, "nearby-cache probe: second page appended");
        nearbyCanvas.SetZoom(1.0f);
        Pump(200);
        nearbyCanvas.CenterOnPage(0);
        nearbyCanvas.repaint();
        nearbyCanvas.CenterOnPage(1);
        Pump(20);
        nearbyCanvas.repaint();
        const std::uint64_t beforeReturn = nearbyCanvas.SurfaceRenderCount();
        const std::size_t cachedBeforeReturn = nearbyCanvas.CachedSurfaceCount();
        nearbyCanvas.CenterOnPage(0);
        Pump(20);
        nearbyCanvas.repaint();
        std::printf("NEARBY-CACHE cachedPages=%zu rendersBefore=%llu rendersAfter=%llu bytes=%zu\n",
            cachedBeforeReturn, static_cast<unsigned long long>(beforeReturn),
            static_cast<unsigned long long>(nearbyCanvas.SurfaceRenderCount()),
            nearbyCanvas.CachedSurfaceBytes());
        Check(cachedBeforeReturn >= 2U, "the page that left the viewport stays cached");
        Check(nearbyCanvas.SurfaceRenderCount() == beforeReturn,
            "scrolling back to a nearby page reuses its cached raster");
    }

    cnote::latex::Shutdown();
    std::printf(g_failures == 0 ? "ALL CANVAS GEOMETRY CHECKS PASSED\n" : "%d CANVAS GEOMETRY CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
