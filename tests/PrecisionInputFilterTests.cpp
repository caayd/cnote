// Offscreen widget-level precision-input filter test (plan steps 5.3/5.4).
//
// Installs cnote::ui::PrecisionInputFilterQt on a real QWidget that implements
// the sink, exactly the way the canvas will, and sends real Qt events through
// QApplication::sendEvent.  It pins the contract the Qt path must keep:
// ordinary wheel/mouse input is byte-for-byte unchanged with the filter
// installed, a consumed native touchpad frame drops exactly its Qt duplicate,
// native pinch/pan reaches the sink cursor-anchored, and gestures routed to the
// sidebar stay with the widget.

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QWheelEvent>
#include <QWidget>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "ui/PrecisionInputFilterQt.h"
#include "ui/PrecisionInputSink.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

// Minimal canvas stand-in: records what the filter delivers and keeps the
// widget's own wheel path observable.
class TestCanvas : public QWidget, public cnote::ui::PrecisionInputSink {
public:
    cnote::ui::PrecisionInputRoute route = cnote::ui::PrecisionInputRoute::Canvas;

    int wheelEvents = 0;
    QPoint lastPixelDelta {};
    QPoint lastAngleDelta {};
    Qt::KeyboardModifiers lastWheelModifiers = Qt::NoModifier;
    int nativeGestureEvents = 0;
    int mousePressEvents = 0;

    int beginCount = 0;
    int endCount = 0;
    int cursorOnCount = 0;
    int cursorOffCount = 0;
    int zoomCount = 0;
    int panCount = 0;
    qreal lastZoomScale = 0.0;
    QPoint lastZoomAnchor {};
    QPointF lastPanDelta {};
    qreal zoomFactor = 1.0;   // accumulates ZoomAtAnchor like the canvas zoom
    QPointF panTotal {};

    [[nodiscard]] cnote::ui::PrecisionInputRoute RoutePrecisionInput(const QPoint&) const override {
        return route;
    }
    void BeginPrecisionGesture() override { ++beginCount; }
    void SetPrecisionGestureCursor(bool active) override {
        if (active) {
            ++cursorOnCount;
        } else {
            ++cursorOffCount;
        }
    }
    void ZoomAtAnchor(qreal cumulativeScale, const QPoint& globalAnchor) override {
        ++zoomCount;
        lastZoomScale = cumulativeScale;
        lastZoomAnchor = globalAnchor;
        zoomFactor = cumulativeScale;
    }
    void PanBy(qreal deltaX, qreal deltaY) override {
        ++panCount;
        lastPanDelta = QPointF(deltaX, deltaY);
        panTotal += lastPanDelta;
    }
    void EndPrecisionGesture() override { ++endCount; }

protected:
    void wheelEvent(QWheelEvent* event) override {
        ++wheelEvents;
        lastPixelDelta = event->pixelDelta();
        lastAngleDelta = event->angleDelta();
        lastWheelModifiers = event->modifiers();
        event->accept();
    }
    void mousePressEvent(QMouseEvent* event) override {
        ++mousePressEvents;
        event->accept();
    }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::NativeGesture) {
            ++nativeGestureEvents;
        }
        return QWidget::event(event);
    }
};

void SendWheel(TestCanvas& canvas, const QPoint& pixelDelta, const QPoint& angleDelta, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWheelEvent event(
        QPointF(50, 60),
        QPointF(450, 360),
        pixelDelta,
        angleDelta,
        Qt::NoButton,
        modifiers,
        Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(&canvas, &event);
}

void SendNativeGesture(
    TestCanvas& canvas,
    Qt::NativeGestureType type,
    const QPointF& delta = QPointF(),
    qreal value = 0.0) {
    QNativeGestureEvent event(
        type,
        QPointingDevice::primaryPointingDevice(),
        2,
        QPointF(30, 40),
        QPointF(30, 40),
        QPointF(430, 340),
        value,
        delta);
    QApplication::sendEvent(&canvas, &event);
}

// 1. Ordinary input is unchanged with the filter installed.
void PlainInputUnchanged() {
    TestCanvas plain;
    SendWheel(plain, QPoint(12, -7), QPoint(0, -120));
    Check(plain.wheelEvents == 1, "plain widget receives the wheel event");
    Check(plain.lastPixelDelta == QPoint(12, -7) && plain.lastAngleDelta == QPoint(0, -120),
        "plain widget keeps pixelDelta and angleDelta");

    TestCanvas filtered;
    cnote::ui::PrecisionInputFilterQt filter(filtered);
    filtered.installEventFilter(&filter);
    SendWheel(filtered, QPoint(12, -7), QPoint(0, -120), Qt::ControlModifier);
    Check(filtered.wheelEvents == 1, "filtered widget still receives the wheel event");
    Check(filtered.lastPixelDelta == QPoint(12, -7) && filtered.lastAngleDelta == QPoint(0, -120),
        "filtered widget sees identical deltas");
    Check(filtered.lastWheelModifiers == Qt::ControlModifier, "ctrl-wheel keeps its modifier for the canvas zoom path");
    Check(filtered.beginCount == 0 && filtered.zoomCount == 0, "the filter applies no wheel zoom of its own");

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(410, 310), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&filtered, &press);
    Check(filtered.mousePressEvents == 1, "mouse input is not consumed by the filter");
}

// 2. A consumed native frame drops exactly the Qt duplicate of that gesture.
void DuplicateSuppression() {
    TestCanvas canvas;
    cnote::ui::PrecisionInputFilterQt filter(canvas);
    canvas.installEventFilter(&filter);

    SendWheel(canvas, QPoint(3, 3), QPoint());
    Check(canvas.wheelEvents == 1, "wheel passes before any native frame");

    filter.router().SetNativeAdapterAvailable(true);
    filter.router().NoteNativeFrameConsumed();
    SendWheel(canvas, QPoint(3, 3), QPoint());
    Check(canvas.wheelEvents == 1, "the duplicate wheel event of a consumed native frame is dropped");

    // The gesture ends: suppression stops and wheels flow again.
    static_cast<void>(filter.router().HandleNativeGestureBegin(QPoint(400, 300)));
    static_cast<void>(filter.router().HandleNativeGestureEnd());
    SendWheel(canvas, QPoint(3, 3), QPoint());
    Check(canvas.wheelEvents == 2, "wheel passes again after the gesture ends");

    // Unavailable adapter: no duplicate can exist, so nothing is suppressed.
    filter.router().SetNativeAdapterAvailable(false);
    filter.router().NoteNativeFrameConsumed();
    SendWheel(canvas, QPoint(3, 3), QPoint());
    Check(canvas.wheelEvents == 3, "fallback path never suppresses wheel events");
}

// 3. Native pinch/pan reaches the sink, cursor-anchored, and ends cleanly.
void NativePinchAndPan() {
    TestCanvas canvas;
    cnote::ui::PrecisionInputFilterQt filter(canvas);
    canvas.installEventFilter(&filter);

    SendNativeGesture(canvas, Qt::BeginNativeGesture);
    Check(canvas.beginCount == 1 && canvas.cursorOnCount == 1, "native gesture begins on the canvas");
    Check(canvas.nativeGestureEvents == 0, "the consumed begin gesture never reaches the widget");

    SendNativeGesture(canvas, Qt::ZoomNativeGesture, QPointF(), 0.5);
    Check(canvas.zoomCount == 1 && std::abs(canvas.lastZoomScale - 1.5) < 1e-9, "native pinch applies (1 + value)");
    Check(canvas.lastZoomAnchor == QPoint(430, 340), "native pinch is anchored at the cursor");

    SendNativeGesture(canvas, Qt::PanNativeGesture, QPointF(-8.0, 5.0));
    Check(canvas.panCount == 1 && canvas.lastPanDelta == QPointF(-8.0, 5.0), "native pan applies the OS delta once");

    SendNativeGesture(canvas, Qt::EndNativeGesture);
    Check(canvas.endCount == 1 && canvas.cursorOffCount == 1, "native gesture ends and releases the cursor");
    Check(canvas.nativeGestureEvents == 0, "begin/zoom/pan/end are all consumed; nothing reaches the widget");

    // Rotate/SmartZoom are not ours: the widget still receives them.
    SendNativeGesture(canvas, Qt::RotateNativeGesture, QPointF(), 15.0);
    Check(canvas.nativeGestureEvents == 1, "non-pinch native gestures pass through to the widget");
}

// 4. Gestures routed to the sidebar stay with the widget.
void SidebarGesturePassesThrough() {
    TestCanvas canvas;
    canvas.route = cnote::ui::PrecisionInputRoute::PassThrough;
    cnote::ui::PrecisionInputFilterQt filter(canvas);
    canvas.installEventFilter(&filter);

    SendNativeGesture(canvas, Qt::BeginNativeGesture);
    SendNativeGesture(canvas, Qt::ZoomNativeGesture, QPointF(), 0.5);
    SendNativeGesture(canvas, Qt::PanNativeGesture, QPointF(4.0, 4.0));
    SendNativeGesture(canvas, Qt::EndNativeGesture);

    Check(canvas.beginCount == 0 && canvas.zoomCount == 0 && canvas.panCount == 0 && canvas.endCount == 0,
        "sidebar-routed gestures never reach the canvas sink");
    Check(canvas.nativeGestureEvents == 4, "sidebar-routed native gestures all pass through to the widget");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    PlainInputUnchanged();
    DuplicateSuppression();
    NativePinchAndPan();
    SidebarGesturePassesThrough();

    std::printf(g_failures == 0 ? "ALL PRECISION FILTER CHECKS PASSED\n" : "%d PRECISION FILTER CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
