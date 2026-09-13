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
    std::vector<Call> calls;

    [[nodiscard]] cnote::ui::PrecisionInputRoute RoutePrecisionInput(const QPoint&) const override {
        return route;
    }
    void BeginPrecisionGesture() override { calls.push_back({ "begin" }); }
    void SetPrecisionGestureCursor(bool active) override { calls.push_back({ active ? "cursor-on" : "cursor-off" }); }
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
    Check(sink.Count("begin") == 1U && sink.Count("cursor-on") == 1U, "accepted gesture begins on the canvas");
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

    router.NoteNativeFrameConsumed();
    Check(router.ShouldDropWheelDuplicate(), "a consumed native frame drops the Qt duplicate wheel");
    Check(router.SuppressingDuplicates(), "suppression is reported while the gesture runs");

    Check(router.HandleNativeGestureBegin(QPoint(20, 20)), "native gesture begin is consumed");
    router.NoteNativeFrameConsumed();
    Check(router.ShouldDropWheelDuplicate(), "duplicate suppression persists during the gesture");
    Check(router.HandleNativeGestureEnd(), "native gesture end is consumed");
    Check(!router.ShouldDropWheelDuplicate(), "suppression ends with the gesture");
    Check(sink.Count("cursor-off") == 1U, "cursor hint is released at the end");

    // Cancel clears suppression and inertia too.
    sink.calls.clear();
    static_cast<void>(router.HandleNativeGestureBegin(QPoint(20, 20)));
    router.NoteNativeFrameConsumed();
    router.SetInertiaActive(true);
    router.HandleTouchpadSample(Sample(cnote::ui::TouchpadGestureSample::Phase::Cancel));
    Check(router.InertiaActive() == false, "cancel stops inertia");
    Check(!router.ShouldDropWheelDuplicate(), "cancel clears duplicate suppression");
    Check(sink.Count("end") == 1U && sink.Count("cursor-off") == 1U, "cancel finishes the sink gesture once");
    Check(!router.GestureActive(), "cancel ends the gesture");
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

// 5. Thresholds, cursor anchoring and the native pinch accumulation.
void ThresholdsAndAnchoring() {
    FakeSink sink;
    cnote::ui::PrecisionInputRouter router(sink);
    router.SetNativeAdapterAvailable(true);

    static_cast<void>(router.HandleNativeGestureBegin(QPoint(100, 200)));
    Check(sink.calls.size() == 2U && sink.calls[0].kind == "begin" && sink.calls[1].kind == "cursor-on",
        "begin reports the canvas gesture and the cursor hint");

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
    Check(router.HandleTouchBegin(QPoint(300, 300)), "touch gesture begins on the canvas");
    Check(router.HandleTouchUpdate(QPoint(310, 300), 1.5), "touch gesture update is consumed");
    Check(sink.Count("zoom") == 1U && sink.calls.back().a == 1.5, "touch pinch applies the cumulative scale");
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

}  // namespace

int main() {
    AvailabilityFallback();
    RoutingExcludesSidebar();
    ConsumeAndSuppressDuplicates();
    InertiaLifecycle();
    ThresholdsAndAnchoring();

    std::printf(g_failures == 0 ? "ALL PRECISION ROUTER CHECKS PASSED\n" : "%d PRECISION ROUTER CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
