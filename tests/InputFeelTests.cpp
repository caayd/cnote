// Deterministic offscreen input-feel regression test.  Two user-reported defects
// on a precision-touchpad laptop:
//
//   1. "the laptop touchpad gestures feel laggy/slow and backwards for left/right
//      scrolling".  Contract (migration plan 5.3 and the reference front end):
//      ordinary wheel notches (QWheelEvent::angleDelta) keep the reference
//      WheelScrollPixels conversion - three scroll lines of 18 logical pixels
//      (54 px) per 120-unit notch - and animate through an accumulating
//      exponential chase (CanvasWidget::StartSmoothScroll): every notch adds
//      its full distance to the running target, so rapid notches sum instead
//      of capping at one notch per animation restart, while precision
//      scrolling (QWheelEvent::pixelDelta) is applied 1:1 inside its
//      own event, with no animation and no second smoothing, because the OS
//      already delivers smooth, momentum-carrying deltas.  Horizontal motion
//      keeps the native direction: the reference panned with the raw
//      WM_MOUSEHWHEEL delta -SmoothPanBy(+...) (MessageLoop.cpp:539) and Qt
//      negates that delta (Qt 6.11 qwindowspointerhandler.cpp:748-752), so Qt's
//      positive angleDelta().x() must pan +54 px, not -54.  A single physical
//      gesture must never be applied twice (native adapter pan + the wheel or
//      native gesture Qt synthesises on top of it).
//
//   2. "the custom cursors dont display at all, it always uses the default mouse
//      cursor".  Contract: the configured draw cursor (settings
//      draw_cursor_kind / draw_cursor_size, reference
//      App::DrawCursorForCurrentSize App.cpp:7465-7485) is shown on the canvas
//      while the drawing tool is active, as a device-pixel-ratio-correct pixmap
//      of the configured logical diameter; the precision crosshair stays the
//      system crosshair; every other tool and the default selection keep the
//      plain system arrow; the cursor is rebuilt when the setting changes and
//      re-applied when the pointer enters or leaves the canvas.
//
// Injecting real QWheelEvents/mouse events into the real CanvasWidget pins the
// behaviour above; touchpad FEEL on real hardware cannot be automated here (no
// touchpad in CI, and the offscreen platform has no cursor surface - a widget
// grab cannot capture the painted cursor), so the desktop checks are listed in
// the run's output and must be confirmed by hand.
//
// The same binary runs at QT_SCALE_FACTOR 1.0 and 1.75 (registered twice in
// CTest) so the cursor pixmap's device pixel ratio and the 1:1 pixelDelta pan
// are checked at a non-1 DPR as well.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include <QApplication>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPixmap>
#include <QPointingDevice>
#include <QScreen>
#include <QWheelEvent>
#include <QWidget>

#include "qt/QtDocumentController.h"
#include "shell/AppManager.h"
#include "settings/SettingsStoreQt.h"
#include "ui/CanvasWidget.h"
#include "ui/InteractionRouter.h"
#include "ui/PrecisionInputFilterQt.h"

#if defined(Q_OS_WIN)
#include "platform/windows/PrecisionTouchpadQt.h"
#endif

namespace {

namespace ui = cnote::ui;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void CheckNear(double actual, double expected, double tolerance, const std::string& what) {
    if (std::abs(actual - expected) > tolerance) {
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

std::string FormatPoint(const QPointF& point) {
    char buffer[80] = {};
    std::snprintf(buffer, sizeof(buffer), "(%.2f,%.2f)", point.x(), point.y());
    return buffer;
}

QPointF ViewportPan(const ui::CanvasWidget& canvas) {
    return QPointF(canvas.ViewportPanX(), canvas.ViewportPanY());
}

// One injected wheel event and what it did to the viewport: the delta visible
// the instant the event returned (a direct application is already complete; an
// animated one has not moved yet) and the delta after the event loop settles.
struct WheelOutcome {
    QPointF immediate {};
    QPointF settled {};
    qreal zoomBefore = 1.0;
    qreal zoomAfter = 1.0;
    bool animated = false;
    bool dropped = false;

    [[nodiscard]] bool isNull() const {
        return immediate.isNull() && settled.isNull();
    }
};

WheelOutcome InjectWheel(
    ui::CanvasWidget& canvas,
    const QPoint& pixelDelta,
    const QPoint& angleDelta,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier,
    int settleMs = 200) {
    const QPointF before = ViewportPan(canvas);
    WheelOutcome outcome;
    outcome.zoomBefore = canvas.Zoom();
    const QPointF position(canvas.width() / 2.0, canvas.height() / 2.0);
    QWheelEvent event(
        position,
        canvas.mapToGlobal(position.toPoint()),
        pixelDelta,
        angleDelta,
        Qt::NoButton,
        modifiers,
        Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(&canvas, &event);

    outcome.immediate = ViewportPan(canvas) - before;
    if (angleDelta == QPoint() && pixelDelta == QPoint()) {
        outcome.dropped = true;
    }
    Pump(settleMs);
    outcome.settled = ViewportPan(canvas) - before;
    outcome.zoomAfter = canvas.Zoom();
    outcome.animated = !outcome.dropped && outcome.immediate.isNull() && !outcome.settled.isNull();
    std::printf(
        "WHEEL pixel=(%d,%d) angle=(%d,%d) immediate=%s settled=%s zoom=%.4f mode=%s\n",
        pixelDelta.x(),
        pixelDelta.y(),
        angleDelta.x(),
        angleDelta.y(),
        FormatPoint(outcome.immediate).c_str(),
        FormatPoint(outcome.settled).c_str(),
        outcome.zoomAfter,
        outcome.animated ? "animated" : "direct");
    return outcome;
}

void SendMouse(
    QWidget& widget,
    QEvent::Type type,
    const QPointF& logical,
    Qt::MouseButton button,
    Qt::MouseButtons buttons) {
    QMouseEvent event(type, logical, widget.mapToGlobal(logical.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

void SendEnter(QWidget& widget) {
    const QPointF position(widget.width() / 2.0, widget.height() / 2.0);
    QEnterEvent event(position, position, widget.mapToGlobal(position.toPoint()));
    QApplication::sendEvent(&widget, &event);
}

// Device-pixel bounding box of the cursor pixmap's opaque pixels, and whether
// the four corners stay transparent (i.e. the shape is a disc, not a square).
struct CursorShape {
    int width = 0;
    int height = 0;
    int opaqueLeft = 0;
    int opaqueTop = 0;
    int opaqueRight = -1;
    int opaqueBottom = -1;
    bool cornersTransparent = false;
};

CursorShape MeasureCursorShape(const QCursor& cursor) {
    CursorShape shape;
    const QImage image = cursor.pixmap().toImage();
    if (image.isNull()) {
        return shape;
    }
    shape.width = image.width();
    shape.height = image.height();
    shape.opaqueLeft = image.width();
    shape.opaqueTop = image.height();
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) < 32) {
                continue;
            }
            shape.opaqueLeft = std::min(shape.opaqueLeft, x);
            shape.opaqueTop = std::min(shape.opaqueTop, y);
            shape.opaqueRight = std::max(shape.opaqueRight, x);
            shape.opaqueBottom = std::max(shape.opaqueBottom, y);
        }
    }
    shape.cornersTransparent = qAlpha(image.pixel(0, 0)) < 32
        && qAlpha(image.pixel(image.width() - 1, 0)) < 32
        && qAlpha(image.pixel(0, image.height() - 1)) < 32
        && qAlpha(image.pixel(image.width() - 1, image.height() - 1)) < 32;
    return shape;
}

// --- defect 1: wheel/precision scrolling ------------------------------------

void PrecisionScrollIsDirectAndOneToOne(ui::CanvasWidget& canvas) {
    std::printf("-- precision pixelDelta is applied 1:1 inside its own event\n");
    canvas.ResetView();
    Pump(60);

    // A single small touchpad-like frame: the whole delta must be in the
    // viewport before the event loop runs again, and nothing may follow it.
    const WheelOutcome single = InjectWheel(canvas, QPoint(3, 4), QPoint());
    CheckNear(single.immediate.x(), 3.0, 0.01, "one pixelDelta event pans x by its whole delta immediately");
    CheckNear(single.immediate.y(), 4.0, 0.01, "one pixelDelta event pans y by its whole delta immediately");
    Check(!single.animated, "a pixelDelta event is not animated");
    CheckNear(single.settled.y(), single.immediate.y(), 0.01, "a pixelDelta pan keeps its delta after the settle window");

    // ~100 Hz stream of small frames (what a precision touchpad delivers): every
    // frame lands in the viewport as it arrives, so the accumulated distance is
    // exactly the sum of the frames - no animation tail eats it.
    canvas.ResetView();
    Pump(60);
    QPointF accumulated;
    for (int frame = 0; frame < 10; ++frame) {
        const WheelOutcome frameOutcome = InjectWheel(canvas, QPoint(2, -3), QPoint(), Qt::NoModifier, 0);
        accumulated += frameOutcome.immediate;
        CheckNear(frameOutcome.immediate.x(), 2.0, 0.01, "stream frame pans x by 2 px in its own event");
        CheckNear(frameOutcome.immediate.y(), -3.0, 0.01, "stream frame pans y by -3 px in its own event");
        QElapsedTimer gap;
        gap.start();
        while (gap.elapsed() < 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CheckNear(accumulated.x(), 20.0, 0.01, "ten 2-px frames accumulate exactly 20 px of horizontal pan");
    CheckNear(accumulated.y(), -30.0, 0.01, "ten -3-px frames accumulate exactly -30 px of vertical pan");

    // Horizontal pixel deltas keep Qt's own sign in both directions.
    canvas.ResetView();
    Pump(60);
    const WheelOutcome right = InjectWheel(canvas, QPoint(7, 0), QPoint());
    CheckNear(right.immediate.x(), 7.0, 0.01, "positive horizontal pixelDelta pans the content right");
    const WheelOutcome left = InjectWheel(canvas, QPoint(-7, 0), QPoint());
    CheckNear(left.immediate.x(), -7.0, 0.01, "negative horizontal pixelDelta pans the content left");

    // Direct panning must cancel a notch animation in flight instead of letting
    // the chase overwrite it on its next tick (reference App::PanBy).
    canvas.ResetView();
    Pump(60);
    const QPointF notchBefore = ViewportPan(canvas);
    static_cast<void>(InjectWheel(canvas, QPoint(), QPoint(0, 120), Qt::NoModifier, 0));
    QPointF midAnimation;
    QElapsedTimer inFlight;
    inFlight.start();
    while (inFlight.elapsed() < 40 && midAnimation.isNull()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        midAnimation = ViewportPan(canvas) - notchBefore;
    }
    std::printf("NOTCH in-flight offset after %lld ms: %s\n",
        static_cast<long long>(inFlight.elapsed()), FormatPoint(midAnimation).c_str());
    Check(!midAnimation.isNull() && midAnimation.y() > 0.0 && midAnimation.y() < 54.0,
        "the notch animation is still in flight when the precision frame arrives");
    const QPointF preciseBefore = ViewportPan(canvas);
    static_cast<void>(InjectWheel(canvas, QPoint(5, 0), QPoint(), Qt::NoModifier, 0));
    const QPointF afterPrecise = ViewportPan(canvas) - preciseBefore;
    CheckNear(afterPrecise.x(), 5.0, 0.01, "a pixelDelta pan during a notch animation lands 1:1");
    Pump(200);
    CheckNear((ViewportPan(canvas) - preciseBefore).x(), 5.0, 0.01, "the cancelled notch animation does not move the viewport again");
    CheckNear((ViewportPan(canvas) - preciseBefore).y(), 0.0, 0.01, "no animation tail is left behind");
}

void WheelNotchesAccumulateAndAnimate(ui::CanvasWidget& canvas) {
    std::printf("-- ordinary notches animate 54 px each and rapid notches accumulate\n");
    canvas.ResetView();
    Pump(60);

    const WheelOutcome notch = InjectWheel(canvas, QPoint(), QPoint(0, 120), Qt::NoModifier, 200);
    CheckNear(notch.immediate.y(), 0.0, 0.01, "a notch is not applied within its own event (it animates)");
    Check(notch.animated, "a single notch uses the smooth-scroll animation");
    CheckNear(notch.settled.y(), 54.0, 0.5, "one notch scrolls three 18-px lines (54 logical px)");

    const WheelOutcome twoBack = InjectWheel(canvas, QPoint(), QPoint(0, -240), Qt::NoModifier, 200);
    CheckNear(twoBack.settled.y(), -108.0, 0.5, "two backward notches scroll 108 px the other way");
    Check(twoBack.animated, "backward notches animate too");

    // Regression: notches arriving while an animation is still in flight must
    // add their full distance to the running target.  The old restart-per-notch
    // cubic re-anchored on the current pan and discarded the unfinished tail,
    // so a fast flick travelled less than its notches.
    canvas.ResetView();
    Pump(60);
    const QPointF flickBefore = ViewportPan(canvas);
    for (int notchIndex = 0; notchIndex < 5; ++notchIndex) {
        static_cast<void>(InjectWheel(canvas, QPoint(), QPoint(0, 120), Qt::NoModifier, 0));
        QElapsedTimer gap;
        gap.start();
        while (gap.elapsed() < 20) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }
    Pump(400);
    CheckNear((ViewportPan(canvas) - flickBefore).y(), 270.0, 1.0,
        "five notches 20 ms apart accumulate the full 270 px (no tail discarded)");

    // Qt negates WM_MOUSEHWHEEL's delta; the reference panned with the raw Win32
    // delta, so the Qt-convention equivalent keeps the native direction.
    const WheelOutcome right = InjectWheel(canvas, QPoint(), QPoint(-120, 0), Qt::NoModifier, 200);
    CheckNear(right.settled.x(), -54.0, 0.5, "a rightward horizontal notch (Qt -120) pans the content left");
    const WheelOutcome left = InjectWheel(canvas, QPoint(), QPoint(120, 0), Qt::NoModifier, 200);
    CheckNear(left.settled.x(), 54.0, 0.5, "a leftward horizontal notch (Qt +120) pans the content right");
    Check(right.settled.x() < 0.0 && left.settled.x() > 0.0,
        "the two horizontal notch directions have opposite signs");

    // Ctrl+wheel still zooms (and re-anchors the pan at the cursor, which is the
    // reference zoom behaviour) instead of scrolling.
    const float zoomBefore = canvas.Zoom();
    const WheelOutcome zoom = InjectWheel(canvas, QPoint(), QPoint(0, 120), Qt::ControlModifier, 0);
    CheckNear(zoom.zoomAfter, static_cast<double>(zoomBefore) * 1.1, 0.001, "ctrl+wheel zooms by the reference 1.1 factor");
    canvas.ResetView();
    Pump(60);
}

void NoGestureIsAppliedTwice(ui::CanvasWidget& canvas) {
    std::printf("-- one gesture, one pan: the native adapter and Qt's duplicates\n");
    // The Windows adapter is the side that consumes the pointer frames, and the
    // filter is the side that can drop what Qt synthesises from them; they have
    // to share one router or the same physical gesture pans twice.
#if defined(Q_OS_WIN)
    ui::PrecisionTouchpadQt touchpad(canvas);
    ui::PrecisionInputFilterQt filter(touchpad.Router());
    canvas.installEventFilter(&filter);
    // The adapter cannot be installed without touchpad hardware (Install() is
    // exercised by precision_touchpad_qt), so the two facts it reports after a
    // consumed frame - API availability and the consumed frame itself - are
    // driven through its public router here.
    touchpad.Router().SetNativeAdapterAvailable(true);

    canvas.ResetView();
    Pump(60);
    const QPoint localCentre(canvas.width() / 2, canvas.height() / 2);
    const QPoint globalCentre = canvas.mapToGlobal(localCentre);

    ui::TouchpadGestureSample begin;
    begin.phase = ui::TouchpadGestureSample::Phase::Begin;
    begin.screenAnchor = globalCentre;
    begin.cumulativeScale = 1.0f;
    touchpad.ApplyTouchpadSample(begin);

    const QPointF beforeNative = ViewportPan(canvas);
    ui::TouchpadGestureSample update;
    update.phase = ui::TouchpadGestureSample::Phase::Update;
    update.screenAnchor = globalCentre;
    update.translationX = 12.0f;
    update.translationY = -9.0f;
    touchpad.ApplyTouchpadSample(update);
    const QPointF nativeDelta = ViewportPan(canvas) - beforeNative;
    CheckNear(nativeDelta.x(), 12.0, 0.01, "the native adapter's frame pans x by its translation");
    CheckNear(nativeDelta.y(), -9.0, 0.01, "the native adapter's frame pans y by its translation");

    // The adapter consumed that frame; Qt's copies of it must not move the view.
    touchpad.Router().NoteNativeFrameConsumed();
    const WheelOutcome duplicateWheel = InjectWheel(canvas, QPoint(), QPoint(0, 120), Qt::NoModifier, 200);
    Check(duplicateWheel.isNull(), "the wheel Qt synthesises for a consumed native frame is dropped");
    CheckNear((ViewportPan(canvas) - beforeNative).x(), 12.0, 0.01, "the dropped duplicate adds no horizontal pan");
    CheckNear((ViewportPan(canvas) - beforeNative).y(), -9.0, 0.01, "the dropped duplicate adds no vertical pan");

    const QPointF beforeGestureDuplicate = ViewportPan(canvas);
    const QPointF position(localCentre);
    QNativeGestureEvent pan(
        Qt::PanNativeGesture,
        QPointingDevice::primaryPointingDevice(),
        2,
        position,
        position,
        QPointF(globalCentre),
        0.0,
        QPointF(30.0, -20.0));
    QApplication::sendEvent(&canvas, &pan);
    Check(pan.isAccepted(), "the Qt-native pan duplicate of a consumed native frame is consumed");
    Check(ViewportPan(canvas) == beforeGestureDuplicate, "the native gesture duplicate does not pan a second time");

    // Ending the gesture (the adapter's own end frame) stops suppression, so a
    // plain wheel after the touchpad gesture still scrolls normally.
    ui::TouchpadGestureSample end;
    end.phase = ui::TouchpadGestureSample::Phase::End;
    end.screenAnchor = globalCentre;
    touchpad.ApplyTouchpadSample(end);
    Check(!touchpad.Router().SuppressingDuplicates(), "the gesture end stops duplicate suppression");
    const WheelOutcome afterGesture = InjectWheel(canvas, QPoint(), QPoint(0, 120), Qt::NoModifier, 200);
    CheckNear(afterGesture.settled.y(), 54.0, 0.5, "a wheel after the touchpad gesture scrolls normally again");

    canvas.removeEventFilter(&filter);
    touchpad.Router().SetNativeAdapterAvailable(false);
#else
    std::printf("SKIP native precision-touchpad duplicate checks (not Windows)\n");
#endif
}

// --- defect 2: the configured canvas cursor ---------------------------------

void ConfiguredCursorIsShown(ui::CanvasWidget& canvas) {
    const qreal ratio = canvas.devicePixelRatioF();
    std::printf("-- configured canvas cursor at dpr %.4f\n", ratio);

    // The active palette slot the reference passes to CreateBrushCursor
    // (App::DrawCursorForCurrentSize App.cpp:7481).
    constexpr std::uint32_t kCursorColor = 0xFF3366CCU;
    constexpr std::uint32_t kOtherCursorColor = 0xFFCC3366U;

    // Default: no custom pixmap, the plain system arrow.
    canvas.SetTool(ui::Tool::Select);
    Check(canvas.cursor().shape() == Qt::ArrowCursor, "the default tool shows the system arrow");
    Check(canvas.cursor().pixmap().isNull(), "the default cursor carries no custom pixmap");

    // Dot: the reference brush disc at the configured logical diameter.
    canvas.SetDrawCursorSettings(cnote::settings::DrawCursorKind::Dot, 8.0f, kCursorColor);
    Check(canvas.cursor().shape() == Qt::ArrowCursor && canvas.cursor().pixmap().isNull(),
        "the setting change alone does not put the drawing cursor on a non-drawing tool");
    canvas.SetTool(ui::Tool::Draw);
    Check(!canvas.cursor().pixmap().isNull(), "the drawing tool shows the configured dot cursor");
    CheckNear(canvas.cursor().pixmap().devicePixelRatio(), ratio, 0.001, "the cursor pixmap carries the widget's device pixel ratio");
    CursorShape dot = MeasureCursorShape(canvas.cursor());
    std::printf("CURSOR dot logical=8.0 device=%dx%d opaque=%s\n",
        dot.width, dot.height,
        (std::to_string(dot.opaqueRight - dot.opaqueLeft + 1) + "x" + std::to_string(dot.opaqueBottom - dot.opaqueTop + 1)).c_str());
    CheckNear(static_cast<double>(dot.opaqueRight - dot.opaqueLeft + 1), 8.0 * ratio, 2.0, "the dot's drawn diameter is the configured logical size in device pixels");
    CheckNear(static_cast<double>(dot.opaqueBottom - dot.opaqueTop + 1), 8.0 * ratio, 2.0, "the dot's drawn height matches its width");
    Check(dot.cornersTransparent, "the dot cursor is a disc, not an opaque square");

    // The hotspot is a DEVICE-INDEPENDENT pixel - Qt multiplies it by the
    // pixmap's device pixel ratio when it builds the native cursor - so it must
    // be the pixmap's DIP centre, not its extent/2 device-pixel centre.  Passing
    // the device centre put the hotspot `ratio` times too far down and to the
    // right, which is why every stroke on a scaled display landed at the dot's
    // bottom-right corner.
    const QPoint hotspot = canvas.cursor().hotSpot();
    const int dipCentre = static_cast<int>(std::lround(static_cast<double>(dot.width) / (2.0 * ratio)));
    CheckNear(static_cast<double>(hotspot.x()), dipCentre, 0.5, "the draw cursor's hotspot is the pixmap's DIP centre on x");
    CheckNear(static_cast<double>(hotspot.y()), dipCentre, 0.5, "the draw cursor's hotspot is the pixmap's DIP centre on y");

    // The disc carries the active palette colour (App.cpp:7481), not the fixed
    // white the port filled it with before; the centre pixel is opaque fill.
    const QImage dotImage = canvas.cursor().pixmap().toImage();
    const QRgb dotCentre = dotImage.pixel(dotImage.width() / 2, dotImage.height() / 2);
    Check(qAlpha(dotCentre) == 255, "the dot's centre is fully opaque");
    Check((dotCentre & 0x00FFFFFFU) == (kCursorColor & 0x00FFFFFFU), "the dot is filled with the configured palette colour");
    // The reference caches on diameter *and* colour (App.cpp:7482-7484).
    canvas.SetDrawCursorSettings(cnote::settings::DrawCursorKind::Dot, 8.0f, kOtherCursorColor);
    const QRgb recoloured = canvas.cursor().pixmap().toImage().pixel(dotImage.width() / 2, dotImage.height() / 2);
    Check((recoloured & 0x00FFFFFFU) == (kOtherCursorColor & 0x00FFFFFFU), "a palette colour change rebuilds the dot cursor");

    // Size setting: applied without touching the tool.
    canvas.SetDrawCursorSettings(cnote::settings::DrawCursorKind::Dot, 20.0f, kCursorColor);
    const CursorShape bigger = MeasureCursorShape(canvas.cursor());
    CheckNear(static_cast<double>(bigger.opaqueRight - bigger.opaqueLeft + 1), 20.0 * ratio, 2.0, "the cursor size setting resizes the dot");
    canvas.SetDrawCursorSettings(cnote::settings::DrawCursorKind::Dot, 100.0f, kCursorColor);
    const CursorShape clampedLarge = MeasureCursorShape(canvas.cursor());
    CheckNear(static_cast<double>(clampedLarge.opaqueRight - clampedLarge.opaqueLeft + 1), 24.0 * ratio, 2.0, "the dot diameter clamps at 24 logical px (App.cpp:171)");
    canvas.SetDrawCursorSettings(cnote::settings::DrawCursorKind::Dot, 1.0f, kCursorColor);
    const CursorShape clampedSmall = MeasureCursorShape(canvas.cursor());
    CheckNear(static_cast<double>(clampedSmall.opaqueRight - clampedSmall.opaqueLeft + 1), 4.0 * ratio, 2.0, "the dot diameter clamps at 4 logical px (App.cpp:170)");

    // Precision crosshair: the reference uses the system cross, no pixmap.
    canvas.SetDrawCursorSettings(cnote::settings::DrawCursorKind::PrecisionCrosshair, 8.0f, kCursorColor);
    Check(canvas.cursor().shape() == Qt::CrossCursor, "the precision crosshair uses the system crosshair");
    Check(canvas.cursor().pixmap().isNull(), "the crosshair needs no custom pixmap");
    canvas.SetDrawCursorSettings(cnote::settings::DrawCursorKind::Dot, 8.0f, kCursorColor);
    Check(!canvas.cursor().pixmap().isNull(), "switching back to the dot rebuilds the pixmap");

    // Eraser: App::EraserCursorForCurrentSize (App.cpp:7459-7468) builds
    // CreateSquareBrushCursor(diameter, RGB(255,255,255)) - a white square with a
    // black outline, side EraserDiameterPixels() = max(3.5, eraseBrushSize * 4)
    // * 2 logical pixels (App.cpp:7426-7432) - instead of the system cross.
    canvas.SetEraseBrushSize(3.0f);
    canvas.SetTool(ui::Tool::Erase);
    Check(!canvas.cursor().pixmap().isNull(), "the eraser tool shows the reference square cursor");
    const QImage eraserImage = canvas.cursor().pixmap().toImage();
    const CursorShape eraser = MeasureCursorShape(canvas.cursor());
    const double eraserSide = std::max(3.5, 3.0 * 4.0) * 2.0 * ratio;
    std::printf("CURSOR eraser logical=24.0 device=%dx%d opaque=%s\n",
        eraser.width, eraser.height,
        (std::to_string(eraser.opaqueRight - eraser.opaqueLeft + 1) + "x" + std::to_string(eraser.opaqueBottom - eraser.opaqueTop + 1)).c_str());
    // The image keeps a 1px transparent margin, so "square" means the opaque
    // bounding box's own corner is filled - a disc's would be transparent.
    Check(qAlpha(eraserImage.pixel(eraser.opaqueLeft, eraser.opaqueTop)) >= 32,
        "the eraser cursor is a square, not a disc");
    CheckNear(static_cast<double>(eraser.opaqueRight - eraser.opaqueLeft + 1), eraserSide, 2.0, "the eraser square's side is max(3.5, eraseBrushSize * 4) * 2 logical px (App.cpp:7430-7432)");
    CheckNear(static_cast<double>(eraser.opaqueBottom - eraser.opaqueTop + 1), eraserSide, 2.0, "the eraser square's height matches its side");
    const QRgb eraserCentre = eraserImage.pixel(eraserImage.width() / 2, eraserImage.height() / 2);
    Check(qAlpha(eraserCentre) == 255 && qRed(eraserCentre) > 240 && qGreen(eraserCentre) > 240 && qBlue(eraserCentre) > 240,
        "the eraser square is filled white");
    const QRgb eraserEdge = eraserImage.pixel((eraser.opaqueLeft + eraser.opaqueRight) / 2, eraser.opaqueTop);
    Check(qAlpha(eraserEdge) >= 32 && qRed(eraserEdge) < 96 && qGreen(eraserEdge) < 96 && qBlue(eraserEdge) < 96,
        "the eraser square is outlined in black");
    // The same setter also feeds the erase hit test's radius, so a size change
    // must resize the cursor.
    canvas.SetEraseBrushSize(6.0f);
    const CursorShape biggerEraser = MeasureCursorShape(canvas.cursor());
    CheckNear(static_cast<double>(biggerEraser.opaqueRight - biggerEraser.opaqueLeft + 1), std::max(3.5, 6.0 * 4.0) * 2.0 * ratio, 2.0,
        "the erase brush size resizes the eraser square");
    canvas.SetEraseBrushSize(3.0f);
    canvas.SetTool(ui::Tool::Draw);
    Check(!canvas.cursor().pixmap().isNull(), "the draw tool shows its disc again after erasing");

    // The interaction still wins: a pan shows the closed hand and hands the
    // drawing cursor back when it ends.
    const QPointF centre(canvas.width() / 2.0, canvas.height() / 2.0);
    SendMouse(canvas, QEvent::MouseButtonPress, centre, Qt::MiddleButton, Qt::MiddleButton);
    Check(canvas.cursor().shape() == Qt::ClosedHandCursor, "a middle-button pan shows the closed hand");
    SendMouse(canvas, QEvent::MouseButtonRelease, centre + QPointF(4.0, 4.0), Qt::MiddleButton, Qt::NoButton);
    Check(!canvas.cursor().pixmap().isNull(), "ending the pan restores the drawing tool's cursor");

    // Re-applied when the pointer enters or leaves: something else (chrome, a
    // child widget, a transient override) must not be able to leave the canvas
    // without its configured cursor.
    canvas.setCursor(Qt::ArrowCursor);
    Check(canvas.cursor().pixmap().isNull(), "the simulated override really replaced the cursor");
    SendEnter(canvas);
    Check(!canvas.cursor().pixmap().isNull(), "entering the canvas re-applies the configured cursor");
    canvas.setCursor(Qt::ArrowCursor);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&canvas, &leave);
    Check(!canvas.cursor().pixmap().isNull(), "leaving the canvas re-applies the configured cursor");

    // Default again: the arrow comes back with no custom pixmap.
    canvas.SetTool(ui::Tool::Select);
    Check(canvas.cursor().shape() == Qt::ArrowCursor, "returning to a non-drawing tool restores the system arrow");
    Check(canvas.cursor().pixmap().isNull(), "the default cursor has no pixmap again");
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
        std::fprintf(stderr, "input feel: QApplication did not initialize within 30 s "
            "(missing Qt platform plugin? QT_PLUGIN_PATH=%s)\n",
            qgetenv("QT_PLUGIN_PATH").constData());
        std::fflush(stderr);
        std::_Exit(7);
    });
    watchdog.detach();

    QApplication app(argc, argv);
    appReady.store(true);
    std::printf("PLATFORM %s dpr=%.4f requestedDpr=%.4f\n",
        QApplication::platformName().toUtf8().constData(),
        QGuiApplication::primaryScreen() != nullptr ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0,
        qgetenv("QT_SCALE_FACTOR").toDouble());

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.NewBlankDocument();

    ui::CanvasWidget canvas(controller);
    canvas.resize(1200, 900);
    canvas.show();
    QCoreApplication::processEvents();

    PrecisionScrollIsDirectAndOneToOne(canvas);
    WheelNotchesAccumulateAndAnimate(canvas);
    NoGestureIsAppliedTwice(canvas);
    ConfiguredCursorIsShown(canvas);

    std::printf(
        "\nDESKTOP CHECKS (cannot be automated: no touchpad, and a widget grab cannot\n"
        "capture the painted cursor).  On the laptop, please confirm:\n"
        "  1. two-finger scroll is smooth and carries momentum (no 90 ms lag per\n"
        "     frame), and swiping left/right moves the page in the same direction as\n"
        "     before the Qt port (swipe right shows content that was off-screen right);\n"
        "  2. the draw tool shows the configured dot (Settings > Draw Cursor: Dot,\n"
        "     size), the crosshair option shows a crosshair, and every other tool\n"
        "     shows the normal arrow;\n"
        "  3. the dot keeps its on-screen size on the 175%% display.\n");

    if (g_failures != 0) {
        std::printf("INPUT FEEL: %d FAIL\n", g_failures);
        return 1;
    }
    std::printf("INPUT FEEL: PASS\n");
    return 0;
}
