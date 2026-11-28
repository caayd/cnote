// Portable precision-input decision-core test (migration plan steps 5.3/5.4).
//
// The Windows precision-touchpad adapter and Qt's native gestures both funnel
// through cnote::ui::PrecisionInputRouter, so the routing, duplicate
// suppression, availability fallback and inertia rules are pinned here without
// any hardware.  The Windows adapter itself and real device input are covered
// separately (precision_touchpad_qt) and remain physically unverified.

#include <QPoint>
#include <QPointF>
#include <QString>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "ui/PrecisionInputRouter.h"
#include "ui/PrecisionInputSink.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

struct Call {
    std::string kind;
    qreal a = 0.0;
    qreal b = 0.0;
    QPoint anchor {};
};

struct FakeSink final : cnote::ui::PrecisionInputSink {
    cnote::ui::PrecisionInputRoute route = cnote::ui::PrecisionInputRoute::Canvas;
    mutable int routeProbes = 0;
    std::vector<Call> calls;

    [[nodiscard]] cnote::ui::PrecisionInputRoute RoutePrecisionInput(const QPoint&) const override {
        ++routeProbes;
        return route;
    }
    void BeginPrecisionGesture() override { calls.push_back({ "begin" }); }
    void ZoomAtAnchor(qreal cumulativeScale, const QPoint& globalAnchor) override {
        calls.push_back({ "zoom", cumulativeScale, 0.0, globalAnchor });
    }
    void PanBy(qreal deltaX, qreal deltaY) override { calls.push_back({ "pan", deltaX, deltaY }); }
    void EndPrecisionGesture() override { calls.push_back({ "end" }); }

    [[nodiscard]] std::size_t Count(const char* kind) const {
        std::size_t count = 0;
        for (const Call& call : calls) {
            if (call.kind == kind) {
                ++count;
            }
        }
        return count;
    }
};

cnote::ui::TouchpadGestureSample Sample(
    cnote::ui::TouchpadGestureSample::Phase phase,
    const QPoint& anchor = QPoint(400, 300)) {
    cnote::ui::TouchpadGestureSample sample;
    sample.phase = phase;
    sample.screenAnchor = anchor;
    return sample;
}

// 1. Availability fallback: without the native adapter nothing is consumed and
//    the Qt wheel/gesture path stays in charge.
void AvailabilityFallback() {
    FakeSink sink;
    cnote::ui::PrecisionInputRouter router(sink);

    router.SetNativeAdapterAvailable(false);
    Check(!router.NativeAdapterAvailable(), "native adapter reports unavailable");

    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Begin));
    Check(sink.calls.empty(), "unavailable adapter ignores native touchpad frames");
    Check(!router.GestureActive(), "unavailable adapter starts no gesture");

    router.NoteNativeFrameConsumed();
    Check(!router.ShouldDropWheelDuplicate(), "unavailable adapter suppresses no wheel events");

    // Qt native gestures do not depend on the Windows adapter and keep working.
    Check(router.HandleNativeGestureBegin(QPoint(10, 10)), "native gesture still routes to the canvas");
    Check(router.HandleNativeGestureZoom(0.5, QPoint(10, 10)), "native pinch still applies");
    Check(sink.Count("zoom") == 1U, "native pinch reaches the sink");
    Check(router.HandleNativeGestureEnd(), "native gesture ends");
    Check(!router.InertiaActive(), "native gestures do not start inertia");
}

// 2. Canvas/sidebar routing, including the legacy re-consult on later frames.
void RoutingExcludesSidebar() {
    FakeSink sink;
    cnote::ui::PrecisionInputRouter router(sink);
    router.SetNativeAdapterAvailable(true);

    sink.route = cnote::ui::PrecisionInputRoute::PassThrough;
    Check(!router.HandleNativeGestureBegin(QPoint(5, 5)), "gesture over the sidebar is not consumed");
    Check(router.GestureIgnored(), "sidebar gesture is tracked as ignored");
    Check(sink.calls.empty(), "sidebar gesture touches no canvas state");
    Check(!router.HandleNativeGestureZoom(0.5, QPoint(5, 5)), "sidebar pinch is not consumed");
    Check(sink.Count("zoom") == 0U, "sidebar pinch does not zoom the canvas");

    // The legacy pointer path re-consults the route until the gesture is
    // accepted, so a gesture that moves onto the canvas is picked up.
    sink.route = cnote::ui::PrecisionInputRoute::Canvas;
    Check(router.HandleNativeGestureZoom(0.5, QPoint(600, 400)), "gesture accepted once it reaches the canvas");
    Check(sink.Count("begin") == 1U, "accepted gesture begins on the canvas");
    Check(sink.Count("zoom") == 1U, "accepted gesture applies the pinch");
    Check(router.HandleNativeGestureEnd(), "accepted gesture ends");

    sink.calls.clear();
    sink.route = cnote::ui::PrecisionInputRoute::PassThrough;
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Begin, QPoint(5, 5)));
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Update, QPoint(5, 5)));
    Check(sink.calls.empty(), "touchpad frames over the sidebar never reach the canvas");
    sink.route = cnote::ui::PrecisionInputRoute::Canvas;
    cnote::ui::TouchpadGestureSample update = Sample(cnote::ui::TouchpadGestureSample::Phase::Update, QPoint(700, 400));
    update.translationX = 12.0f;
    router.HandleTouchpadSample(update);
    Check(sink.Count("pan") == 1U, "touchpad frame accepted after moving onto the canvas");
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::End, QPoint(700, 400)));
    Check(sink.Count("end") == 1U, "accepted touchpad gesture ends once");
}

// 3. Consuming versus passing through, and duplicate-event suppression.
void ConsumeAndSuppressDuplicates() {
    FakeSink sink;
    cnote::ui::PrecisionInputRouter router(sink);
    router.SetNativeAdapterAvailable(true);

    // A consumed frame with no live gesture has no duplicate to suppress: the
    // trailing POINTERUP must never drop real wheel input.
    router.NoteNativeFrameConsumed();
    Check(!router.ShouldDropWheelDuplicate(), "a consumed frame without a live gesture suppresses nothing");
    Check(!router.SuppressingDuplicates(), "suppression is not reported outside a gesture");

    // The native adapter owns the canvas gesture; Qt's copies of the same
    // physical input are consumed without ever reaching the sink twice.
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Begin));
    Check(sink.Count("begin") == 1U, "native touchpad begin reaches the canvas once");
    router.NoteNativeFrameConsumed();
    Check(router.ShouldDropWheelDuplicate(), "a consumed native frame drops the Qt duplicate wheel");
    Check(router.SuppressingDuplicates(), "suppression is reported while the gesture is live");

    Check(router.HandleNativeGestureBegin(QPoint(20, 20)), "the duplicate native begin is consumed");
    Check(sink.Count("begin") == 1U, "the duplicate begin never emits a second BeginPrecisionGesture");
    Check(router.HandleNativeGesturePan(QPointF(30.0, -20.0), QPoint(20, 20)), "the duplicate native pan is consumed");
    Check(sink.Count("pan") == 0U, "the duplicate pan never reaches the canvas");
    router.NoteNativeFrameConsumed();
    Check(router.HandleNativeGestureEnd(), "the duplicate native end is consumed");
    Check(router.GestureActive() && router.Source() == cnote::ui::PrecisionInputRouter::GestureSource::NativeTouchpad,
        "Qt's duplicate end does not finish the native touchpad gesture");
    Check(!router.ShouldDropWheelDuplicate(), "the duplicate end clears suppression");
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::End));
    Check(sink.Count("end") == 1U, "the native touchpad end finishes the sink gesture once");
    Check(!router.GestureActive(), "the touchpad gesture is idle again");

    // Immediately after the gesture ended, wheel events must pass through:
    // even a consumed trailing frame does not re-arm suppression without a
    // live gesture.
    router.NoteNativeFrameConsumed();
    Check(!router.ShouldDropWheelDuplicate(), "wheel events pass immediately after the gesture ends");
    Check(!router.SuppressingDuplicates(), "no suppression outlives the gesture");

    // A genuine begin arriving while idle is not treated as a duplicate.
    Check(router.HandleNativeGestureBegin(QPoint(20, 20)), "a genuine begin starts a new gesture");
    Check(sink.Count("begin") == 2U, "the genuine begin reaches the canvas");
    Check(router.HandleNativeGestureEnd(), "the genuine gesture ends");

    // Cancel clears suppression and inertia too.
    sink.calls.clear();
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Begin));
    router.NoteNativeFrameConsumed();
    router.SetInertiaActive(true);
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Cancel));
    Check(router.InertiaActive() == false, "cancel stops inertia");
    Check(!router.ShouldDropWheelDuplicate(), "cancel clears duplicate suppression");
    Check(sink.Count("end") == 1U, "cancel finishes the sink gesture once");
    Check(!router.GestureActive(), "cancel ends the gesture");

    // Inertia ending clears suppression so the first real wheel event after a
    // flick is never dropped.
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Begin));
    router.NoteNativeFrameConsumed();
    router.SetInertiaActive(true);
    Check(router.ShouldDropWheelDuplicate(), "inertia frames keep suppressing duplicates while the gesture is live");
    router.SetInertiaActive(false);
    Check(!router.ShouldDropWheelDuplicate(), "inertia end clears suppression");
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Cancel));
}

// 4. Inertia start/stop and inertia panning after the manipulation ends.
void InertiaLifecycle() {
    FakeSink sink;
    cnote::ui::PrecisionInputRouter router(sink);
    router.SetNativeAdapterAvailable(true);

    cnote::ui::TouchpadGestureSample begin = Sample(cnote::ui::TouchpadGestureSample::Phase::Begin);
    router.HandleTouchpadSample(begin);
    Check(!router.InertiaActive(), "gesture starts without inertia");

    cnote::ui::TouchpadGestureSample update = Sample(cnote::ui::TouchpadGestureSample::Phase::Update);
    update.inertiaActive = true;
    router.HandleTouchpadSample(update);
    Check(router.InertiaActive(), "inertia frame starts inertia");

    update.inertiaActive = false;
    router.HandleTouchpadSample(update);
    Check(!router.InertiaActive(), "plain frame stops inertia");

    update.inertiaActive = true;
    router.HandleTouchpadSample(update);
    Check(router.InertiaActive(), "inertia restarts on the next inertia frame");

    cnote::ui::TouchpadGestureSample end = Sample(cnote::ui::TouchpadGestureSample::Phase::End);
    end.inertiaActive = true;
    router.HandleTouchpadSample(end);
    Check(!router.GestureActive(), "manipulation ended");
    Check(router.InertiaActive(), "inertia keeps running after the manipulation ends");

    const std::size_t pansBefore = sink.Count("pan");
    cnote::ui::TouchpadGestureSample coast = Sample(cnote::ui::TouchpadGestureSample::Phase::Update);
    coast.inertiaActive = true;
    coast.translationX = 9.0f;
    coast.translationY = -3.0f;
    router.HandleTouchpadSample(coast);
    Check(sink.Count("pan") == pansBefore + 1U, "inertia frames keep panning after the gesture ends");

    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Cancel));
    Check(!router.InertiaActive(), "cancel stops inertia panning");

    // An inertia frame with no active gesture and no inertia flag is ignored.
    sink.calls.clear();
    cnote::ui::TouchpadGestureSample stray = Sample(cnote::ui::TouchpadGestureSample::Phase::Update);
    router.HandleTouchpadSample(stray);
    Check(sink.calls.empty(), "stray frames outside a gesture are ignored");
}

// 5. Touch owns its own lifecycle: native begin/update cannot steal it and a
//    TouchCancel leaves the router clean for the next gesture.
void TouchLifecycleIsolation() {
    FakeSink sink;
    cnote::ui::PrecisionInputRouter router(sink);
    router.SetNativeAdapterAvailable(true);

    const cnote::ui::PrecisionInputRouter::TouchMetrics start { QPointF(300.0, 300.0), 100.0 };
    Check(router.HandleTouchBegin(start), "touch gesture begins on the canvas");
    Check(router.Source() == cnote::ui::PrecisionInputRouter::GestureSource::Touch, "touch owns the gesture");
    Check(sink.Count("begin") == 1U, "touch begin reaches the canvas once");

    // A native gesture begin while touch is live is not a second gesture and
    // must not emit BeginPrecisionGesture again.
    Check(!router.HandleNativeGestureBegin(QPoint(400, 300)), "native begin during touch is rejected");
    Check(sink.Count("begin") == 1U, "the rejected native begin emits no second BeginPrecisionGesture");
    Check(router.Source() == cnote::ui::PrecisionInputRouter::GestureSource::Touch, "touch keeps ownership");

    // A native touchpad frame must not finish or hijack the Qt touch gesture.
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Update, QPoint(400, 300)));
    Check(router.GestureActive() && router.Source() == cnote::ui::PrecisionInputRouter::GestureSource::Touch,
        "a touchpad update cannot hijack a touch gesture");
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::End, QPoint(400, 300)));
    Check(router.Source() == cnote::ui::PrecisionInputRouter::GestureSource::Touch,
        "a touchpad end cannot finish a touch gesture");

    // TouchCancel is an End: the router goes fully idle and the next begin is
    // accepted rather than stuck behind touchActive_.
    Check(router.HandleTouchCancel(), "touch cancel is consumed");
    Check(router.Phase() == cnote::ui::PrecisionInputRouter::GesturePhase::Idle, "touch cancel leaves the router idle");
    Check(router.Source() == cnote::ui::PrecisionInputRouter::GestureSource::None, "touch cancel clears the source");
    Check(!router.HandleTouchCancel(), "a cancel with no touch gesture is not consumed");

    Check(router.HandleTouchBegin(start), "the next touch begin after cancel is accepted");
    Check(sink.Count("begin") == 2U, "the next touch begin starts exactly one canvas gesture");
    Check(router.HandleTouchEnd(), "the next touch gesture ends");

    // Native End and Cancel must not finish a native-gesture-owned gesture.
    Check(router.HandleNativeGestureBegin(QPoint(500, 400)), "native gesture begins after the touch path is idle");
    Check(sink.Count("begin") == 3U, "the native begin starts one canvas gesture");
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::End));
    Check(router.GestureActive() && router.Source() == cnote::ui::PrecisionInputRouter::GestureSource::NativeGesture,
        "a touchpad end cannot finish a Qt native gesture");
    Check(router.HandleNativeGestureEnd(), "the native gesture still ends through its own path");
    Check(!router.GestureActive(), "the native gesture ends cleanly");
}

// 6. Thresholds, cursor anchoring and the native pinch accumulation.
void ThresholdsAndAnchoring() {
    FakeSink sink;
    cnote::ui::PrecisionInputRouter router(sink);
    router.SetNativeAdapterAvailable(true);

    static_cast<void>(router.HandleNativeGestureBegin(QPoint(100, 200)));
    Check(sink.calls.size() == 1U && sink.calls[0].kind == "begin",
        "begin reports the canvas gesture without a cursor hint");

    // Below the legacy epsilon nothing is applied.
    static_cast<void>(router.HandleNativeGestureZoom(cnote::ui::PrecisionInputRouter::kScaleEpsilon / 2.0, QPoint(100, 200)));
    Check(sink.Count("zoom") == 0U, "sub-epsilon pinch is ignored");
    static_cast<void>(router.HandleNativeGesturePan(QPointF(0.0002, -0.0002), QPoint(100, 200)));
    Check(sink.Count("pan") == 0U, "sub-epsilon pan is ignored");

    static_cast<void>(router.HandleNativeGestureZoom(0.25, QPoint(100, 200)));
    Check(sink.Count("zoom") == 1U && std::abs(sink.calls.back().a - 1.25) < 1e-9, "pinch accumulates (1 + value)");
    Check(sink.calls.back().anchor == QPoint(100, 200), "zoom stays anchored at the cursor");

    static_cast<void>(router.HandleNativeGestureZoom(0.25, QPoint(140, 240)));
    Check(std::abs(sink.calls.back().a - 1.5625) < 1e-9, "pinch accumulates over the gesture");

    static_cast<void>(router.HandleNativeGesturePan(QPointF(-12.0, 7.5), QPoint(140, 240)));
    Check(sink.Count("pan") == 1U && sink.calls.back().a == -12.0 && sink.calls.back().b == 7.5,
        "pan deltas are applied directly without extra smoothing");

    Check(router.HandleNativeGestureEnd(), "gesture ends");
    Check(!router.HandleNativeGestureEnd(), "a second end is not consumed");

    // Touch metrics: centroid and two-finger distance.
    const cnote::ui::PrecisionInputRouter::TouchMetrics metrics =
        cnote::ui::PrecisionInputRouter::ComputeTouchMetrics({ QPointF(100.0, 100.0), QPointF(100.0, 200.0) });
    Check(metrics.centroid == QPointF(100.0, 150.0), "touch centroid is the average position");
    Check(std::abs(metrics.distance - 100.0) < 1e-9, "touch distance is the two-finger separation");
    const cnote::ui::PrecisionInputRouter::TouchMetrics empty =
        cnote::ui::PrecisionInputRouter::ComputeTouchMetrics({});
    Check(empty.distance == 0.0, "an empty touch list reports zero distance");

    // Two-finger touch pinch/pan through the router.
    sink.calls.clear();
    Check(router.HandleTouchBegin(cnote::ui::PrecisionInputRouter::TouchMetrics { QPointF(300.0, 300.0), 100.0 }),
        "touch gesture begins on the canvas");
    Check(router.HandleTouchUpdate(cnote::ui::PrecisionInputRouter::TouchMetrics { QPointF(310.0, 300.0), 150.0 }),
        "touch gesture update is consumed");
    Check(sink.Count("zoom") == 1U && sink.calls.back().a == 1.5, "touch pinch derives the cumulative scale from the start distance");
    qreal panX = 0.0;
    for (const Call& call : sink.calls) {
        if (call.kind == "pan") {
            panX = call.a;
        }
    }
    Check(sink.Count("pan") == 1U && panX == 10.0, "touch pan follows the centroid delta");
    Check(router.HandleTouchEnd(), "touch gesture ends");
    Check(!router.HandleTouchEnd(), "a second touch end is not consumed");
}

// 7. One coast resolves its canvas route once instead of per frame.
void InertiaRouteIsCached() {
    FakeSink sink;
    cnote::ui::PrecisionInputRouter router(sink);
    router.SetNativeAdapterAvailable(true);

    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Begin));
    cnote::ui::TouchpadGestureSample end = Sample(cnote::ui::TouchpadGestureSample::Phase::End);
    end.inertiaActive = true;
    router.HandleTouchpadSample(end);

    const int probesAtCoastStart = sink.routeProbes;
    const std::size_t pansAtCoastStart = sink.Count("pan");
    for (int frame = 0; frame < 8; ++frame) {
        cnote::ui::TouchpadGestureSample coast = Sample(cnote::ui::TouchpadGestureSample::Phase::Update);
        coast.inertiaActive = true;
        coast.translationX = 4.0f;
        coast.translationY = -2.0f;
        router.HandleTouchpadSample(coast);
    }
    Check(sink.routeProbes == probesAtCoastStart + 1, "one route probe covers a whole inertia run");
    Check(sink.Count("pan") == pansAtCoastStart + 8U, "every inertia frame still pans");

    // A route that rejects the anchor is cached for the run too: no frame
    // starts panning and the hit test is not repeated.
    router.SetInertiaActive(false);
    sink.route = cnote::ui::PrecisionInputRoute::PassThrough;
    const QPoint sidebarAnchor(5, 5);
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Begin, sidebarAnchor));
    end = Sample(cnote::ui::TouchpadGestureSample::Phase::End, sidebarAnchor);
    end.inertiaActive = true;
    router.HandleTouchpadSample(end);
    const int probesAtPassThrough = sink.routeProbes;
    const std::size_t pansAtPassThrough = sink.Count("pan");
    for (int frame = 0; frame < 4; ++frame) {
        cnote::ui::TouchpadGestureSample coast =
            Sample(cnote::ui::TouchpadGestureSample::Phase::Update, sidebarAnchor);
        coast.inertiaActive = true;
        coast.translationX = 4.0f;
        router.HandleTouchpadSample(coast);
    }
    Check(sink.routeProbes == probesAtPassThrough + 1, "a rejected inertia anchor is probed once for the run");
    Check(sink.Count("pan") == pansAtPassThrough, "an off-canvas inertia run pans nothing");
}

// 8. A pinch that returns to its base scale applies that final frame, and
//    frames that repeat the applied scale stay no-ops.  Comparing the zoom to
//    1.0 instead of the applied scale swallowed the return frame, leaving the
//    canvas at the last intermediate scale (PERF-014).
void PinchReturnsToBaseScale() {
    FakeSink sink;
    cnote::ui::PrecisionInputRouter router(sink);
    router.SetNativeAdapterAvailable(true);

    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Begin));
    cnote::ui::TouchpadGestureSample frame = Sample(cnote::ui::TouchpadGestureSample::Phase::Update);
    frame.cumulativeScale = 1.5f;
    router.HandleTouchpadSample(frame);
    Check(sink.Count("zoom") == 1U && std::abs(sink.calls.back().a - 1.5) < 1e-9,
        "the pinch applies its first accumulated scale");

    const std::size_t zoomsAfterFirstFrame = sink.Count("zoom");
    router.HandleTouchpadSample(frame);
    Check(sink.Count("zoom") == zoomsAfterFirstFrame, "repeating the applied scale zooms nothing");

    frame.cumulativeScale = 1.0f;
    router.HandleTouchpadSample(frame);
    Check(sink.Count("zoom") == zoomsAfterFirstFrame + 1U && std::abs(sink.calls.back().a - 1.0) < 1e-9,
        "returning to the base scale reaches the sink");
    Check(std::abs(router.CumulativeGestureScale() - 1.0) < 1e-9,
        "the applied scale is tracked for the gesture's next frame");

    frame.cumulativeScale = 1.0001f;
    router.HandleTouchpadSample(frame);
    Check(sink.Count("zoom") == zoomsAfterFirstFrame + 1U,
        "a frame inside kScaleEpsilon of the applied scale stays a no-op");

    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::End));
}

}  // namespace

int main() {
    AvailabilityFallback();
    RoutingExcludesSidebar();
    ConsumeAndSuppressDuplicates();
    InertiaLifecycle();
    TouchLifecycleIsolation();
    ThresholdsAndAnchoring();
    InertiaRouteIsCached();
    PinchReturnsToBaseScale();

    std::printf(g_failures == 0 ? "ALL PRECISION ROUTER CHECKS PASSED\n" : "%d PRECISION ROUTER CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
