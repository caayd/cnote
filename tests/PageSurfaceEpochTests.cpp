// Deterministic offscreen regression test for the page-surface cache key.
//
// Reported defect: "strokes occasionally seem to lag, causing sharp jumps
// from the start and end points", reproducible when drawing on a newly added
// page of a multi-page project.
//
// Root cause: PageRenderParams::contentRevision carried the session-global
// revision, so every stroke commit invalidated the surface signature of EVERY
// page, and the first paint after a commit re-rendered every other visible
// page (a dense page is ~1 s on the GUI thread).  While the GUI thread was
// inside that paint, Qt compressed queued pointer-move events, so the next
// stroke recorded only the press point plus a late coalesced sample - the
// sharp jump.
//
// The fix keys page surfaces on Document::PageEpoch, which only the mutated
// page's commits bump.  This test drives the real CanvasWidget +
// QtDocumentController offscreen and counts surface re-renders:
//   (a) a stroke committed on page 1 while page 0 is also visible must not
//       re-render anything (the committed page's raster is composited,
//       not re-rendered),
//   (b) the same holds after a mid-document InsertPage shifted indices,
//   (c) a pointer sample in the gap between pages must not append a {0,0}
//       spike: it maps through the stroke's own page and clamps to the edge.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QWidget>

#include "Document.h"
#include "qt/QtDocumentController.h"
#include "shell/AppManager.h"
#include "ui/CanvasWidget.h"
#include "ui/PageDefaultsQt.h"

namespace {

namespace ui = cnote::ui;
using cnote::Vec2;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
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

// One full stroke gesture: press at `from`, a few moves to `to`, release.
void DrawStroke(ui::CanvasWidget& canvas, const QPointF& from, const QPointF& to) {
    SendMouse(canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= 4; ++step) {
        const QPointF at = from + (to - from) * (static_cast<qreal>(step) / 4.0);
        SendMouse(canvas, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton);
    }
    SendMouse(canvas, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
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
        std::fprintf(stderr, "page surface epochs: QApplication did not initialize within 30 s\n");
        std::fflush(stderr);
        std::_Exit(7);
    });
    watchdog.detach();

    QApplication app(argc, argv);
    appReady.store(true);

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.NewBlankDocument();

    ui::CanvasWidget canvas(controller);
    canvas.resize(1400, 1400);
    canvas.show();
    QCoreApplication::processEvents();

    // Minimum zoom so stacked pages fit vertically; the canvas only renders
    // pages intersecting the paint region, so they must be on screen for the
    // re-render counts to be meaningful.
    canvas.SetTool(ui::Tool::Draw);
    canvas.SetZoom(ui::CanvasWidget::kMinZoom);
    Pump(200);  // let the zoom session settle so surfaces render at this scale
    const QRectF canvasRect(canvas.rect());

    ui::NewPageDraft draft {};
    Check(controller.InsertPages(1, 1, draft) == 1U, "second page appended");
    Check(controller.DocumentRef().PageCount() == 2U, "document has two pages");

    const ui::CanvasPageView view0 = canvas.CalculatePageView(0);
    const ui::CanvasPageView view1 = canvas.CalculatePageView(1);
    Check(view0.valid && view1.valid, "both page views are valid");
    Check(view0.rect.intersects(canvasRect) && view1.rect.intersects(canvasRect),
          "both pages are on screen at min zoom");
    std::printf("VIEWS page0=(%.0f,%.0f %.0fx%.0f) page1=(%.0f,%.0f %.0fx%.0f) scale=%.3f\n",
                view0.rect.left(), view0.rect.top(), view0.rect.width(), view0.rect.height(),
                view1.rect.left(), view1.rect.top(), view1.rect.width(), view1.rect.height(),
                static_cast<double>(view1.scale));

    // Warm the caches, then draw on page 1: no surface may re-render (the
    // committed page's raster is composited in place, and page 0's epoch did
    // not change).
    canvas.repaint();
    const std::uint64_t warmRenders = canvas.SurfaceRenderCount();

    const QPointF strokeFrom = view1.rect.center() + QPointF(-20.0, -20.0);
    const QPointF strokeTo = view1.rect.center() + QPointF(20.0, 20.0);
    DrawStroke(canvas, strokeFrom, strokeTo);
    canvas.repaint();
    QCoreApplication::processEvents();
    canvas.repaint();

    const std::uint64_t afterStroke = canvas.SurfaceRenderCount();
    std::printf("RENDERS warm=%llu afterStrokeOnPage1=%llu\n",
                static_cast<unsigned long long>(warmRenders),
                static_cast<unsigned long long>(afterStroke));
    Check(controller.DocumentRef().PageAt(1).strokes.size() == 1U,
          "stroke committed on the new page");
    Check(afterStroke - warmRenders == 0U,
          "stroke commit on page 1 did not re-render page 0's cached surface");

    // A second stroke on the same page keeps the same guarantee (composite
    // commit re-stamps, never re-renders).
    DrawStroke(canvas, strokeFrom + QPointF(0.0, -10.0), strokeTo + QPointF(0.0, -10.0));
    canvas.repaint();
    QCoreApplication::processEvents();
    canvas.repaint();
    std::printf("RENDERS afterSecond=%llu\n", static_cast<unsigned long long>(canvas.SurfaceRenderCount()));
    Check(canvas.SurfaceRenderCount() - warmRenders == 0U,
          "second stroke commit also re-rendered nothing");

    // (b) Insert a page mid-document: indices below the insert shift, and a
    // stroke on the last page must still not force re-renders elsewhere.
    Check(controller.InsertPages(1, 1, draft) == 1U, "middle page inserted");
    // InsertPages(1, 1) shifts the old page 1 (two committed strokes) to
    // index 2; page 2 assertions count deltas off that base.
    const std::size_t page2StrokesBeforeInsert = controller.DocumentRef().PageAt(2).strokes.size();
    canvas.NotifyDocumentChanged();
    canvas.CenterOnPage(2);
    canvas.repaint();
    QCoreApplication::processEvents();
    canvas.repaint();
    const std::uint64_t afterInsert = canvas.SurfaceRenderCount();

    const ui::CanvasPageView view2 = canvas.CalculatePageView(2);
    Check(view2.valid && view2.rect.intersects(canvasRect), "last page still on screen");
    std::printf("VIEWS2 page2=(%.0f,%.0f %.0fx%.0f) canvas=(0,0 %.0fx%.0f)\n",
                view2.rect.left(), view2.rect.top(), view2.rect.width(), view2.rect.height(),
                canvasRect.width(), canvasRect.height());
    DrawStroke(canvas, view2.rect.center() + QPointF(-20.0, -20.0), view2.rect.center() + QPointF(20.0, 20.0));
    canvas.repaint();
    QCoreApplication::processEvents();
    canvas.repaint();
    std::printf("RENDERS afterInsert=%llu afterStrokeOnPage2=%llu\n",
                static_cast<unsigned long long>(afterInsert),
                static_cast<unsigned long long>(canvas.SurfaceRenderCount()));
    std::printf("STROKES p0=%zu p1=%zu p2=%zu\n",
                controller.DocumentRef().PageAt(0).strokes.size(),
                controller.DocumentRef().PageAt(1).strokes.size(),
                controller.DocumentRef().PageAt(2).strokes.size());
    Check(controller.DocumentRef().PageAt(2).strokes.size() == page2StrokesBeforeInsert + 1U,
          "stroke committed on last page");
    Check(canvas.SurfaceRenderCount() - afterInsert == 0U,
          "stroke after mid-document insert did not re-render other pages");

    // (c) Gap sample: drag from page 0 down into the kPageGap band and back.
    // The sample between the pages must clamp to page 0's bottom edge instead
    // of producing a {0,0} spike to the page origin.
    canvas.CenterOnPage(0);
    canvas.repaint();
    const ui::CanvasPageView view0Top = canvas.CalculatePageView(0);
    const QPointF gapPoint(view0Top.rect.center().x(), view0Top.rect.bottom() + 10.0);
    Check(gapPoint.y() < view0Top.rect.bottom() + ui::CanvasWidget::kPageGap,
          "sample point sits in the gap band");

    const cnote::Page& page0 = controller.DocumentRef().PageAt(0);
    const std::size_t strokesBefore = page0.strokes.size();
    const QPointF startOnPage = view0Top.rect.center() + QPointF(0.0, -40.0);
    const QPointF endOnPage = view0Top.rect.center() + QPointF(0.0, -20.0);
    SendMouse(canvas, QEvent::MouseButtonPress, startOnPage, Qt::LeftButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseMove, gapPoint, Qt::NoButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseMove, endOnPage, Qt::NoButton, Qt::LeftButton);
    SendMouse(canvas, QEvent::MouseButtonRelease, endOnPage, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();

    Check(controller.DocumentRef().PageAt(0).strokes.size() == strokesBefore + 1U,
          "gap-crossing stroke committed on page 0");
    if (controller.DocumentRef().PageAt(0).strokes.size() > strokesBefore) {
        const cnote::Stroke& stroke = controller.DocumentRef().PageAt(0).strokes.back();
        // The gesture is a straight vertical pull at the page's horizontal
        // centre: every committed point keeps that x.  A {0,0} miss sample is
        // smoothed toward the origin (one lerp step ≈ 30% of the way), which
        // would pull x out of this band - that is the spike the bug drew.
        const float centreX = page0.width * 0.5f;
        bool outOfBand = false;
        bool inBounds = true;
        for (const Vec2& point : stroke.points) {
            if (std::abs(point.x - centreX) > 25.0f) {
                outOfBand = true;
            }
            if (point.x < -0.01f || point.x > page0.width + 0.01f
                || point.y < -0.01f || point.y > page0.height + 0.01f) {
                inBounds = false;
            }
        }
        Check(inBounds, "gap sample clamped inside page 0");
        Check(!outOfBand, "no spike toward the page origin from the gap sample");
        std::printf("GAP stroke points=%zu last=(%.1f,%.1f)\n",
                    stroke.points.size(), stroke.points.back().x, stroke.points.back().y);
    }

    if (g_failures == 0) {
        std::printf("PASS: page surface epochs\n");
        return 0;
    }
    std::printf("FAILED: %d checks\n", g_failures);
    return 1;
}
