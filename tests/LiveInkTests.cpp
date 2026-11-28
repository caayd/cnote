// Live ink (PERF-024/PERF-025) regression test.
//
// * A dotted stroke must use the incremental live-ink layer: the per-sample
//   paint submits a bounded number of centerline points instead of replaying
//   the whole smoothed path, and the dash pattern carried across append
//   batches must land on the same pixels as a single full repaint of the same
//   pending stroke.  The suite runs at DPR 1.0 and 1.75 (QT_SCALE_FACTOR like
//   the canvas geometry test).
// * The snap ghost's fade must blit a retained raster: a fade tick paints no
//   vector geometry, while the opacity compositing still matches the old
//   per-frame vector draw within 8-bit rounding.
// * The live layer's growth still copies its surface (the 96 px slack is a
//   deliberate amortization, PERF-025); the test reports the measured copied
//   bytes and guards against losing the amortization.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QWidget>

#include "Document.h"
#include "qt/QtDocumentController.h"
#include "render/PageRenderer.h"
#include "shell/AppManager.h"
#include "shell/CaptureDriver.h"
#include "ui/CanvasWidget.h"

namespace {

namespace ui = cnote::ui;
using cnote::Vec2;
using cnote::Stroke;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
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

struct DiffStats {
    int maxChannel = 0;
    long long differing = 0;
    long long total = 0;
};

// Per-channel RGB difference of two same-sized device images over `region`.
DiffStats CompareRegion(const QImage& a, const QImage& b, const QRect& region, int threshold) {
    DiffStats stats;
    const QRect area = region.intersected(QRect(QPoint(0, 0), a.size()))
        .intersected(QRect(QPoint(0, 0), b.size()));
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            const int diff = std::max({
                std::abs(qRed(pa) - qRed(pb)),
                std::abs(qGreen(pa) - qGreen(pb)),
                std::abs(qBlue(pa) - qBlue(pb)),
            });
            stats.maxChannel = std::max(stats.maxChannel, diff);
            ++stats.total;
            if (diff > threshold) {
                ++stats.differing;
            }
        }
    }
    return stats;
}

// Ghost-only pixels: pure-red ink composited at `opacity` over the white page
// has a green and blue channel of 255 * (1 - opacity) and red 255.  `clean`
// is the same frame after the ghost is gone; requiring it to be white there
// excludes the snapped shape's own antialiased red edges.
struct GhostPixels {
    int count = 0;
    double meanGreen = 0.0;
};

GhostPixels CountGhostPixels(const QImage& image, const QImage& clean, int minGreen, int maxGreen) {
    GhostPixels result;
    int greenSum = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            const QRgb background = clean.pixel(x, y);
            if (qRed(pixel) >= 254
                && std::abs(qGreen(pixel) - qBlue(pixel)) <= 1
                && qGreen(pixel) >= minGreen && qGreen(pixel) <= maxGreen
                && qRed(background) >= 250 && qGreen(background) >= 250 && qBlue(background) >= 250) {
                ++result.count;
                greenSum += qGreen(pixel);
            }
        }
    }
    if (result.count > 0) {
        result.meanGreen = static_cast<double>(greenSum) / static_cast<double>(result.count);
    }
    return result;
}

cnote::Stroke MakeCurve(bool dotted) {
    cnote::Stroke stroke;
    stroke.style.color = 0xFFFF0000U;
    stroke.style.width = 8.0f;
    stroke.style.dotted = dotted;
    for (int index = 0; index < 30; ++index) {
        stroke.points.push_back(Vec2 {
            30.0f + static_cast<float>(index) * 9.0f,
            120.0f + 34.0f * std::sin(static_cast<float>(index) * 0.38f),
        });
    }
    stroke.smoothing = 0.55f;
    return stroke;
}

// One raster-vs-vector parity check at the suite's DPR: the ghost renders the
// stroke once into a raster and later blits it at a changing opacity; every
// opacity must match painting the vector with that opacity within rounding.
void CheckGhostRasterParity(float devicePixelRatio) {
    for (const bool dotted : { false, true }) {
        const cnote::Stroke stroke = MakeCurve(dotted);
        cnote::render::PageRenderParams params;
        params.scale = 1.15f;
        params.devicePixelRatio = devicePixelRatio;
        params.textPixelRatio = 1.0f;
        const std::optional<cnote::render::StrokePreviewRaster> raster =
            cnote::render::RenderStrokePreviewRaster(stroke, params);
        Check(raster.has_value(), std::string("ghost raster renders the ") + (dotted ? "dotted" : "solid") + " stroke");
        if (!raster.has_value()) {
            continue;
        }
        QImage vector(raster->image.size(), QImage::Format_ARGB32_Premultiplied);
        vector.fill(Qt::transparent);
        {
            QPainter painter(&vector);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.translate(
                -raster->pageRect.x() * raster->deviceScale,
                -raster->pageRect.y() * raster->deviceScale);
            cnote::render::PageRenderParams deviceParams = params;
            deviceParams.scale = raster->deviceScale;
            deviceParams.devicePixelRatio = 1.0f;
            if (params.textPixelRatio.has_value()) {
                deviceParams.textPixelRatio = *params.textPixelRatio * params.devicePixelRatio;
            }
            cnote::render::PaintStrokePreview(painter, stroke, deviceParams, 1.0f);
        }
        for (const float opacity : { 1.0f, 0.3f, 0.12f }) {
            const auto composite = [opacity](const QImage& source) {
                QImage image(source.size(), QImage::Format_RGB32);
                image.fill(Qt::white);
                QPainter painter(&image);
                painter.setOpacity(opacity);
                painter.drawImage(0, 0, source);
                return image;
            };
            const QImage rasterFrame = composite(raster->image);
            const QImage vectorFrame = composite(vector);
            const DiffStats diff = CompareRegion(rasterFrame, vectorFrame, QRect(QPoint(0, 0), rasterFrame.size()), 2);
            std::printf("GHOST-PARITY dotted=%d dpr=%.2f opacity=%.2f maxDiff=%d differing(>2)=%lld/%lld bytes=%lld\n",
                dotted ? 1 : 0, devicePixelRatio, opacity, diff.maxChannel, diff.differing, diff.total,
                static_cast<long long>(raster->image.sizeInBytes()));
            Check(diff.maxChannel <= 3,
                std::string("ghost raster matches the vector at opacity ") + std::to_string(opacity)
                    + (dotted ? " (dotted)" : " (solid)"));
            Check(diff.differing <= diff.total / 100,
                std::string("ghost raster/vector opacity differences stay under 1%")
                    + (dotted ? " (dotted)" : " (solid)"));
        }
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
        std::fprintf(stderr, "live ink: QApplication did not initialize within 30 s\n");
        std::fflush(stderr);
        std::_Exit(7);
    });
    watchdog.detach();

    QApplication app(argc, argv);
    appReady.store(true);

    cnote::shell::OffscreenWindows offscreen;
    app.installEventFilter(&offscreen);

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.NewBlankDocument();

    ui::CanvasWidget canvas(controller);
    offscreen.SetPrimaryWindow(&canvas);
    canvas.resize(1200, 900);
    canvas.show();
    QCoreApplication::processEvents();

    const double devicePixelRatio = canvas.devicePixelRatioF();
    const double requestedDpr = qgetenv("QT_SCALE_FACTOR").toDouble();
    std::printf("PLATFORM %s dpr=%.4f requestedDpr=%.4f\n",
        QApplication::platformName().toUtf8().constData(), devicePixelRatio, requestedDpr);
    if (requestedDpr > 0.0) {
        Check(std::abs(devicePixelRatio - requestedDpr) < 0.01,
            "devicePixelRatioF matches QT_SCALE_FACTOR");
    }

    CheckGhostRasterParity(static_cast<float>(devicePixelRatio));

    cnote::StrokeStyle strokeStyle;
    strokeStyle.color = 0xFFFF0000U;
    strokeStyle.width = 8.0f;
    controller.SetActiveStrokeStyle(strokeStyle);

    canvas.SetTool(ui::Tool::Draw);
    canvas.SetZoom(1.0f);
    Pump(200);
    canvas.CenterOnPage(0);
    Pump(20);
    canvas.repaint();

    ui::InteractionModifiers modifiers = canvas.Router().Modifiers();
    modifiers.dotted = true;
    modifiers.straightLine = false;
    canvas.SetInteractionModifiers(modifiers);

    // Keep the gesture inside the page at this DPR: zoom 1 is a reference
    // zoom, so the logical page rect shrinks as the device pixel ratio grows.
    const ui::CanvasPageView pageView = canvas.CalculatePageView(0);
    std::printf("PROBE scale=%.4f pageRect=%.1f,%.1f..%.1f,%.1f\n",
        pageView.scale, pageView.rect.left(), pageView.rect.top(),
        pageView.rect.right(), pageView.rect.bottom());
    const QPointF pageCenter = pageView.rect.center();
    constexpr int kSamples = 240;
    const double span = pageView.rect.width() * 0.55;
    const double amplitude = std::min(55.0, pageView.rect.height() * 0.12);
    const QPointF start(pageCenter.x() - span * 0.5, pageCenter.y());
    const auto livePoint = [&](int step) {
        return start + QPointF(span * step / kSamples, amplitude * std::sin(step * 0.09));
    };

    SendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    std::uint64_t maxPaintPoints = 0;
    std::uint64_t maxLatePaintPoints = 0;
    std::uint64_t firstMax = 0;
    std::uint64_t liveLayerPeakBytes = 0;
    bool paintedEverySample = true;
    for (int step = 1; step <= kSamples; ++step) {
        const QPointF point = livePoint(step);
        SendMouse(canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
        canvas.repaint();
        QCoreApplication::processEvents();
        const std::uint64_t painted = canvas.LastLiveStrokePaintPointCount();
        paintedEverySample = paintedEverySample && painted > 0;
        maxPaintPoints = std::max(maxPaintPoints, painted);
        if (step <= 40) {
            firstMax = std::max(firstMax, painted);
        }
        if (step > kSamples - 40) {
            maxLatePaintPoints = std::max(maxLatePaintPoints, painted);
        }
        liveLayerPeakBytes = std::max(liveLayerPeakBytes, static_cast<std::uint64_t>(canvas.LiveStrokeLayerBytes()));
    }
    const std::uint64_t copyBytes = canvas.LiveStrokeLayerCopyBytes();
    const std::uint64_t growths = canvas.LiveStrokeLayerGrowthCount();
    const std::uint64_t finalLayerBytes = canvas.LiveStrokeLayerBytes();
    const std::uint64_t blitBytes = canvas.LiveStrokeLayerBlitBytes();
    const QSize layerPixels = canvas.LiveStrokeLayerPixelSize();
    std::printf(
        "LIVE-INK dotted samples=%d paintPoints max=%llu first40=%llu last40=%llu "
        "layerPx=%dx%d layerBytes=%llu peak=%llu growths=%llu copied=%llu blitted=%llu\n",
        kSamples,
        static_cast<unsigned long long>(maxPaintPoints),
        static_cast<unsigned long long>(firstMax),
        static_cast<unsigned long long>(maxLatePaintPoints),
        layerPixels.width(), layerPixels.height(),
        static_cast<unsigned long long>(finalLayerBytes),
        static_cast<unsigned long long>(liveLayerPeakBytes),
        static_cast<unsigned long long>(growths),
        static_cast<unsigned long long>(copyBytes),
        static_cast<unsigned long long>(blitBytes));
    Check(paintedEverySample, "the dotted live stroke paints at every sample");
    // PERF-024: a sample's paint must not grow with stroke length.  The
    // incremental batch feeds a sample or two plus the two-segment tail; the
    // pre-fix dotted fallback submitted the whole (growing) smoothed path.
    Check(maxPaintPoints <= 24, "dotted per-sample paint stays bounded");
    Check(maxLatePaintPoints <= firstMax + 8, "dotted per-sample paint does not grow with length");
    // PERF-025: the deliberate 96 px growth slack keeps reallocations rare.
    // Without it nearly every sample that tips the bounding box copies again.
    // The copies must stay a small fraction of the surfaces the same paints
    // already blit; otherwise geometric growth would be worth the complexity.
    std::printf("LIVE-GROWTH growths/sample=%.3f copied/blitted=%.3f copied/final=%.3f\n",
        static_cast<double>(growths) / static_cast<double>(kSamples),
        blitBytes > 0 ? static_cast<double>(copyBytes) / static_cast<double>(blitBytes) : 0.0,
        finalLayerBytes > 0 ? static_cast<double>(copyBytes) / static_cast<double>(finalLayerBytes) : 0.0);
    Check(growths <= static_cast<std::uint64_t>(kSamples) / 3U,
        "the live layer growth slack amortizes reallocations over many samples");
    Check(blitBytes > 0U && copyBytes * 4U <= blitBytes,
        "live layer growth copies stay far below the per-paint blit volume");

    // Incremental preview (many append batches, carried dash phase) against a
    // full repaint of the same pending stroke (one batch, phase from zero).
    const QImage incremental = canvas.grab().toImage();
    canvas.SetStrokeDotted(true);  // resets the layer; the next paint re-composites
    canvas.repaint();
    QCoreApplication::processEvents();
    const QImage fullRepaint = canvas.grab().toImage();
    const InkBox incrementalBox = RedInk(incremental);
    const InkBox fullBox = RedInk(fullRepaint);
    const QRect dotRegion = QRect(
        QPoint(std::min(incrementalBox.x0, fullBox.x0), std::min(incrementalBox.y0, fullBox.y0)),
        QPoint(std::max(incrementalBox.x1, fullBox.x1), std::max(incrementalBox.y1, fullBox.y1)));
    const DiffStats layerDiff = CompareRegion(incremental, fullRepaint, dotRegion, 8);
    std::printf("LIVE-PARITY incremental/full box=%d,%d..%d,%d maxDiff=%d differing(>8)=%lld/%lld\n",
        dotRegion.left(), dotRegion.top(), dotRegion.right(), dotRegion.bottom(),
        layerDiff.maxChannel, layerDiff.differing, layerDiff.total);
    Check(incrementalBox.valid() && fullBox.valid(), "dotted preview has ink");
    Check(BoxNear(incrementalBox, fullBox, static_cast<int>(std::lround(std::max(2.0, 2.0 * devicePixelRatio)))),
        "incremental dotted ink lands on the full repaint's pixels");
    Check(layerDiff.maxChannel <= 64, "incremental dashed seams stay within rounding of one full repaint");
    Check(layerDiff.differing <= std::max<long long>(32, layerDiff.total / 200),
        "less than 0.5% of the dotted bbox differs from one full repaint");

    // The live preview against the committed page raster: the dotted stroke
    // must not jump at release, at either DPR.
    SendMouse(canvas, QEvent::MouseButtonRelease, livePoint(kSamples), Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    canvas.repaint();
    const QImage committed = canvas.grab().toImage();
    const InkBox committedBox = RedInk(committed);
    const QRect commitRegion = QRect(
        QPoint(std::min(incrementalBox.x0, committedBox.x0), std::min(incrementalBox.y0, committedBox.y0)),
        QPoint(std::max(incrementalBox.x1, committedBox.x1), std::max(incrementalBox.y1, committedBox.y1)));
    const DiffStats commitDiff = CompareRegion(fullRepaint, committed, commitRegion, 8);
    std::printf("LIVE-COMMIT preview/committed box=%d,%d..%d,%d maxDiff=%d differing(>8)=%lld/%lld\n",
        commitRegion.left(), commitRegion.top(), commitRegion.right(), commitRegion.bottom(),
        commitDiff.maxChannel, commitDiff.differing, commitDiff.total);
    Check(BoxNear(incrementalBox, committedBox, static_cast<int>(std::lround(std::max(2.0, 3.0 * devicePixelRatio)))),
        "committed dotted stroke lands on the preview pixels");
    Check(commitDiff.maxChannel <= 96, "committed dotted pixels stay within rounding of the preview");
    Check(commitDiff.differing <= std::max<long long>(64, commitDiff.total / 100),
        "less than 1% of the dotted bbox differs after commit");
    Check(controller.DocumentRef().PageAt(0).strokes.size() == 1U, "the dotted stroke committed once");
    Check(controller.Undo(), "undo removes the dotted test stroke");
    QCoreApplication::processEvents();

    modifiers.dotted = false;
    canvas.SetInteractionModifiers(modifiers);
    canvas.repaint();
    QCoreApplication::processEvents();

    // --- Snap ghost fade --------------------------------------------------------
    // A held freehand circle snaps to a shape; its freehand original becomes
    // the ghost at 0.3 opacity, then fades over 250 ms after release.
    // Deliberately not a perfect circle: the wobble is recognizable as an
    // ellipse but leaves parts of the freehand original outside the snapped
    // shape, so the ghost is visible on its own (not just under the shape).
    const auto circlePoint = [](int step, const QPointF& circleCenter) {
        const double angle = static_cast<double>(step) * 2.0 * 3.14159265358979323846 / 120.0;
        const double radius = 100.0 + 8.0 * std::sin(3.0 * angle) + 4.0 * std::cos(5.0 * angle);
        return circleCenter + QPointF(radius * std::cos(angle), radius * std::sin(angle));
    };
    SendMouse(canvas, QEvent::MouseButtonPress, circlePoint(0, pageCenter), Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= 120; ++step) {
        SendMouse(canvas, QEvent::MouseMove, circlePoint(step, pageCenter), Qt::NoButton, Qt::LeftButton);
    }
    // Resting starts the 500 ms hold timer; snapping keeps the pointer down.
    Pump(650);
    QCoreApplication::processEvents();
    canvas.repaint();
    const cnote::Stroke* snapped = controller.PendingStroke();
    Check(snapped != nullptr && snapped->points.size() < 100U,
        "the held freehand circle snapped to the shape");
    Check(canvas.SnapGhostRasterPaintCount() > 0U, "the snapped ghost paints from its raster");
    Check(canvas.SnapGhostVectorPaintCount() == 0U, "the held ghost paints no vector geometry");
    Check(canvas.SnapGhostRasterBytes() > 0U, "the ghost raster is retained");
    std::printf("GHOST-HELD rasterPaints=%llu vectorPaints=%llu bytes=%llu\n",
        static_cast<unsigned long long>(canvas.SnapGhostRasterPaintCount()),
        static_cast<unsigned long long>(canvas.SnapGhostVectorPaintCount()),
        static_cast<unsigned long long>(canvas.SnapGhostRasterBytes()));

    const QImage heldFrame = canvas.grab().toImage();

    SendMouse(canvas, QEvent::MouseButtonRelease, circlePoint(120, pageCenter), Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
    const std::uint64_t vectorPaintsAtRelease = canvas.SnapGhostVectorPaintCount();
    const std::uint64_t rasterPaintsAtRelease = canvas.SnapGhostRasterPaintCount();
    for (int tick = 0; tick < 6; ++tick) {
        Pump(20);
        canvas.repaint();
        QCoreApplication::processEvents();
    }
    const QImage fadeFrame = canvas.grab().toImage();
    const std::uint64_t fadeVectorPaints = canvas.SnapGhostVectorPaintCount() - vectorPaintsAtRelease;
    const std::uint64_t fadeRasterPaints = canvas.SnapGhostRasterPaintCount() - rasterPaintsAtRelease;
    std::printf("GHOST-FADE ticks=6 rasterPaints=%llu vectorPaints=%llu\n",
        static_cast<unsigned long long>(fadeRasterPaints),
        static_cast<unsigned long long>(fadeVectorPaints));
    Check(fadeRasterPaints >= 3U, "fade ticks blit the retained ghost raster");
    Check(fadeVectorPaints == 0U, "fade ticks paint no vector ghost geometry");

    Pump(400);
    canvas.repaint();
    QCoreApplication::processEvents();
    const QImage cleanFrame = canvas.grab().toImage();
    Check(canvas.SnapGhostRasterBytes() == 0U, "the finished fade releases the ghost raster");

    // The freehand original deviates from the snapped circle; at those pixels
    // the held ghost is pure red at the fixed 0.3 opacity over the white page.
    const GhostPixels heldGhost = CountGhostPixels(heldFrame, cleanFrame, 176, 181);
    const GhostPixels fadeGhost = CountGhostPixels(fadeFrame, cleanFrame, 182, 254);
    std::printf("GHOST-PIXELS held=%d (meanGreen=%.1f) fade=%d (meanGreen=%.1f)\n",
        heldGhost.count, heldGhost.meanGreen, fadeGhost.count, fadeGhost.meanGreen);
    Check(heldGhost.count > 0, "the held ghost shows its 0.3-opacity freehand original");
    Check(fadeGhost.count > 0, "the ghost is still visible partway through the fade");
    Check(fadeGhost.meanGreen > heldGhost.meanGreen,
        "the fade lowers the ghost's opacity below the held 0.3");
    Check(controller.DocumentRef().PageAt(0).strokes.size() == 1U, "the snapped shape committed once");

    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
